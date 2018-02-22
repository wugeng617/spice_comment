/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "common/marshaller.h"
#include "common/generated_server_marshallers.h"

#include "spice.h"
#include "red_common.h"
#include "main_channel.h"
#include "reds.h"
#include "red_dispatcher.h"
#include "snd_worker.h"
#include "common/snd_codec.h"
#include "demarshallers.h"

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#define SND_RECEIVE_BUF_SIZE     (16 * 1024 * 2) //接收32K样本
#define RECORD_SAMPLES_SIZE (SND_RECEIVE_BUF_SIZE >> 2) //共128KB的缓冲区

enum PlaybackCommand {
    SND_PLAYBACK_MIGRATE, //迁移
    SND_PLAYBACK_MODE, //播放模式，RAW | CELT | OPUS
    SND_PLAYBACK_CTRL, //控制命令
    SND_PLAYBACK_PCM,  //PCM数据
    SND_PLAYBACK_VOLUME, //音量命令
    SND_PLAYBACK_LATENCY, //延时命令
};

enum RecordCommand {
    SND_RECORD_MIGRATE, // 迁移
    SND_RECORD_CTRL, // 控制
    SND_RECORD_VOLUME, // 音量设置
};

#define SND_PLAYBACK_MIGRATE_MASK (1 << SND_PLAYBACK_MIGRATE)
#define SND_PLAYBACK_MODE_MASK (1 << SND_PLAYBACK_MODE)
#define SND_PLAYBACK_CTRL_MASK (1 << SND_PLAYBACK_CTRL)
#define SND_PLAYBACK_PCM_MASK (1 << SND_PLAYBACK_PCM)
#define SND_PLAYBACK_VOLUME_MASK (1 << SND_PLAYBACK_VOLUME)
#define SND_PLAYBACK_LATENCY_MASK ( 1 << SND_PLAYBACK_LATENCY)

#define SND_RECORD_MIGRATE_MASK (1 << SND_RECORD_MIGRATE)
#define SND_RECORD_CTRL_MASK (1 << SND_RECORD_CTRL)
#define SND_RECORD_VOLUME_MASK (1 << SND_RECORD_VOLUME)

typedef struct SndChannel SndChannel;
typedef void (*snd_channel_send_messages_proc)(void *in_channel); //SndChannel消息发送
//SndChannel消息分发函数
typedef int (*snd_channel_handle_message_proc)(SndChannel *channel, size_t size, uint32_t type, void *message); 
typedef void (*snd_channel_on_message_done_proc)(SndChannel *channel); //SndChannel消息发送完毕函数
typedef void (*snd_channel_cleanup_channel_proc)(SndChannel *channel); //SndChannel清理函数

typedef struct SndWorker SndWorker;

/* 注意，SndChannel不是RedChannel的派生类，不想显示和光标通道的设计 
 * SndChannel主要负责声音数据的调制和解调，并且作为PlaybackChannel和RecordChannel的父类
 */
struct SndChannel {
    RedsStream *stream; //网络IO流
    SndWorker *worker; //SndWorker
    spice_parse_channel_func_t parser; //消息头解析和分发函数
    int refs; //引用计数，RedChannel里也有

    RedChannelClient *channel_client; /* 
    	RCC，因为音频回放和捕获只支持一个客户端，所以只需一个指针即可*/

    int active; //本地活动状态，表示本通道是否已经执行过start命令，
    int client_active; //客户端活动状态，表示是否给RCC客户端发送过了start命令
    int blocked; //发送消息是否阻塞（发生EAGAIN）

    uint32_t command; //命令位图
    uint32_t ack_generation; //ack代特征值
    uint32_t client_ack_generation;//客户端ack代数特征值
    uint32_t out_messages; //发出消息数量，不一定发送成功了
    uint32_t ack_messages; //ack数量

    struct {
        uint64_t serial; //消息序列号
        SpiceMarshaller *marshaller; //数据调制器
        uint32_t size; //数据大小
        uint32_t pos; //当前发送位置
    } send_data; //发送数据的结构体

    struct {
        uint8_t buf[SND_RECEIVE_BUF_SIZE]; //声音接收缓冲区
        uint8_t *message_start; //消息起始位置
        uint8_t *now; //当前已填充数据的末尾
        uint8_t *end; //消息末尾位置
    } receive_data; //接收数据结构体

    snd_channel_send_messages_proc send_messages; //消息发送函数
    snd_channel_handle_message_proc handle_message; //消息分发函数
    snd_channel_on_message_done_proc on_message_done; //消息发送完成函数
    snd_channel_cleanup_channel_proc cleanup; //SndChannel清理函数
};

typedef struct PlaybackChannel PlaybackChannel;

typedef struct AudioFrame AudioFrame;
struct AudioFrame {
    uint32_t time;
    uint32_t samples[SND_CODEC_MAX_FRAME_SIZE];
    PlaybackChannel *channel;
    AudioFrame *next;
};

/*回放通道*/
struct PlaybackChannel {
    SndChannel base; //公共音频通道
    AudioFrame frames[3]; //3个预分配的样本缓冲区，一个为音频处理的一帧
    AudioFrame *free_frames; //空闲帧
    AudioFrame *in_progress; //正在处理帧
    AudioFrame *pending_frame; //待发送的帧
    uint32_t mode; //数据模式，RAW|CELT|OPUS
    uint32_t latency; //延时
    SndCodec codec; //编解码器，回放通道里只用到了编码
    uint8_t  encode_buf[SND_CODEC_MAX_COMPRESSED_BYTES]; 
	//编码缓冲区，编码后最大的尺寸，当前是480，CELT实际只用了47个字节
};

struct SndWorker {
    RedChannel *base_channel; //通用通道
    SndChannel *connection; //音频通道对象，注意音频通道不是RedChannel的子类，
    SndWorker *next; //SndWorker链表
    int active; /*活动标记，表示是否发送了start命令，spice音频作为一种软件模拟
    	的qemu音频硬件（想想是不是），无论spice客户端是否连接了，都会执行声卡的
    	start | stop | volume | data命令，所以需要有一个状态来记录用户是否已经
    	正在进行数据操作。这就是active的作用。如果spice客户端不连接时，就不进行
    	相应的控制和数据操作状态，那么当客户端中途连接过来时，就无法得到相应的
    	用户操作意图的音频表现（如静音、音量、音频数据的当前播放位置）*/
};

typedef struct SpiceVolumeState {
    uint8_t volume_nchannels;
    uint16_t *volume;
    int mute;
} SpiceVolumeState;

struct SpicePlaybackState {
    struct SndWorker worker; //SndWorkerVM启动就有，但是SndChannel和RedChannel不是vm启动就有
    SpicePlaybackInstance *sin;
    SpiceVolumeState volume;
    uint32_t frequency;
};

struct SpiceRecordState {
    struct SndWorker worker;
    SpiceRecordInstance *sin;
    SpiceVolumeState volume;
    uint32_t frequency;
};

typedef struct RecordChannel {
    SndChannel base;
    uint32_t samples[RECORD_SAMPLES_SIZE]; //存放收到的样本，一个样本4字节
    uint32_t write_pos; //samples写入的位置
    uint32_t read_pos; //qemu声卡当前读取样本的位置
    uint32_t mode; //收到的录音数据的模式，raw | opus |celt
    uint32_t mode_time; //记下客户端发送的mode时间
    uint32_t start_time; //开始时间
    SndCodec codec; //编解码器，录音通道里只用到了解码
    uint8_t  decode_buf[SND_CODEC_MAX_FRAME_BYTES]; //录音数据解码缓冲区，mode为raw时不用
} RecordChannel;

static SndWorker *workers;
static uint32_t playback_compression = TRUE;

static void snd_receive(void* data);

//引用
static SndChannel *snd_channel_get(SndChannel *channel)
{
    channel->refs++;
    return channel;
}

//解引用
static SndChannel *snd_channel_put(SndChannel *channel)
{
    if (!--channel->refs) {
        spice_printerr("SndChannel=%p freed", channel);
        free(channel);
        return NULL;
    }
    return channel;
}

static void snd_disconnect_channel(SndChannel *channel)
{
    SndWorker *worker;

    if (!channel || !channel->stream) {
        spice_debug("not connected");
        return;
    }
    spice_debug("SndChannel=%p rcc=%p type=%d", channel, 
    	channel->channel_client, channel->channel_client->channel->type);
    worker = channel->worker;

	//调用SndChannel清理函数
    channel->cleanup(channel);

	//断开RCC
    red_channel_client_disconnect(worker->connection->channel_client);
    worker->connection->channel_client = NULL;

	//取消socket事件监听
    core->watch_remove(channel->stream->watch);
    channel->stream->watch = NULL;

	//释放socket的RedStream
    reds_stream_free(channel->stream);
    channel->stream = NULL;

	//干掉调制解调器
    spice_marshaller_destroy(channel->send_data.marshaller);

	//释放引用计数
    snd_channel_put(channel);
    worker->connection = NULL;
}

static void snd_playback_free_frame(PlaybackChannel *playback_channel, AudioFrame *frame)
{
    frame->channel = playback_channel;
    frame->next = playback_channel->free_frames;
    playback_channel->free_frames = frame;
}

//回放消息发送完成函数
static void snd_playback_on_message_done(SndChannel *channel)
{
    PlaybackChannel *playback_channel = (PlaybackChannel *)channel;
    if (playback_channel->in_progress) { 
		//发送完成后，将正在发送的帧归还，因为所有消息都走这个流程，
		//所以只有PCM数据包走次流程
        snd_playback_free_frame(playback_channel, playback_channel->in_progress);
        playback_channel->in_progress = NULL;
        if (playback_channel->pending_frame) { 
			//如果还有帧待发送，添加发送命令
            channel->command |= SND_PLAYBACK_PCM_MASK;
        }
    }
}

// 录音通道消息发送完成函数，实现空
static void snd_record_on_message_done(SndChannel *channel)
{
}

static int snd_send_data(SndChannel *channel)
{
    uint32_t n; //n 记录待发送数据量

    if (!channel) {
        return FALSE;
    }

	// 没有数据需要发送，则直接返回
    if (!(n = channel->send_data.size - channel->send_data.pos)) {
        return TRUE;
    }

    for (;;) {
        struct iovec vec[IOV_MAX];
        int vec_size;

        if (!n) { //n 为0表示所有数据都发送完成
        	//调用消息发送完成函数，归还in_process帧
            channel->on_message_done(channel);

            if (channel->blocked) {//取消阻塞状态
                channel->blocked = FALSE;
				//更新事件监听
                core->watch_update_mask(channel->stream->watch, SPICE_WATCH_EVENT_READ);
            }
			//发送完成，退出发送流程
            break;
        }

		//从调制解调器中获取iovec，通过channel->send_data.pos参数跳过了已经发送的部分
        vec_size = spice_marshaller_fill_iovec(channel->send_data.marshaller,
                                               vec, IOV_MAX, channel->send_data.pos);
		//writev 写buffer到socket
		n = reds_stream_writev(channel->stream, vec, vec_size);
        if (n == -1) {
            switch (errno) {
            case EAGAIN:
				//socket的写入缓冲区用光了，记录一下阻塞状态
				//同时让qemu监听socket的可写事件，可写时执行snd_event继续发送数据
				//snd_event会掉用回到次函数
                channel->blocked = TRUE;
                core->watch_update_mask(channel->stream->watch, SPICE_WATCH_EVENT_READ |
                                        SPICE_WATCH_EVENT_WRITE);
                return FALSE;
            case EINTR:
                break;
            case EPIPE:
                snd_disconnect_channel(channel);
                return FALSE;
            default:
                spice_printerr("%s", strerror(errno));
                snd_disconnect_channel(channel);
                return FALSE;
            }
        } else {
        	//发送成功，更新已发送位置（非循环缓冲区）
            channel->send_data.pos += n;
        }
		//更新一下剩余数据量，发送失败的情况函数全部返回了
        n = channel->send_data.size - channel->send_data.pos;
    }
    return TRUE;
}

/**
 * snd_record_handle_write - 录音通道将收到的录音数据解码后写入到样本缓冲区中
 * record_channel 录音通道指针
 * size 消息大小
 * message 消息结构体指针
 */
static int snd_record_handle_write(
	RecordChannel *record_channel, 
	size_t size, 
	void *message)
{
    SpiceMsgcRecordPacket *packet;
    uint32_t write_pos;
    uint32_t* data;
    uint32_t len;
    uint32_t now;

    if (!record_channel) {
        return FALSE;
    }

    packet = (SpiceMsgcRecordPacket *)message;

    if (record_channel->mode == SPICE_AUDIO_DATA_MODE_RAW) {
		//原始模式下，数据
        data = (uint32_t *)packet->data;
        size = packet->data_size >> 2; //数据字节数转换成样本数量
        size = MIN(size, RECORD_SAMPLES_SIZE); //数据太大则截断
     } else {//压缩数据模式下，先解压缩
        int decode_size;
        decode_size = sizeof(record_channel->decode_buf);
        if (snd_codec_decode(record_channel->codec, packet->data, packet->data_size,
                    record_channel->decode_buf, &decode_size) != SND_CODEC_OK)
            return FALSE;//解码失败应该有统计或者日志
        data = (uint32_t *) record_channel->decode_buf;//设置样本缓冲区地址
        size = decode_size >> 2;//这样直接截断还是有丢数据的风险
    }

	//处理循环缓冲区的回绕
    write_pos = record_channel->write_pos % RECORD_SAMPLES_SIZE;
	//写完后的新的位置直接+size，此时的write_pos可能是溢出的，需要下次处理
	//录音数据时才会回绕。
    record_channel->write_pos += size; 

    len = RECORD_SAMPLES_SIZE - write_pos;//末尾到前一次写位置的距离
    now = MIN(len, size);//只写到末尾，或者全被写完
    size -= now;//去掉可写部分，size则为还要写入的部分
    memcpy(record_channel->samples + write_pos, data, now << 2);//copy样本

    if (size) {
        memcpy(record_channel->samples, data + now, size << 2);//写入剩余的样本
    }
	//注意，上面的写法要求，收到的数据解压后的样本数量<=RECORD_SAMPLES_SIZE，否则
	//会造成缓冲区溢出

	// 如果本次写入的数据造成之前未读取的数据被覆盖了，那么把覆盖掉的数据丢弃掉，
	// 只保留最近一个RECORD_SAMPLES_SIZE数量的样本数据
    if (record_channel->write_pos - record_channel->read_pos > RECORD_SAMPLES_SIZE) {
        record_channel->read_pos = record_channel->write_pos - RECORD_SAMPLES_SIZE;
    }
    return TRUE;
}

//回放通道消息分发函数，只处理
static int snd_playback_handle_message(SndChannel *channel, 
	size_t size, uint32_t type, void *message)
{
    if (!channel) {
        return FALSE;
    }

    switch (type) {
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        spice_printerr("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

/**
 * snd_record_handle_message - 录音通道分发消息
 * channel SndChannel指针
 * size 消息大小
 * type 消息类型
 * message 经过解析（反序列化）之后的消息结构体指针
**/
static int snd_record_handle_message(
	SndChannel *channel, size_t size, 
	uint32_t type, void *message)
{
    RecordChannel *record_channel = (RecordChannel *)channel;

    if (!channel) {
        return FALSE;
    }
	
    switch (type) {
    case SPICE_MSGC_RECORD_DATA://录音消息数据，解压后放在RecordChannel的缓冲区里
        return snd_record_handle_write((RecordChannel *)channel, size, message);
	
    case SPICE_MSGC_RECORD_MODE: {
		int ret = 0;
        SpiceMsgcRecordMode *mode = (SpiceMsgcRecordMode *)message;
        SpiceRecordState *st = 
			SPICE_CONTAINEROF(channel->worker, SpiceRecordState, worker);
		
		spice_info("received record mode message, "
			"time:%u, mode:%u, data:%p, data_size:%u",
			mode->time, mode->mode, mode->data, mode->data_size);

		// 更新mode设置时间，但是貌似没什么用
		record_channel->mode_time = mode->time;

		// 如果是RAW格式，直接记下数据模式，跳出处理
		if (mode->mode == SPICE_AUDIO_DATA_MODE_RAW) {
			record_channel->mode = mode->mode;
			break;
		}
		
        // 不是RAW格式，则必然是压缩格式
        // 如果编解码器不支持声卡的采样率，则返回失败
        if (!snd_codec_is_capable(mode->mode, st->frequency)) {
			spice_printerr("unsupported mode %d", record_channel->mode);
            return FALSE;
        }

		// 如果创建解码器失败，输出日志返回
		ret = snd_codec_create(&record_channel->codec, mode->mode, 
			st->frequency, SND_CODEC_DECODE);
		if (SND_CODEC_OK != ret) {
			spice_printerr("create decoder failed");
            return FALSE;
		}

		// 压缩格式下，创建解码器成功，解码器放在了record_channel的codec指针中
		// 记录下当前使用的音频格式
        record_channel->mode = mode->mode;                
        break;
    }

    case SPICE_MSGC_RECORD_START_MARK: {
        SpiceMsgcRecordStartMark *mark = (SpiceMsgcRecordStartMark *)message;
        record_channel->start_time = mark->time;
        break;
    }
    case SPICE_MSGC_DISCONNECTING:
        break;
    default:
        spice_printerr("invalid message type %u", type);
        return FALSE;
    }
    return TRUE;
}

/**
 * snd_receive  SndChannel数据接收函数，被PlaybackChannel和RecordChannel
 * 共同调用
**/
static void snd_receive(void* data)
{
    SndChannel *channel = (SndChannel*)data;
    SpiceDataHeaderOpaque *header;

    if (!channel) {
        return;
    }

	//默认情况下incoming.header的data指向incoming(IncomingHandle类型)的header_buf
	//在音频通道里，header->data需要指向整个收到数据的缓冲区的地址
    header = &channel->channel_client->incoming.header;

    for (;;) {
        ssize_t n;
		//计算剩余缓冲区大小，总是尝试读满缓冲区
        n = channel->receive_data.end - channel->receive_data.now;
        spice_assert(n);
        n = reds_stream_read(channel->stream, channel->receive_data.now, n);//n很可能被修改了
        // 读取消息时，有三种情况：
        // 1. 读取消息失败，一个字节都没读上来
        // 2. 读到了数据，但是读到的数据和缓冲区里的数据加到一起都不够一个消息
        // 3. 读到了数据，消息缓冲区里已经有的数据加上刚读到消息刚好或者超过一个消息
        if (n <= 0) {//读取成功
            if (n == 0) {//read返回0表示连接断开
                snd_disconnect_channel(channel);
                return;
            }
            spice_assert(n == -1);//返回<0时必然是-1
            switch (errno) {
            case EAGAIN://非阻塞模式下，没数据可读就会返回EAGAIN
            	//数据没有接收到，不读了，直接返回，SpiceWatch会监听socket的可读事件
            	//并再次调用本函数
                return;
            case EINTR:
				//被信号中断，执行循环即可，会马上读一下
                break;
            case EPIPE:
				//这个代码应该多余，read函数应该不会产生SIGPIPE错误
				//SIGPIPE只有在对读端已关闭的socket进行write操作时才会产生
				//因此这里应该不进来
                snd_disconnect_channel(channel);
                return;
            default:
				//默认错误，直接关闭通道
                spice_printerr("%s", strerror(errno));
                snd_disconnect_channel(channel);
                return;
            }
        } else {//读取成功，但是读满到buffer的末尾
            channel->receive_data.now += n;//修改当前已用缓冲区的位置的位置
            for (;;) {
                uint8_t *msg_start = channel->receive_data.message_start;//消息开始的位置
                uint8_t *data = msg_start + header->header_size;/* 数据指针，消息头部当前为6
                	此时计算得到的data数据可能还不正常，因为可能收到的数据不足6个字节，
                	那么data只是指向了buffer的位置，数据还没到位 */
                	
                size_t parsed_size; //存放消息解析后对象的尺寸
                uint8_t *parsed; //消息包解析成消息对象后的指针
                message_destructor_t parsed_free; //消息对象的释放函数指针

                header->data = msg_start;//修改spice消息头部结构的起始地址，指向消息起始地址，否则header解析非法
                n = channel->receive_data.now - msg_start; //当前缓冲区中已有的数据量

				// n < header->header_size成立时，
				// n < der->header_size + header->get_msg_size(header)不是一定成立吗？
				// 为什么不直接简化成第二个条件？如果直接用第二个条件，当只接受到一部分数据时
				// header的头部数据本身是不完整的，那么直接调用header->get_msg_size可能得到一个
				// 不可预知的值，如果header->get_msg_size的原型是返回int，那就造成收到部分数据就
				// 进行消息解析了。另外一个原因在于，判断第一个条件速度更快，对于头部还未收满的情况
				// 可以省略第二个条件中的判断，节省了get_msg_size的
				// 也就是说，只有整个消息都收到后才开始解析
				// 收到的数据
                if (n < header->header_size ||
                    n < header->header_size + header->get_msg_size(header)) {
                    break;
                }
				
				// 执行消息解析
                parsed = channel->parser((void *)data, data + header->get_msg_size(header),
                                         header->get_msg_type(header),
                                         SPICE_VERSION_MINOR, &parsed_size, &parsed_free);
                if (parsed == NULL) { //消息解析失败会断开通道连接
                    spice_printerr("failed to parse message type %d", header->get_msg_type(header));
                    snd_disconnect_channel(channel);
                    return;
                }

				//调用消息分发函数，回放通道调用snd_playback_handle_message
				//录音通道调用snd_record_handle_message，消息分发失败则断开通道
                if (!channel->handle_message(channel, parsed_size,
                                             header->get_msg_type(header), parsed)) {
                    parsed_free(parsed);//这个不是应该调用parsed_free吗
                    snd_disconnect_channel(channel);
                    return;
                }
				//释放解析出来的内存
                parsed_free(parsed);

				//设置好下一个消息的开始位置，这个时候message_start指向的地方可能已经读
				//上来部分数据，只是还没处理。
                channel->receive_data.message_start = msg_start + header->header_size +
                                                     header->get_msg_size(header);
            }
			//如果已经接收数据的位置正好在消息结束位置，则将下一个消息的开始位置
			//设置到buffer开始的位置。已经接收的数据位置也设置到buffer的起始位置
            if (channel->receive_data.now == channel->receive_data.message_start) {
                channel->receive_data.now = channel->receive_data.buf;
                channel->receive_data.message_start = channel->receive_data.buf;
            } else if (channel->receive_data.now == channel->receive_data.end) {
				//如果当前数据接收的位置不在消息边界处，那么有两种情况，
				//一种多接受的消息内容还不到缓冲区末尾。（520计算注定不会超过末尾），那么
				//此时receive_data.new到end还有一段可用的空间，直接进入下一个循环，接收
				//空闲空间的数据。还有一种情况是，读到的消息填充到缓冲区末尾，还没有接收完成
				//，这种情况下，需要将已经读到的消息复制到缓冲区开始的位置，调整消息开始的位置
				//为缓冲区开始，并且调整当前填充的位置为buf+n。
				//注意：这里使用memcpy应该有bug，一旦buf+n > message_start，则copy的位置，
				//内存重叠了，数据就会有问题
                memcpy(channel->receive_data.buf, channel->receive_data.message_start, n);
                channel->receive_data.now = channel->receive_data.buf + n;
                channel->receive_data.message_start = channel->receive_data.buf;
            }
        }
    }
}

/* RedStream的SpiceWatch中使用的事件处理函数，传入的数据是SndChannel结构，
   这个函数同时被PlaybackChannel和RecordChannel两者同时使用, 所以对于这两个
   通道来说，调用的send_messages不一样，playbackChannel调用的是snd_playback_send
   RecordChannel调用的则是snd_record_send
*/
static void snd_event(int fd, int event, void *data)
{
    SndChannel *channel = data;

    if (event & SPICE_WATCH_EVENT_READ) {
        snd_receive(channel);
    }
    if (event & SPICE_WATCH_EVENT_WRITE) {
        channel->send_messages(channel);
    }
}

/*重置SndChannel里的数据发送器*/
static inline int snd_reset_send_data(SndChannel *channel, uint16_t verb)
{
    SpiceDataHeaderOpaque *header;

    if (!channel) {
        return FALSE;
    }

    header = &channel->channel_client->send_data.header;//取RCC的头部结构

	//重置调制解调器
    spice_marshaller_reset(channel->send_data.marshaller);

	//再次在调制解调器中分配头部空间，header->data的内存在上面的spice_marshaller_reset
	//调用中已经被释放，所以这里可以直接赋值，不用担心内存泄漏
    header->data = spice_marshaller_reserve_space(channel->send_data.marshaller,
                                                  header->header_size);

	//设置基础数据位置
    spice_marshaller_set_base(channel->send_data.marshaller,
                              header->header_size);
    channel->send_data.pos = 0;//重置发送位置
    //消息大小设置为0，再后面所有消息填充后到marshaller后准备发送时
    //再设置
    header->set_msg_size(header, 0);
    header->set_msg_type(header, verb);//设置消息类型
    channel->send_data.serial++;//序列号增加
    if (!channel->channel_client->is_mini_header) {//目前没用到
        header->set_msg_serial(header, channel->send_data.serial);
        header->set_msg_sub_list(header, 0);
    }

    return TRUE;
}

static int snd_begin_send_message(SndChannel *channel)
{
    SpiceDataHeaderOpaque *header = &channel->channel_client->send_data.header;

	//flush marshaller以获取正确的数据和数据大小
    spice_marshaller_flush(channel->send_data.marshaller);
	
	//取得marshaller里所有数据的大小，包括头部的空间
	//读取位置在snd_reset_send_data里重置为0了
    channel->send_data.size = spice_marshaller_get_total_size(channel->send_data.marshaller);

	//设置消息头的消息体大小，不包括消息头
    header->set_msg_size(header, channel->send_data.size - header->header_size);

	//开始发送marshaller里的数据
    return snd_send_data(channel);
}

// 发送迁移消息
static int snd_channel_send_migrate(SndChannel *channel)
{
    SpiceMsgMigrate migrate;

	//重置消息头和消息调制器
    if (!snd_reset_send_data(channel, SPICE_MSG_MIGRATE)) {
        return FALSE;
    }
	
    spice_debug(NULL);
    migrate.flags = 0;

	//迁移消息调制
    spice_marshall_msg_migrate(channel->send_data.marshaller, &migrate);

    return snd_begin_send_message(channel);
}

//封装函数
static int snd_playback_send_migrate(PlaybackChannel *channel)
{
    return snd_channel_send_migrate(&channel->base);
}

//发送音频通道音量消息包，消息包含各个通道的音量
//音量的U16值表示什么意思呢
static int snd_send_volume(SndChannel *channel, SpiceVolumeState *st, int msg)
{
    SpiceMsgAudioVolume *vol;
    uint8_t c;

    vol = alloca(sizeof (SpiceMsgAudioVolume) +
                 st->volume_nchannels * sizeof (uint16_t));
    if (!snd_reset_send_data(channel, msg)) {
        return FALSE;
    }
    vol->nchannels = st->volume_nchannels;
    for (c = 0; c < st->volume_nchannels; ++c) {
        vol->volume[c] = st->volume[c];
    }
    spice_marshall_SpiceMsgAudioVolume(channel->send_data.marshaller, vol);

    return snd_begin_send_message(channel);
}

//回放发送音量
static int snd_playback_send_volume(PlaybackChannel *playback_channel)
{
    SndChannel *channel = &playback_channel->base;
    SpicePlaybackState *st = SPICE_CONTAINEROF(channel->worker, SpicePlaybackState, worker);

	//如果客户端不支持音量调节，则直接返回成功
    if (!red_channel_client_test_remote_cap(channel->channel_client,
                                            SPICE_PLAYBACK_CAP_VOLUME)) {
        return TRUE;
    }

    return snd_send_volume(channel, &st->volume, SPICE_MSG_PLAYBACK_VOLUME);
}

// 静音消息公共函数
static int snd_send_mute(SndChannel *channel, SpiceVolumeState *st, int msg)
{
    SpiceMsgAudioMute mute;

    if (!snd_reset_send_data(channel, msg)) {
        return FALSE;
    }
    mute.mute = st->mute;
    spice_marshall_SpiceMsgAudioMute(channel->send_data.marshaller, &mute);

    return snd_begin_send_message(channel);
}

// 回放通道发送静音，如果客户端不支持音量调节，也就不支持静音
static int snd_playback_send_mute(PlaybackChannel *playback_channel)
{
    SndChannel *channel = &playback_channel->base;
    SpicePlaybackState *st = SPICE_CONTAINEROF(channel->worker, SpicePlaybackState, worker);

    if (!red_channel_client_test_remote_cap(channel->channel_client,
                                            SPICE_PLAYBACK_CAP_VOLUME)) {
        return TRUE;
    }

    return snd_send_mute(channel, &st->volume, SPICE_MSG_PLAYBACK_MUTE);
}

// 发送延时消息
static int snd_playback_send_latency(PlaybackChannel *playback_channel)
{
    SndChannel *channel = &playback_channel->base;
    SpiceMsgPlaybackLatency latency_msg;

    spice_debug("latency %u", playback_channel->latency);
    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_LATENCY)) {
        return FALSE;
    }
    latency_msg.latency_ms = playback_channel->latency;
    spice_marshall_msg_playback_latency(channel->send_data.marshaller, &latency_msg);

    return snd_begin_send_message(channel);
}

//回放通道发送start消息，消息包括通道数、采样率、样本格式、开始时间
static int snd_playback_send_start(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    SpicePlaybackState *st = SPICE_CONTAINEROF(channel->worker, SpicePlaybackState, worker);
    SpiceMsgPlaybackStart start;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_START)) {
        return FALSE;
    }

    start.channels = SPICE_INTERFACE_PLAYBACK_CHAN; //spice只支持双通道
    start.frequency = st->frequency; //采样率
    spice_assert(SPICE_INTERFACE_PLAYBACK_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    start.time = reds_get_mm_time(); //开始时间去消息构建时
    spice_marshall_msg_playback_start(channel->send_data.marshaller, &start);

    return snd_begin_send_message(channel);
}

// 回放通道发送stop消息，stop消息数据部分为空
static int snd_playback_send_stop(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_STOP)) {
        return FALSE;
    }

    return snd_begin_send_message(channel);
}

// 回放通道发送控制消息，进入此流程则认为客户端与本地的client_active状态
// 一致了。如果SndChannel当前状态是活动，则发送start消息，否则stop消息
static int snd_playback_send_ctl(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
	/* 注意下面的条件是赋值，所以要取赋值后的值来判断，
	   PlaybackChannel里的client_active只在这里赋值 */
    if ((channel->client_active = channel->active)) {
        return snd_playback_send_start(playback_channel);
    } else {
        return snd_playback_send_stop(playback_channel);
    }
}

//录音通道发送start消息，相比playback的start消息，Record没有start时间
static int snd_record_send_start(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;
    SpiceRecordState *st = SPICE_CONTAINEROF(channel->worker, SpiceRecordState, worker);
    SpiceMsgRecordStart start;

    if (!snd_reset_send_data(channel, SPICE_MSG_RECORD_START)) {
        return FALSE;
    }

    start.channels = SPICE_INTERFACE_RECORD_CHAN;
    start.frequency = st->frequency;
    spice_assert(SPICE_INTERFACE_RECORD_FMT == SPICE_INTERFACE_AUDIO_FMT_S16);
    start.format = SPICE_AUDIO_FMT_S16;
    spice_marshall_msg_record_start(channel->send_data.marshaller, &start);

    return snd_begin_send_message(channel);
}

static int snd_record_send_stop(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;

    if (!snd_reset_send_data(channel, SPICE_MSG_RECORD_STOP)) {
        return FALSE;
    }

    return snd_begin_send_message(channel);
}

static int snd_record_send_ctl(RecordChannel *record_channel)
{
    SndChannel *channel = (SndChannel *)record_channel;

	/* 注意下面的条件是赋值，所以要取赋值后的值来判断，
	   Redcord里的client_active只在这里赋值 */
    if ((channel->client_active = channel->active)) {
        return snd_record_send_start(record_channel);
    } else {
        return snd_record_send_stop(record_channel);
    }
}

// 录音通道发送录音的音量，跟playback类似
static int snd_record_send_volume(RecordChannel *record_channel)
{
    SndChannel *channel = &record_channel->base;
    SpiceRecordState *st = SPICE_CONTAINEROF(channel->worker, SpiceRecordState, worker);

    if (!red_channel_client_test_remote_cap(channel->channel_client,
                                            SPICE_RECORD_CAP_VOLUME)) {
        return TRUE;
    }

    return snd_send_volume(channel, &st->volume, SPICE_MSG_RECORD_VOLUME);
}

static int snd_record_send_mute(RecordChannel *record_channel)
{
    SndChannel *channel = &record_channel->base;
    SpiceRecordState *st = SPICE_CONTAINEROF(channel->worker, SpiceRecordState, worker);

    if (!red_channel_client_test_remote_cap(channel->channel_client,
                                            SPICE_RECORD_CAP_VOLUME)) {
        return TRUE;
    }

    return snd_send_mute(channel, &st->volume, SPICE_MSG_RECORD_MUTE);
}

static int snd_record_send_migrate(RecordChannel *record_channel)
{
    /* No need for migration data: if recording has started before migration,
     * the client receives RECORD_STOP from the src before the migration completion
     * notification (when the vm is stopped).
     * Afterwards, when the vm starts on the dest, the client receives RECORD_START. */
    return snd_channel_send_migrate(&record_channel->base);
}

// 回放通道发送PCM数据，PCM数据放在playback通道的in_progress帧中
static int snd_playback_send_write(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    AudioFrame *frame;
    SpiceMsgPlaybackPacket msg;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_DATA)) {
        return FALSE;
    }


    frame = playback_channel->in_progress;
    msg.time = frame->time;

    spice_marshall_msg_playback_data(channel->send_data.marshaller, &msg);
	
	//in_progress帧缓冲区中的数据是PCM数据，所以如果要压缩，还要走压缩流程
    if (playback_channel->mode == SPICE_AUDIO_DATA_MODE_RAW) {
        spice_marshaller_add_ref(channel->send_data.marshaller,
                                 (uint8_t *)frame->samples,
                                 snd_codec_frame_size(playback_channel->codec) * sizeof(frame->samples[0]));
    }
    else { //压缩数据模式，压缩编码数据，压缩失败时断开连接
        int n = sizeof(playback_channel->encode_buf);
        if (snd_codec_encode(playback_channel->codec, (uint8_t *) frame->samples,
                                    snd_codec_frame_size(playback_channel->codec) * sizeof(frame->samples[0]),
                                    playback_channel->encode_buf, &n) != SND_CODEC_OK) {
            spice_printerr("encode failed");
            snd_disconnect_channel(channel);
            return FALSE;
        }
        spice_marshaller_add_ref(channel->send_data.marshaller, playback_channel->encode_buf, n);
    }

    return snd_begin_send_message(channel);
}

//发送回放的数据模式
static int playback_send_mode(PlaybackChannel *playback_channel)
{
    SndChannel *channel = (SndChannel *)playback_channel;
    SpiceMsgPlaybackMode mode;

    if (!snd_reset_send_data(channel, SPICE_MSG_PLAYBACK_MODE)) {
        return FALSE;
    }
	
    mode.time = reds_get_mm_time();
    mode.mode = playback_channel->mode;
    spice_marshall_msg_playback_mode(channel->send_data.marshaller, &mode);

    return snd_begin_send_message(channel);
}

static void snd_playback_send(void* data)
{
    PlaybackChannel *playback_channel = (PlaybackChannel*)data;
    SndChannel *channel = (SndChannel*)playback_channel;

    if (!playback_channel || !snd_send_data(data)) {
        return;
    }

    while (channel->command) {
        if (channel->command & SND_PLAYBACK_MODE_MASK) {
			// 命令要求发送数据模式，最先发送数据模式，由回放通道的mode值来发送
            if (!playback_send_mode(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_MODE_MASK;
        }
		// 发送回放PCM数据
        if (channel->command & SND_PLAYBACK_PCM_MASK) {
			//snd_send_data已经保证发送部分的帧已经被发送完成了，
			//所以in_progress必定为NULL
			//而且设置了PCM_MASK就必然要求pending_frame里有数据
            spice_assert(!playback_channel->in_progress && 
            	playback_channel->pending_frame);
			//将pending帧转交给in_progress帧
            playback_channel->in_progress = playback_channel->pending_frame;
            playback_channel->pending_frame = NULL;
            channel->command &= ~SND_PLAYBACK_PCM_MASK;
            if (!snd_playback_send_write(playback_channel)) {
                spice_printerr("snd_send_playback_write failed");
                return;
            }
        }
		// 回放控制命令，start和stop，由SndChannel的active值来决定
        if (channel->command & SND_PLAYBACK_CTRL_MASK) {
            if (!snd_playback_send_ctl(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_CTRL_MASK;
        }
		// 发送回放通道的音量信息，先发送音量，再发送静音信息
        if (channel->command & SND_PLAYBACK_VOLUME_MASK) {
            if (!snd_playback_send_volume(playback_channel) ||
                !snd_playback_send_mute(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_VOLUME_MASK;
        }
		// 迁移信息
        if (channel->command & SND_PLAYBACK_MIGRATE_MASK) {
            if (!snd_playback_send_migrate(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_MIGRATE_MASK;
        }
		// 延时信息
        if (channel->command & SND_PLAYBACK_LATENCY_MASK) {
            if (!snd_playback_send_latency(playback_channel)) {
                return;
            }
            channel->command &= ~SND_PLAYBACK_LATENCY_MASK;
        }
    }
}

static void snd_record_send(void* data)
{
    RecordChannel *record_channel = (RecordChannel*)data;
    SndChannel *channel = (SndChannel*)record_channel;

    if (!record_channel || !snd_send_data(data)) {
        return;
    }

    while (channel->command) {
		//控制信息，start和stop，具体是哪个，由SndChannel的active来决定
        if (channel->command & SND_RECORD_CTRL_MASK) {
            if (!snd_record_send_ctl(record_channel)) {
                return;
            }
            channel->command &= ~SND_RECORD_CTRL_MASK;
        }
		//音量信息
        if (channel->command & SND_RECORD_VOLUME_MASK) {
            if (!snd_record_send_volume(record_channel) ||
                !snd_record_send_mute(record_channel)) {
                return;
            }
            channel->command &= ~SND_RECORD_VOLUME_MASK;
        }
		//迁移信息
        if (channel->command & SND_RECORD_MIGRATE_MASK) {
            if (!snd_record_send_migrate(record_channel)) {
                return;
            }
            channel->command &= ~SND_RECORD_MIGRATE_MASK;
        }
    }
}


/**
 * __new_channel - 创建SndChannel，不会创建SndChannel这个抽象类，
 * 只会创建实际的PlaybackChannel或者RecordChannel
 * worker	SndWorker，通过它能找回RedChannel，并建立SndWorker和SndChannel之间的关联
 * size		派生类的实际大小，传入PlaybackChannel或者RecordChannel的实际大小
 * channel_id	通道类型，回放还是捕获
 * RedClient	客户端实例，用于建立RCC关联
 * stream	socket网络IO流
 * migrate	是否是在迁移
 * send_messages	消息发送钩子
 * handle_message	消息分发钩子
 * on_message_done  消息发送完成钩子
 * cleanup	通道清理钩子 
 * common_caps  	通用特性
 * num_common_caps  通用特性数量
 * caps		通道特性
 * num_caps		通道特性数量
 *
 * 主要的动作: 
 * 		1. 设置RedStream的socket属性，如优先级、TOS、TCP_NODELAY、O_NONBLOCK
 * 		2. 创建SndChannel对象，使用参数进行一些值设置，并设置一些回调函数
 *      3. 开始socket监听
 * 		4. 创建RCC并和SndChannel关联起来
**/
static SndChannel *__new_channel(SndWorker *worker, int size, 
	uint32_t channel_id, RedClient *client, RedsStream *stream, int migrate,
	snd_channel_send_messages_proc send_messages,
	snd_channel_handle_message_proc handle_message,
	snd_channel_on_message_done_proc on_message_done,
	snd_channel_cleanup_channel_proc cleanup,
	uint32_t *common_caps, int num_common_caps, uint32_t *caps, int num_caps)
{
    SndChannel *channel;
    int delay_val;
    int flags;
#ifdef SO_PRIORITY
    int priority;
#endif
    int tos;
    MainChannelClient *mcc = red_client_get_main(client);

	/* 1.设置socket的优先级、TOS、TCP_NODELAY、NONBLOCK属性 */
    spice_assert(stream);
    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        spice_printerr("accept failed, %s", strerror(errno));
        goto error1;
    }

#ifdef SO_PRIORITY
    priority = 6;
    if (setsockopt(stream->socket, SOL_SOCKET, SO_PRIORITY, (void*)&priority,
                   sizeof(priority)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }
#endif

    tos = IPTOS_LOWDELAY;
    if (setsockopt(stream->socket, IPPROTO_IP, IP_TOS, (void*)&tos, sizeof(tos)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }

    delay_val = main_channel_client_is_low_bandwidth(mcc) ? 0 : 1;
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val, sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            spice_printerr("setsockopt failed, %s", strerror(errno));
        }
    }

    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        spice_printerr("accept failed, %s", strerror(errno));
        goto error1;
    }

	/* 2. 创建SndChannel子类对象并初始化SndChannel成员 */
    spice_assert(size >= sizeof(*channel));
    channel = spice_malloc0(size);
    channel->refs = 1;
	//获取并设置消息解析函数
    channel->parser = spice_get_client_channel_parser(channel_id, NULL);
    channel->stream = stream;
    channel->worker = worker;
    channel->receive_data.message_start = channel->receive_data.buf;
    channel->receive_data.now = channel->receive_data.buf;
    channel->receive_data.end = channel->receive_data.buf + sizeof(channel->receive_data.buf);
    channel->send_data.marshaller = spice_marshaller_new();

	// 3. 给socket注册可读事件监听
    stream->watch = core->watch_add(stream->socket, SPICE_WATCH_EVENT_READ,
                                  snd_event, channel); //开始监听socket事件
    if (stream->watch == NULL) {
        spice_printerr("watch_add failed, %s", strerror(errno));
        goto error2;
    }

	/* 4. 设置通道回调函数 */
    channel->send_messages = send_messages;
    channel->handle_message = handle_message;
    channel->on_message_done = on_message_done;
    channel->cleanup = cleanup;

	/* 5. 创建RCC，使用caps来创建*/
    channel->channel_client = red_channel_client_create_dummy(
    	sizeof(RedChannelClient),
    	worker->base_channel,
    	client,
    	num_common_caps, common_caps,
    	num_caps, caps);
    if (!channel->channel_client) {
        goto error2;
    }
    return channel;

error2:
    free(channel);

error1:
    reds_stream_free(stream);
    return NULL;
}

// 客户端断开时的钩子函数
static void snd_disconnect_channel_client(RedChannelClient *rcc)
{
    SndWorker *worker;

	//RCC断开，但是RedChannel和RedWorker一直都在
    spice_assert(rcc->channel);
    spice_assert(rcc->channel->data);
    worker = (SndWorker *)rcc->channel->data;

    spice_debug("channel-type=%d", rcc->channel->type);
    if (worker->connection) {
		//SndChannel是否
        spice_assert(worker->connection->channel_client == rcc);
        snd_disconnect_channel(worker->connection);
    }
}

// 给音频通道添加命令，注意添加多个重复命令跟添加一次的效果一样
static void snd_set_command(SndChannel *channel, uint32_t command)
{
    if (!channel) {
        return;
    }
    channel->command |= command;
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_volume(SpicePlaybackInstance *sin,
                                                  uint8_t nchannels,
                                                  uint16_t *volume)
{
    SpiceVolumeState *st = &sin->st->volume;
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

	//在SpicePlaybackState里记录音量信息
    st->volume_nchannels = nchannels;

	//干掉之前的配置，生产新的音量配置
    free(st->volume);
    st->volume = spice_memdup(volume, sizeof(uint16_t) * nchannels);

	//如果通道没连接，或者没有通道则不用发送
    if (!channel || nchannels == 0)
        return;

	// 发送音量信息
    snd_playback_send_volume(playback_channel);
}

SPICE_GNUC_VISIBLE void spice_server_playback_set_mute(SpicePlaybackInstance *sin, uint8_t mute)
{
    SpiceVolumeState *st = &sin->st->volume;
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);

	//在SpicePlaybackState里记录静音信息
    st->mute = mute;

	//如果通道未建立则，直接返回
    if (!channel)
        return;

	//如果通道已经连接，则发送静音信息给客户端
    snd_playback_send_mute(playback_channel);
}

SPICE_GNUC_VISIBLE void spice_server_playback_start(SpicePlaybackInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = 
		SPICE_CONTAINEROF(channel, PlaybackChannel, base);

	// 发送回放start命令时，将SndWorker标记为活动
    sin->st->worker.active = 1;

	// 通道未连接则直接返回
    if (!channel)
        return;

	// 发送start命令，要求之前没有start过，重复start必定是bug
    spice_assert(!playback_channel->base.active);

	//reds的mm定时器是个什么东西？
    reds_disable_mm_timer();
    playback_channel->base.active = TRUE; //SndChannel设置激活

	//如果没有给客户端非活动，立即发送start命令
    if (!playback_channel->base.client_active) {		
        snd_set_command(&playback_channel->base, SND_PLAYBACK_CTRL_MASK);
        snd_playback_send(&playback_channel->base);
    } else {
		//如果客户端已经发送过了，就不发送了
		//什么情况下会走到这里？猜想应该是多客户端或者
		//迁移时可能走到这个逻辑
        playback_channel->base.command &= ~SND_PLAYBACK_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_stop(SpicePlaybackInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = 
		SPICE_CONTAINEROF(channel, PlaybackChannel, base);

	// 发送回放stop命令时，SndWork标记为非活动
    sin->st->worker.active = 0;

	// 通道未连接则直接返回
    if (!channel)
        return;
	
    spice_assert(playback_channel->base.active);
	
    reds_enable_mm_timer();
	
    playback_channel->base.active = FALSE; //本地回放设置为非活动

	//如果客户端还是活动的，立即发送stop命令
    if (playback_channel->base.client_active) {
        snd_set_command(&playback_channel->base, SND_PLAYBACK_CTRL_MASK);
        snd_playback_send(&playback_channel->base);
    } else {
		//如果客户端已经stop了，就不发送stop命令和pcm数据命令了
		//因为stop时，可能有数据在pending_frame里，则不发送了
		//什么情况下会走到这里？猜想应该是多客户端或者
		//迁移时可能走到这个逻辑
        playback_channel->base.command &= ~SND_PLAYBACK_CTRL_MASK;
        playback_channel->base.command &= ~SND_PLAYBACK_PCM_MASK;

		// 如果还有pending帧，丢掉这帧，回收帧缓冲区
        if (playback_channel->pending_frame) {
            spice_assert(!playback_channel->in_progress);
            snd_playback_free_frame(playback_channel,
                                    playback_channel->pending_frame);
            playback_channel->pending_frame = NULL;
        }
    }
}

SPICE_GNUC_VISIBLE void spice_server_playback_get_buffer(SpicePlaybackInstance *sin,
                                                         uint32_t **frame, uint32_t *num_samples)
{
    SndChannel *channel = sin->st->worker.connection;
    PlaybackChannel *playback_channel = 
		SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    if (!channel || !playback_channel->free_frames) { 
		//如果没有用户连接spice_server，channel为空
		//而且当空闲的frames都被分配完之后，将返回一个空buffer
		//spiceaudio对于这种情况是直接丢掉了。对于帧被分配完的情况，应该输出日志
        *frame = NULL;
        *num_samples = 0;
        return;
    }
	//获取buffer时，要求已经发送过start命令（已经激活）
    spice_assert(playback_channel->base.active); 
	//SndChannel加引用计数
	snd_channel_get(channel);

    *frame = playback_channel->free_frames->samples;
	//从空闲链表中摘除被分配出去的音频帧AudioFrame
    playback_channel->free_frames = playback_channel->free_frames->next;
	//返回编解码器支持的帧数量
    *num_samples = snd_codec_frame_size(playback_channel->codec);
}

SPICE_GNUC_VISIBLE void spice_server_playback_put_samples(SpicePlaybackInstance *sin, uint32_t *samples)
{
    PlaybackChannel *playback_channel;
    AudioFrame *frame;
	
    frame = SPICE_CONTAINEROF(samples, AudioFrame, samples);
    playback_channel = frame->channel;

	//因为spice_server_playback_get_buffer对SndChannel加了引用，所以frame->channel一定有效
    spice_assert(playback_channel);

	//连接断开了或者残留帧的情况，直接不输出帧了
    if (!snd_channel_put(&playback_channel->base) ||
        sin->st->worker.connection != &playback_channel->base) {
        /* lost last reference, channel has been destroyed previously */
        spice_info("audio samples belong to a disconnected channel");
        return;
    }

	// 连接有效的情况下，必然发送过start命令了
    spice_assert(playback_channel->base.active);

	// 正常发送的情况下，pending_frame被processing_frame接管，pending_frame被设置为空
	// 如果发送新帧时，发现pending_frame，那么说明必然有帧没有被接管成功，那么pending_frame
	// 里的内容就被丢弃掉了。所以，应该加点日志，或者加点统计来处理一下
    if (playback_channel->pending_frame) {
        snd_playback_free_frame(playback_channel, playback_channel->pending_frame);
    }

	// 每一帧都会记录下生产的时间
    frame->time = reds_get_mm_time();

	// 设置qxl的mm时间，为什么这么做还不清楚
    red_dispatcher_set_mm_time(frame->time);

	// 设置pending_frame为新帧
    playback_channel->pending_frame = frame;

	// 设置PCM数据命令，snd_playback_send就会发送pending_frame指针里的数据
    snd_set_command(&playback_channel->base, SND_PLAYBACK_PCM_MASK);

	// 触发回放通道发包流程
    snd_playback_send(&playback_channel->base);
}

void snd_set_playback_latency(RedClient *client, uint32_t latency)
{
    SndWorker *now = workers;

    for (; now; now = now->next) {
        if (now->base_channel->type == SPICE_CHANNEL_PLAYBACK && now->connection &&
            now->connection->channel_client->client == client) {

            if (red_channel_client_test_remote_cap(now->connection->channel_client,
                SPICE_PLAYBACK_CAP_LATENCY)) {
                PlaybackChannel* playback = (PlaybackChannel*)now->connection;

                playback->latency = latency;
                snd_set_command(now->connection, SND_PLAYBACK_LATENCY_MASK);
                snd_playback_send(now->connection);
            } else {
                spice_debug("client doesn't not support SPICE_PLAYBACK_CAP_LATENCY");
            }
        }
    }
}

static int snd_desired_audio_mode(int frequency, 
	int client_can_celt, 
	int client_can_opus)
{
	// 关压缩则返回原始模式，默认情况
    if (! playback_compression)
        return SPICE_AUDIO_DATA_MODE_RAW;

	// 如果客户端支持OPUS，且本地OPUS编解码器也支持指定的频率，则优先使用OPUS
	// 实际上OPUS只支持48000基数的采样率，如 24000, 48000, 12000等
    if (client_can_opus && 
		snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        return SPICE_AUDIO_DATA_MODE_OPUS;

	// 如果客户端支持CELT，且本地CELT编解码器也支持指定的频率，则使用CELT
	// 实际上CELT支持任何分辨率，而且默认开启了CELT编译，所以只要客户端支持CETL
	// 就一定可以走CELT
    if (client_can_celt && 
		snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, frequency))
        return SPICE_AUDIO_DATA_MODE_CELT_0_5_1;

	/* 如果上面情况都不满足，则使用原始数据格式，如：
	   1. 客户端不支持CELT和OPUS
	   2. 客户端只支持OPUS，但当前采样率OPUS不支持
	   3. 客户端只支持OPUS，但是服务端默认OPUS没开启编译
	   默认走RAW格式
	*/
    return SPICE_AUDIO_DATA_MODE_RAW;
}

static void on_new_playback_channel(SndWorker *worker)
{
    PlaybackChannel *playback_channel =
        SPICE_CONTAINEROF(worker->connection, PlaybackChannel, base);
    SpicePlaybackState *st = SPICE_CONTAINEROF(worker, SpicePlaybackState, worker);

    spice_assert(playback_channel);

	// 无论是否正在播放都要发送模式命令给客户端
    snd_set_command((SndChannel *)playback_channel, SND_PLAYBACK_MODE_MASK);
    if (playback_channel->base.active) { 
		//如果连接时，播放已经开始进行，那么需要先发送控制命令给客户端
        snd_set_command((SndChannel *)playback_channel, SND_PLAYBACK_CTRL_MASK);
    }
    if (st->volume.volume_nchannels) {
		//如果连接时，已经设置了音量，需要把音量发送给客户端
        snd_set_command((SndChannel *)playback_channel, SND_PLAYBACK_VOLUME_MASK);
    }
    if (playback_channel->base.active) {
        reds_disable_mm_timer();
    }
}

static void snd_playback_cleanup(SndChannel *channel)
{
    PlaybackChannel *playback_channel = 
		SPICE_CONTAINEROF(channel, PlaybackChannel, base);

    if (playback_channel->base.active) {
        reds_enable_mm_timer();
    }

    snd_codec_destroy(&playback_channel->codec);
}

/**
 * snd_set_playback_peer - 通道客户端连接时的钩子函数
 * channel 回放通道对象，在虚拟机开机时就创建好了
 * RedClient 客户端对象，在主通道连接时就创建好了
 * stream 回放通道的流对象，在回放通道tcp连接建立好了之后创建
 * 其他省略
 * 注意，这个函数需要能支持在vm已经未开启、已经开启的情况，
 * 而且能够反复的连接、断开回话，特别是在已经vm里播放音乐的情况下连接也能正常
**/
static void snd_set_playback_peer(RedChannel *channel, RedClient *client,
	RedsStream *stream, int migration, int num_common_caps, 
	uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    SndWorker *worker = channel->data;
    PlaybackChannel *playback_channel = NULL;
    SpicePlaybackState *st = 
		SPICE_CONTAINEROF(worker, SpicePlaybackState, worker);

    snd_disconnect_channel(worker->connection);
	
    // 创建PlaybackChannel(也是SndChannel), RCC, 设置socket，通道钩子函数，监听socket
	playback_channel = (PlaybackChannel *)__new_channel(
		worker,
		sizeof(*playback_channel),
		SPICE_CHANNEL_PLAYBACK,
		client,
		stream,
		migration,
		snd_playback_send,
		snd_playback_handle_message,
		snd_playback_on_message_done,
		snd_playback_cleanup,
		common_caps, num_common_caps,
		caps, num_caps);
    if (!playback_channel) {
        return;
    }

	//建立SndWorker和SndChannel之间的关联
    worker->connection = &playback_channel->base;
	//将默认的三个帧缓冲区压入free_frames队列中
    snd_playback_free_frame(playback_channel, &playback_channel->frames[0]);
    snd_playback_free_frame(playback_channel, &playback_channel->frames[1]);
    snd_playback_free_frame(playback_channel, &playback_channel->frames[2]);
	
	// 判断客户端是否支持CELT
    int client_can_celt = red_channel_client_test_remote_cap(
		playback_channel->base.channel_client,
		SPICE_PLAYBACK_CAP_CELT_0_5_1);

	// 判断客户端是否支持OPUS
    int client_can_opus = red_channel_client_test_remote_cap(
		playback_channel->base.channel_client,
		SPICE_PLAYBACK_CAP_OPUS);

	// 通过协商，取得当前可以支持的数据模式
    int desired_mode = snd_desired_audio_mode(st->frequency, 
    	client_can_celt, client_can_opus);
    playback_channel->mode = SPICE_AUDIO_DATA_MODE_RAW; //默认RAW格式

	// 协商的格式不是RAW,则创建编解码器并设置为客户端期望的格式
    if (desired_mode != SPICE_AUDIO_DATA_MODE_RAW) {
        if (snd_codec_create(&playback_channel->codec, 
			desired_mode, st->frequency, SND_CODEC_ENCODE) == SND_CODEC_OK)
		{
            playback_channel->mode = desired_mode;
        } else {
            spice_printerr("create encoder failed");
        }
    }

	// 调用回放通道创建钩子函数，以支持正在回放时的spice回话
    on_new_playback_channel(worker);

	// 如果本地激活，则马上发送一个PLBK_START命令给客户端
    if (worker->active) {
        spice_server_playback_start(st->sin);
    }

	// 执行一次回放通道的命令发送
    snd_playback_send(worker->connection);
}

static void snd_record_migrate_channel_client(RedChannelClient *rcc)
{
    SndWorker *worker;

    spice_debug(NULL);
    spice_assert(rcc->channel);
    spice_assert(rcc->channel->data);
    worker = (SndWorker *)rcc->channel->data;

    if (worker->connection) {
        spice_assert(worker->connection->channel_client == rcc);
        snd_set_command(worker->connection, SND_RECORD_MIGRATE_MASK);
        snd_record_send(worker->connection);
    }
}

// 录音通道，设置录音通道的音量
SPICE_GNUC_VISIBLE void spice_server_record_set_volume(SpiceRecordInstance *sin,
                                                uint8_t nchannels,
                                                uint16_t *volume)
{
    SpiceVolumeState *st = &sin->st->volume;
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    st->volume_nchannels = nchannels;
    free(st->volume); //先释放之前的音量信息，不要判断一下吗？至少断言一下
    st->volume = spice_memdup(volume, sizeof(uint16_t) * nchannels);

    if (!channel || nchannels == 0)
        return;
	//设置时发送音量信息
    snd_record_send_volume(record_channel);
}

// 录音通道，设置静音值，可能静音，也可能非静音
SPICE_GNUC_VISIBLE void spice_server_record_set_mute(SpiceRecordInstance *sin, uint8_t mute)
{
    SpiceVolumeState *st = &sin->st->volume;
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    st->mute = mute;

    if (!channel)
        return;

	//马上发送静音状态信息
    snd_record_send_mute(record_channel);
}

// 录音通道start命令响应函数，无论spice是否连接，都会执行
SPICE_GNUC_VISIBLE void spice_server_record_start(SpiceRecordInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    sin->st->worker.active = 1;
    if (!channel)
        return;
    spice_assert(!record_channel->base.active);
    record_channel->base.active = TRUE;
	//读位置和写位置重置
    record_channel->read_pos = record_channel->write_pos = 0;   //todo: improve by
                                                                //stream generation
    if (!record_channel->base.client_active) {
        snd_set_command(&record_channel->base, SND_RECORD_CTRL_MASK);
        snd_record_send(&record_channel->base);
    } else {
        record_channel->base.command &= ~SND_RECORD_CTRL_MASK;
    }
}

SPICE_GNUC_VISIBLE void spice_server_record_stop(SpiceRecordInstance *sin)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);

    sin->st->worker.active = 0;
    if (!channel)
        return;
    spice_assert(record_channel->base.active);
    record_channel->base.active = FALSE;
    if (record_channel->base.client_active) {
        snd_set_command(&record_channel->base, SND_RECORD_CTRL_MASK);
        snd_record_send(&record_channel->base);
    } else {
        record_channel->base.command &= ~SND_RECORD_CTRL_MASK;
    }
}

//从录音通道中获取数据，放到samples中，返回读到的样本数量
SPICE_GNUC_VISIBLE uint32_t spice_server_record_get_samples(SpiceRecordInstance *sin,
                                                            uint32_t *samples, uint32_t bufsize)
{
    SndChannel *channel = sin->st->worker.connection;
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);
    uint32_t read_pos;
    uint32_t now;
    uint32_t len;

	//通道断开，则直接返回没获取到数据
    if (!channel)
        return 0;
    spice_assert(record_channel->base.active);

	//如果数据不足缓冲区的一般，不返回数据，
	//这样RECORD_SAMPLES_SIZE不能轻易修改，否则，这样可能造成收数据不及时
    if (record_channel->write_pos < RECORD_SAMPLES_SIZE / 2) {
        return 0;
    }

	//可用样本数量和缓冲区的大小最小值
    len = MIN(record_channel->write_pos - record_channel->read_pos, bufsize);

	// 数据不足一个缓冲区，先尝试读一下数据
    if (len < bufsize) {
        SndWorker *worker = record_channel->base.worker;
        snd_receive(record_channel);
        if (!worker->connection) {//为什么这里判断一下？
            return 0;
        }
		//重新计算一下可读取的样本数量，因为可能读上来一些数据了
        len = MIN(record_channel->write_pos - record_channel->read_pos, bufsize);
    }

	//回绕一下读位置, 注意：数据接收的时候可能会改record_channel->read_pos的值
	//所以read_pos需要等snd_receive后读取才有效
    read_pos = record_channel->read_pos % RECORD_SAMPLES_SIZE;
	
	//本次必然要读到len个样本，更新一下读位置，但是此时的值可能超过了缓冲区
	//要到下一次读的时候才回绕。本次直接增加len个样本的值
    record_channel->read_pos += len; 

	//先读到缓冲区末尾的数据，或者len长度样本
    now = MIN(len, RECORD_SAMPLES_SIZE - read_pos);
    memcpy(samples, &record_channel->samples[read_pos], now * 4);
	//如果还要回绕到开始，则读取剩下的数据量
    if (now < len) {
        memcpy(samples + now, record_channel->samples, (len - now) * 4);
    }

	//返回实际读到的样本数量
    return len;
}

// spice服务端获取推荐的采样率
SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_playback_rate(SpicePlaybackInstance *sin)
{
    int client_can_opus = TRUE;//默认认为客户端支持OPUS
    if (sin && sin->st->worker.connection) {//如果通道连接了，看客户端能否支持OPUS
        SndChannel *channel = sin->st->worker.connection;
        PlaybackChannel *playback_channel = SPICE_CONTAINEROF(channel, PlaybackChannel, base);
        client_can_opus = red_channel_client_test_remote_cap(playback_channel->base.channel_client,
                                          SPICE_PLAYBACK_CAP_OPUS);
    }

	//如果客户端支持OPUS，并且本地编译了OPUS，则返回OPUS的频率
    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, SND_CODEC_ANY_FREQUENCY))
        return SND_CODEC_OPUS_PLAYBACK_FREQ;

	//默认返回CELT算法的采样率
    return SND_CODEC_CELT_PLAYBACK_FREQ;
}

// 设置回放采样率
SPICE_GNUC_VISIBLE void spice_server_set_playback_rate(SpicePlaybackInstance *sin, uint32_t frequency)
{
    RedChannel *channel = sin->st->worker.base_channel;
	//直接设置SpicePlaybackState的采样率
    sin->st->frequency = frequency;
	//等到设置playback的采样率时，才能确定是否支持OPUS, 因为OPUS可能设置的采样率不支持
	//回放支持要支持OPUS需要：
	// 1.代码编译了OPUS库支持，包括客户端和服务端
	// 2.QEMU设置的采样率支持OPUS
	// 3.客户端支持OPUS
	// 4.qemu选项没有关闭音频压缩
    if (channel && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        red_channel_set_cap(channel, SPICE_PLAYBACK_CAP_OPUS);
}

// spice服务端获取录音时的推荐采样率，逻辑与回放通道类似
SPICE_GNUC_VISIBLE uint32_t spice_server_get_best_record_rate(SpiceRecordInstance *sin)
{
    int client_can_opus = TRUE;
    if (sin && sin->st->worker.connection) {
        SndChannel *channel = sin->st->worker.connection;
        RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);
        client_can_opus = red_channel_client_test_remote_cap(record_channel->base.channel_client,
                                          SPICE_RECORD_CAP_OPUS);
    }

    if (client_can_opus && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, SND_CODEC_ANY_FREQUENCY))
        return SND_CODEC_OPUS_PLAYBACK_FREQ;

    return SND_CODEC_CELT_PLAYBACK_FREQ;
}

SPICE_GNUC_VISIBLE void spice_server_set_record_rate(SpiceRecordInstance *sin, uint32_t frequency)
{
    RedChannel *channel = sin->st->worker.base_channel;
    sin->st->frequency = frequency;
    if (channel && snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_OPUS, frequency))
        red_channel_set_cap(channel, SPICE_RECORD_CAP_OPUS);
}

static void on_new_record_channel(SndWorker *worker)
{
    RecordChannel *record_channel = (RecordChannel *)worker->connection;
    SpiceRecordState *st = SPICE_CONTAINEROF(worker, SpiceRecordState, worker);

    spice_assert(record_channel);

    if (st->volume.volume_nchannels) { //触发音量信息发送请求
        snd_set_command((SndChannel *)record_channel, SND_RECORD_VOLUME_MASK);
    }
    if (record_channel->base.active) { //如果录音通道已经激活了，发送start信息
        snd_set_command((SndChannel *)record_channel, SND_RECORD_CTRL_MASK);
    }
}

static void snd_record_cleanup(SndChannel *channel)
{
    RecordChannel *record_channel = SPICE_CONTAINEROF(channel, RecordChannel, base);
    snd_codec_destroy(&record_channel->codec);
}

/**
 * snd_set_record_peer - record通道连接时的钩子函数
 * channel 连接的通道对象
 * client 连接的客户端
 * stream 连接的socket流
 * migration 是否为迁移时的连接，vdi走不到
 * num_common_caps 通用特性数量
 * comm_caps 通用特性位图
 * num_caps 录音通道特性数量
 * caps 录音通道特性位图
 * 
**/
static void snd_set_record_peer(RedChannel *channel, RedClient *client, 
	RedsStream *stream, int migration, int num_common_caps, 
	uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    SndWorker *worker = channel->data;
    RecordChannel *record_channel;
    SpiceRecordState *st = SPICE_CONTAINEROF(worker, SpiceRecordState, worker);

    snd_disconnect_channel(worker->connection);

    if (!(record_channel = (RecordChannel *)__new_channel(
		worker,
		sizeof(*record_channel),
		SPICE_CHANNEL_RECORD,
		client,
		stream,
		migration,
		snd_record_send,
		snd_record_handle_message,
		snd_record_on_message_done,
		snd_record_cleanup,
		common_caps, num_common_caps,
		caps, num_caps))) 
	{
        return;
    }

	//默认模式为原始数据模式
    record_channel->mode = SPICE_AUDIO_DATA_MODE_RAW;

	//建立SndWork和SndChannel之间的关联
    worker->connection = &record_channel->base;

	//调用钩子
    on_new_record_channel(worker);
    if (worker->active) {
        spice_server_record_start(st->sin);
    }
    snd_record_send(worker->connection);
}

static void snd_playback_migrate_channel_client(RedChannelClient *rcc)
{
    SndWorker *worker;

    spice_assert(rcc->channel);
    spice_assert(rcc->channel->data);
    worker = (SndWorker *)rcc->channel->data;
    spice_debug(NULL);

    if (worker->connection) {
        spice_assert(worker->connection->channel_client == rcc);
        snd_set_command(worker->connection, SND_PLAYBACK_MIGRATE_MASK);
        snd_playback_send(worker->connection);
    }
}

/*添加SndWorker到静态链表首部*/
static void add_worker(SndWorker *worker)
{
    worker->next = workers;
    workers = worker;
}

/*移除一个SndWorker*/
static void remove_worker(SndWorker *worker)
{
    SndWorker **now = &workers;
    while (*now) {
        if (*now == worker) {
            *now = worker->next;
            return;
        }
        now = &(*now)->next;
    }
    spice_printerr("not found");
}

//
void snd_attach_playback(SpicePlaybackInstance *sin)
{
    SndWorker *playback_worker;
    RedChannel *channel;
    ClientCbs client_cbs = { NULL, };

	/*构建SpicePlaybackState结构，该结构包含了SndWorker、音量和频率值*/
    sin->st = spice_new0(SpicePlaybackState, 1);
    sin->st->sin = sin;
    playback_worker = &sin->st->worker;
    sin->st->frequency = SND_CODEC_CELT_PLAYBACK_FREQ; /* Default to the legacy rate */

	/*SndWorker base_channel指向通道，通道的data指向SndWorker */
    // TODO: Make RedChannel base of worker? instead of assigning it to channel->data
    channel = red_channel_create_dummy(sizeof(RedChannel), SPICE_CHANNEL_PLAYBACK, 0);
    //channel->data = playback_worker; 这行代码重复了，下面的red_channel_set_data再设置了一次
	
    client_cbs.connect = snd_set_playback_peer; //通道连接钩子
    client_cbs.disconnect = snd_disconnect_channel_client; //RCC断开钩子
    client_cbs.migrate = snd_playback_migrate_channel_client; //通道迁移钩子，VDI里无用
    red_channel_register_client_cbs(channel, &client_cbs);
    red_channel_set_data(channel, playback_worker);

    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY))
        red_channel_set_cap(channel, SPICE_PLAYBACK_CAP_CELT_0_5_1); //支持CELT051音频压缩

    red_channel_set_cap(channel, SPICE_PLAYBACK_CAP_VOLUME);//支持音量调节

    playback_worker->base_channel = channel;//通道和SndWorker实现了互相引用
    add_worker(playback_worker); //添加到静态SndWork链表
    reds_register_channel(playback_worker->base_channel);//添加到全局reds的通道链表中
}

/*本函数基本上是snd_attach_playback的翻版，流程完全一样*/
void snd_attach_record(SpiceRecordInstance *sin)
{
    SndWorker *record_worker;
    RedChannel *channel;
    ClientCbs client_cbs = { NULL, };

    sin->st = spice_new0(SpiceRecordState, 1);
    sin->st->sin = sin;
    record_worker = &sin->st->worker;
    sin->st->frequency = SND_CODEC_CELT_PLAYBACK_FREQ; /* Default to the legacy rate */

    // TODO: Make RedChannel base of worker? instead of assigning it to channel->data
    channel = red_channel_create_dummy(sizeof(RedChannel), SPICE_CHANNEL_RECORD, 0);

    channel->data = record_worker;
    client_cbs.connect = snd_set_record_peer;
    client_cbs.disconnect = snd_disconnect_channel_client;
    client_cbs.migrate = snd_record_migrate_channel_client;
    red_channel_register_client_cbs(channel, &client_cbs);
    red_channel_set_data(channel, record_worker);
    if (snd_codec_is_capable(SPICE_AUDIO_DATA_MODE_CELT_0_5_1, SND_CODEC_ANY_FREQUENCY))
        red_channel_set_cap(channel, SPICE_RECORD_CAP_CELT_0_5_1);
    red_channel_set_cap(channel, SPICE_RECORD_CAP_VOLUME);

    record_worker->base_channel = channel;
    add_worker(record_worker);
    reds_register_channel(record_worker->base_channel);
}

static void snd_detach_common(SndWorker *worker)
{
    if (!worker) {
        return;
    }
    remove_worker(worker);
    snd_disconnect_channel(worker->connection);//断开SndChannel
    reds_unregister_channel(worker->base_channel);//redchannel从全局链表中摘除
    red_channel_destroy(worker->base_channel);//redchannel销毁
}

static void spice_playback_state_free(SpicePlaybackState *st)
{
    free(st->volume.volume);//音量设置后，音量信息也是malloc出来的
    free(st);//st时malloc出来的
}

//干掉回放通道
void snd_detach_playback(SpicePlaybackInstance *sin)
{
    snd_detach_common(&sin->st->worker);
    spice_playback_state_free(sin->st);
}

static void spice_record_state_free(SpiceRecordState *st)
{
    free(st->volume.volume);
    free(st);
}

//干掉录音通道
void snd_detach_record(SpiceRecordInstance *sin)
{
    snd_detach_common(&sin->st->worker);
    spice_record_state_free(sin->st);
}

/* 设置回放通道压缩开关，
   会造成期望的数据模式发生改变，所以如果模式发送改变
   需要重新发送模式数据
*/
void snd_set_playback_compression(int on)
{
    SndWorker *now = workers;

    playback_compression = !!on;

    for (; now; now = now->next) {
        if (now->base_channel->type == SPICE_CHANNEL_PLAYBACK && now->connection) {
            PlaybackChannel* playback = (PlaybackChannel*)now->connection;
            SpicePlaybackState *st = SPICE_CONTAINEROF(now, SpicePlaybackState, worker);
            int client_can_celt = red_channel_client_test_remote_cap(playback->base.channel_client,
                                    SPICE_PLAYBACK_CAP_CELT_0_5_1);
            int client_can_opus = red_channel_client_test_remote_cap(playback->base.channel_client,
                                    SPICE_PLAYBACK_CAP_OPUS);
            int desired_mode = snd_desired_audio_mode(st->frequency, client_can_opus, client_can_celt);
            if (playback->mode != desired_mode) {
                playback->mode = desired_mode;
                snd_set_command(now->connection, SND_PLAYBACK_MODE_MASK);
            }
        }
    }
}

/* 返回回放压缩开关 */
int snd_get_playback_compression(void)
{
    return playback_compression;
}
