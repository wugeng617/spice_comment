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


    Author:
        yhalperi@redhat.com
*/

#ifndef _H_RED_CHANNEL
#define _H_RED_CHANNEL

#include <pthread.h>
#include <limits.h>

#include "common/ring.h"
#include "common/marshaller.h"

#include "spice.h"
#include "red_common.h"
#include "demarshallers.h"
#include "reds_stream.h"

#define MAX_SEND_BUFS 1000
#define CLIENT_ACK_WINDOW 20

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

#define MAX_HEADER_SIZE sizeof(SpiceDataHeader)

/* Basic interface for channels, without using the RedChannel interface.
   The intention is to move towards one channel interface gradually.
   At the final stage, this interface shouldn't be exposed. Only RedChannel will use it. 

   spice消息头部的处理函数定义和结构定义。
   之所以这么定义，是因为spice可以用简单头部，和复杂头部来进行协议通信
   这样就会造成头部操作的实际动作不一样。所以使用函数指针接口来定义Spice数据头部结构体
*/
typedef struct SpiceDataHeaderOpaque SpiceDataHeaderOpaque;

typedef uint16_t (*get_msg_type_proc)(SpiceDataHeaderOpaque *header); //获取头部中的类型
typedef uint32_t (*get_msg_size_proc)(SpiceDataHeaderOpaque *header); //获取头部中的数据长度
typedef void (*set_msg_type_proc)(SpiceDataHeaderOpaque *header, uint16_t type); //设置消息头中的类型字段
typedef void (*set_msg_size_proc)(SpiceDataHeaderOpaque *header, uint32_t size); //设置消息头中的消息大小字段
typedef void (*set_msg_serial_proc)(SpiceDataHeaderOpaque *header, uint64_t serial); //设置消息头的序号，VDI中为空
typedef void (*set_msg_sub_list_proc)(SpiceDataHeaderOpaque *header, uint32_t sub_list); //设置消息头的字列表数量，VDI中为空

struct SpiceDataHeaderOpaque {
    uint8_t *data; //头部数据的存放地址
    uint16_t header_size; // 头部大小

	/*头部操作接口集合，见上面的原型定义*/
    set_msg_type_proc set_msg_type; 
    set_msg_size_proc set_msg_size;
    set_msg_serial_proc set_msg_serial;
    set_msg_sub_list_proc set_msg_sub_list;

    get_msg_type_proc get_msg_type;
    get_msg_size_proc get_msg_size;
};

/* 直接处理字节流的消息句柄 */
typedef int (*handle_message_proc)(void *opaque,
                                   uint16_t type, uint32_t size, uint8_t *msg);

/* 处理解析后内存消息对象的句柄，为了和handle_message_proc原型区分，
这个函数的原型把type和size参数的位置换了一下，msg的指针类型只是声明上有点不同 */
typedef int (*handle_parsed_proc)(void *opaque, uint32_t size, uint16_t type, void *message);

/* 分配消息缓冲区 */
typedef uint8_t *(*alloc_msg_recv_buf_proc)(void *opaque, uint16_t type, uint32_t size);

/* 释放消息缓冲区，感觉type参数没什么用 */
typedef void (*release_msg_recv_buf_proc)(void *opaque,
                                          uint16_t type, uint32_t size, uint8_t *msg);

/* 收取消息时的错误处理函数，当前代码会在收取消息异常返回时调用次函数，
   包括如下四种错误情况：
   1. socket read返回失败
   2. 分配消息缓冲区失败
   3. 释放消息缓冲区失败
   4. 消息分发处理失败，无论是分发的字节流还是消息对象
*/
typedef void (*on_incoming_error_proc)(void *opaque);

/* 成功接收到消息时调用，n表示接受到消息的长度 */
typedef void (*on_input_proc)(void *opaque, int n);

/* 如果incoming handler中设置了parse，表示需要把从socket里收到
 * 的字节流解析成内存对象后，再调用handle_parsed进行处理；如果没有
 * 设置则直接调用handle_message进行处理，也就是说handle_message处理的是
 * 最原始的字节流，将spice协议的包头解析了出来，再具体的消息结构还要handle_message
 * 自己去处理。
*/
typedef struct IncomingHandlerInterface {
    handle_message_proc handle_message; //字节流消息处理函数
    alloc_msg_recv_buf_proc alloc_msg_buf; //分配消息缓冲区函数
    on_incoming_error_proc on_error; // 接受消息时的错误处理函数
    release_msg_recv_buf_proc release_msg_buf; // 释放消息缓冲区函数，用于错误处理
    
    // The following is an optional alternative to handle_message, used if not null
    spice_parse_channel_func_t parser; //消息解析函数，将消息字节流解析为内存对象
    handle_parsed_proc handle_parsed; //消息对象处理句柄，决定着怎么处理解析后的消息对象
    on_input_proc on_input; //每次成功收到一点数据都会调用此函数，无论是头部数据还是消息数据
} IncomingHandlerInterface;

/* 通道数据接收器结构 */
typedef struct IncomingHandler {
    IncomingHandlerInterface *cb; //数据接收句柄操作指针
    void *opaque; // 消息接受时，各种消息接收过程中句柄函数需要使用的占位指针
    uint8_t header_buf[MAX_HEADER_SIZE]; //预分配的头部缓冲区
    SpiceDataHeaderOpaque header; // 消息头部处理结构，用于读取和设置消息的头部
    uint32_t header_pos; // 当前读取spice消息头部的位置
    uint8_t *msg; // alloc_msg_buf分配的消息缓冲区位置，是消息部分的指针
    uint32_t msg_pos; // 当前接收到的消息数据部分的位置
    uint64_t serial; // 消息序号
} IncomingHandler;

/* 获取发送数据的尺寸，如果没有消息要发送，则应该返回0 */
typedef int (*get_outgoing_msg_size_proc)(void *opaque);

/* 准备发送数据，将待发送的数据存放在iovec中，并返回iovec的数量，
   pos表示要跳过的数据数量，发送一个spice协议消息时可能被中断（如socket写满、信号中断）
   所以一个消息可能需要多次调用此函数，才可以把消息发送完毕。
   pos表示已经发送过的数据量，因此是本次发送需要跳过的已发送数据量 */
typedef void (*prepare_outgoing_proc)(void *opaque, struct iovec *vec, int *vec_size, int pos);

/* 发送失败返回前的错误处理函数 */
typedef void (*on_outgoing_error_proc)(void *opaque);

/* 当发送数据被阻塞时调用的回调函数 */
typedef void (*on_outgoing_block_proc)(void *opaque);

/* 当所有数据发送完成的回调函数 */
typedef void (*on_outgoing_msg_done_proc)(void *opaque);

/* 发送数据成功时的回调函数 */
typedef void (*on_output_proc)(void *opaque, int n);

/* 数据发送器操作和回调函数集合 */
typedef struct OutgoingHandlerInterface {
    get_outgoing_msg_size_proc get_msg_size; //获取发送消息的大小
    prepare_outgoing_proc prepare; //消息准备接口
    on_outgoing_error_proc on_error; //消息出错例程
    on_outgoing_block_proc on_block; //消息阻塞例程
    on_outgoing_msg_done_proc on_msg_done; //消息发送完成例程
    on_output_proc on_output; //发送了一部分消息的例程
} OutgoingHandlerInterface;

// 数据发送器
typedef struct OutgoingHandler {
    OutgoingHandlerInterface *cb; //数据发送器的回调函数集合
    void *opaque; //发送器占位指针，指向RCC
    struct iovec vec_buf[IOV_MAX]; // 静态iovec数组，被vec引用
    int vec_size; //iovec的有效数量
    struct iovec *vec; //iovec列表
    int pos;  //当前已发送数据量
    int size; //发送消息的大小，包括头部的整个消息的大小，
              //发送消息时，当发现此值为0，则表示要发送一个新消息
              //要调用get_outgoing_msg_size_proc get_msg_size来获取消息大小
} OutgoingHandler;

/* Red Channel interface */

typedef struct BufDescriptor {//缓冲区描述符
    uint32_t size; //缓冲区大小
    uint8_t *data; //缓冲区地址
} BufDescriptor;

typedef struct RedChannel RedChannel; //RCH
typedef struct RedChannelClient RedChannelClient;//RCC
typedef struct RedClient RedClient;  //RCL
typedef struct MainChannelClient MainChannelClient; //MRCL

/* Messages handled by red_channel
 * SET_ACK - sent to client on channel connection
 * Note that the numbers don't have to correspond to spice message types,
 * but we keep the 100 first allocated for base channel approach.
 * RedChannel控制的消息
 * */
enum {
    PIPE_ITEM_TYPE_SET_ACK=1, //配置ACK
    PIPE_ITEM_TYPE_MIGRATE, //迁移消息
    PIPE_ITEM_TYPE_EMPTY_MSG, //空消息，只有消息头，通常表示一个事件的消息
    PIPE_ITEM_TYPE_PING, //PING消息包

    PIPE_ITEM_TYPE_CHANNEL_BASE=101, //通道消息基础值
};

// RCC发送管道节点
typedef struct PipeItem {
    RingItem link; //连接点
    int type; //管道项的类型
} PipeItem;

typedef int (*channel_configure_socket_proc)(RedChannelClient *rcc);

typedef void (*channel_disconnect_proc)(RedChannelClient *rcc);


typedef void (*channel_send_pipe_item_proc)(RedChannelClient *rcc, PipeItem *item);

typedef void (*channel_hold_pipe_item_proc)(RedChannelClient *rcc, PipeItem *item);

typedef void (*channel_release_pipe_item_proc)(RedChannelClient *rcc,
	PipeItem *item, int item_pushed);


typedef uint8_t *(*channel_alloc_msg_recv_buf_proc)(RedChannelClient *channel,
	uint16_t type, uint32_t size);

typedef void (*channel_release_msg_recv_buf_proc)(RedChannelClient *channel,
	uint16_t type, uint32_t size, uint8_t *msg);


typedef int (*channel_handle_parsed_proc)(RedChannelClient *rcc, 
	uint32_t size, uint16_t type, void *message);

typedef int (*channel_handle_message_proc)(RedChannelClient *rcc,
	uint16_t type, uint32_t size, uint8_t *msg);


// 没用到，先注释掉
//typedef void (*channel_on_incoming_error_proc)(RedChannelClient *rcc);
//typedef void (*channel_on_outgoing_error_proc)(RedChannelClient *rcc);

/* 迁移相关的回调，不关心 */
typedef int (*channel_handle_migrate_flush_mark_proc)(RedChannelClient *base);

typedef int (*channel_handle_migrate_data_proc)(RedChannelClient *base,
	uint32_t size, void *message);

typedef uint64_t (*channel_handle_migrate_data_get_serial_proc)(
	RedChannelClient *base, uint32_t size, void *message);


/* RCC连接处理 */
typedef void (*channel_client_connect_proc)(RedChannel *channel, 
	RedClient *client, RedsStream *stream,
	int migration, int num_common_cap, uint32_t *common_caps,
	int num_caps, uint32_t *caps);

/* RCC断开函数，每个RCC断开都要执行 */
typedef void (*channel_client_disconnect_proc)(RedChannelClient *base);

/* RCC迁移 */
typedef void (*channel_client_migrate_proc)(RedChannelClient *base);

// TODO: add ASSERTS for thread_id  in client and channel calls
//
/*
 * callbacks that are triggered from channel client stream events.
 * They are called from the thread that listen to the stream events.
 * RCC操作函数，包含socket配置、RCC断开、（管道项引用、解引用、发送）、
 * （消息缓冲区分配、释放）、RCC迁移事件函数
 */
typedef struct {
    channel_configure_socket_proc config_socket; //通道初始化时配置socket
    //通道断开, 主通道、输入通道、显示通道、光标通道、智能卡通道、USB通道
    //会独立设置
    channel_disconnect_proc on_disconnect; 
    channel_send_pipe_item_proc send_item; //发送管道数据，使用pipe_item
    channel_hold_pipe_item_proc hold_item; //持有管道数据， 相当于ref
    channel_release_pipe_item_proc release_item; //释放管道数据，相当于unref
    channel_alloc_msg_recv_buf_proc alloc_recv_buf; //分配消息接收缓冲区
    channel_release_msg_recv_buf_proc release_recv_buf; //释放消息接收缓冲区
    channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark; //迁移...
    channel_handle_migrate_data_proc handle_migrate_data; //迁移...
    channel_handle_migrate_data_get_serial_proc handle_migrate_data_get_serial;//迁移...
} ChannelCbs;


/*
 * callbacks that are triggered from client events.
 * They should be called from the thread that handles the RedClient
 * 通道级别的事件处理函数
 */
typedef struct {
    channel_client_connect_proc connect; //通道连接事件，被reds_channel_do_link调用
    channel_client_disconnect_proc disconnect;
    channel_client_migrate_proc migrate;
} ClientCbs;

typedef struct RedChannelCapabilities {
    int num_common_caps;
    uint32_t *common_caps;
    int num_caps;
    uint32_t *caps;
} RedChannelCapabilities;

int test_capability(uint32_t *caps, int num_caps, uint32_t cap);

typedef struct RedChannelClientLatencyMonitor {
    int state;
    uint64_t last_pong_time;
    SpiceTimer *timer;
    uint32_t id;
    int tcp_nodelay; //tcp_nodelay
    int warmup_was_sent;

    int64_t roundtrip; //来回延时，单位ns
} RedChannelClientLatencyMonitor;

typedef struct RedChannelClientConnectivityMonitor {
    int state;
    uint32_t out_bytes;
    uint32_t in_bytes;
    uint32_t timeout;
    SpiceTimer *timer;
} RedChannelClientConnectivityMonitor;

struct RedChannelClient {
    RingItem channel_link; //接入到RedChannel
    RingItem client_link; //接入到RedClient
    RedChannel *channel; //所属的RedChannel
    RedClient  *client; //所属的RedClient
    RedsStream *stream; //对应的socket流，RedChannel则没有（1:N）
    int dummy; //是否为虚拟RCC，录音和回放通道使用了
    int dummy_connected; //虚拟RCC是否已经连接，用于判读RCC是否连接

    uint32_t refs; //引用计数

    struct {
        uint32_t generation; //本地会话ID，创建时没有直接初始化
        uint32_t client_generation; //客户端的会话ID
        uint32_t messages_window; //本地消息窗口
        uint32_t client_window; //客户端消息窗口
    } ack_data; //连接ACK信息

    struct {
		//公共部分是当前发送使用的数据，在发送紧急数据
		//或者恢复成发送普通数据时，这些值会进行更新
        SpiceMarshaller *marshaller; /* 当前rcc使用的mashaller，在urgent和main的mashaller之间切换 */
        SpiceDataHeaderOpaque header; //SPICE协议头操作对象
        uint32_t size; //待发送消息， 创建时没有直接初始化
        PipeItem *item; //当前待发送消息的管道项， 创建时没有直接初始化
        //以下部分切花数据发送器不会改变，是跟Redsstream相关的
        int blocked; //socket流是否发生阻塞， 创建时没有直接初始化
        uint64_t serial; //socket流消息序号，创建时没有直接初始化
        uint64_t last_sent_serial; //上次发送序号， 创建时没有直接初始化

        struct {
            SpiceMarshaller *marshaller;//主调制解调器
            uint8_t *header_data; //保存主调制解调器的数据头部缓冲区，用于恢复
            PipeItem *item; //保存主调制解调器的发送项指针，用于恢复
        } main;

        struct {
            SpiceMarshaller *marshaller; //紧急调制解调器
        } urgent; //紧急数据发送器
    } send_data; //数据发送信息

    OutgoingHandler outgoing;//数据发送器
    IncomingHandler incoming;//数据接收器
    int during_send;// 正在发送数据标记
    int id; // ID，debugging purposes，RCC标号
    Ring pipe; //发送管道
    uint32_t pipe_size; //发送管道项数

    RedChannelCapabilities remote_caps; //客户端的CAPS，包括公共的和通道的
    int is_mini_header;//是否迷你头部
    int destroying; //RCC正在销毁标记

    int wait_migrate_data; //迁移相关
    int wait_migrate_flush_mark; //迁移相关

    RedChannelClientLatencyMonitor latency_monitor; //延时监控器
    RedChannelClientConnectivityMonitor connectivity_monitor; //连接监控器
};

struct RedChannel {
    uint32_t type; //通道的类型，每种通道值不一样
    uint32_t id; //通道的id，一种通道可能有多个通道（如显示、USB重定向），一般从0开始编号

    uint32_t refs; //引用计数

    RingItem link; // channels link for reds，连接到全局的通道链表

    SpiceCoreInterface *core; //
    int handle_acks; //是否需要控制ack

    // RedChannel will hold only connected channel clients 
    // (logic - when pushing pipe item to all channel clients, 
    // there is no need to go over disconnect clients). 
    // While client will hold the channel clients till it is destroyed
    // and then it will destroy them as well.
    // However RCC still holds a reference to the Channel.
    // Maybe replace these logic with ref count?
    // TODO: rename to 'connected_clients'?
    Ring clients; //RCC链表
    uint32_t clients_num; //RCC数量,同时用作rcc的id

	/*RedChannel里有处理收发数据的接口OutgoingHandlerInterface
	  和IncomingHandlerInterface，RCC里也有OutgoingHandler和
	  IncomingHandler。那到底用哪个来收发客户端的数据呢？
	*/
    OutgoingHandlerInterface outgoing_cb; //通道数据发送到客户端连接的接口
    IncomingHandlerInterface incoming_cb; //通道接收网络数据的接口

    ChannelCbs channel_cbs; //通道的消息收发定制函数
    ClientCbs client_cbs; //通道的客户端连接、断开、迁移钩子函数

    RedChannelCapabilities local_caps; //本地能力，用于通道功能特性的协商
    uint32_t migration_flags;

    void *data; //私有数据，每个通道的含义各不相同，如声音通道指向SndWorker

    // TODO: when different channel_clients are in different threads from Channel -> need to protect!
    pthread_t thread_id;//线程id
#ifdef RED_STATISTICS
    uint64_t *out_bytes_counter;
#endif
};

/*
 * When an error occurs over a channel, we treat it as a warning
 * for spice-server and shutdown the channel.
 */
#define spice_channel_client_error(rcc, format, ...)                                     \
    do {                                                                                 \
        spice_warning("rcc %p type %u id %u: " format, rcc,                              \
                    rcc->channel->type, rcc->channel->id, ## __VA_ARGS__);               \
        red_channel_client_shutdown(rcc);                                                \
    } while (0)

/* if one of the callbacks should cause disconnect, use red_channel_shutdown and don't
 * explicitly destroy the channel */
RedChannel *red_channel_create(int size,
                               SpiceCoreInterface *core,
                               uint32_t type, uint32_t id,
                               int handle_acks,
                               channel_handle_message_proc handle_message,
                               ChannelCbs *channel_cbs,
                               uint32_t migration_flags);

/** alternative constructor, meant for marshaller based (inputs,main) channels,
 *  will become default eventually
 *  创建一个通道并设置好其消息分发函数, 会调用red_channel_create
**/
RedChannel *red_channel_create_parser(int size,
                               SpiceCoreInterface *core,
                               uint32_t type, uint32_t id,
                               int handle_acks,
                               spice_parse_channel_func_t parser,
                               channel_handle_parsed_proc handle_parsed,
                               ChannelCbs *channel_cbs,
                               uint32_t migration_flags);

void red_channel_register_client_cbs(RedChannel *channel, ClientCbs *client_cbs);
// caps are freed when the channel is destroyed
void red_channel_set_common_cap(RedChannel *channel, uint32_t cap);
void red_channel_set_cap(RedChannel *channel, uint32_t cap);
void red_channel_set_data(RedChannel *channel, void *data);

RedChannelClient *red_channel_client_create(int size, RedChannel *channel, RedClient *client,
                                            RedsStream *stream,
                                            int monitor_latency,
                                            int num_common_caps, uint32_t *common_caps,
                                            int num_caps, uint32_t *caps);
// TODO: tmp, for channels that don't use RedChannel yet (e.g., snd channel), but
// do use the client callbacks. So the channel clients are not connected (the channel doesn't
// have list of them, but they do have a link to the channel, and the client has a list of them)
RedChannel *red_channel_create_dummy(int size, uint32_t type, uint32_t id);
RedChannelClient *red_channel_client_create_dummy(int size,
                                                  RedChannel *channel,
                                                  RedClient  *client,
                                                  int num_common_caps, uint32_t *common_caps,
                                                  int num_caps, uint32_t *caps);

int red_channel_is_connected(RedChannel *channel);
int red_channel_client_is_connected(RedChannelClient *rcc);

void red_channel_client_default_migrate(RedChannelClient *rcc);
int red_channel_client_waits_for_migrate_data(RedChannelClient *rcc);
/* seamless migration is supported for only one client. This routine
 * checks if the only channel client associated with channel is
 * waiting for migration data */
int red_channel_waits_for_migrate_data(RedChannel *channel);

/*
 * the disconnect callback is called from the channel's thread,
 * i.e., for display channels - red worker thread, for all the other - from the main thread.
 * RedClient is managed from the main thread. red_channel_client_destroy can be called only
 * from red_client_destroy.
 */

void red_channel_client_destroy(RedChannelClient *rcc);
void red_channel_destroy(RedChannel *channel);

int red_channel_client_test_remote_common_cap(RedChannelClient *rcc, uint32_t cap);
int red_channel_client_test_remote_cap(RedChannelClient *rcc, uint32_t cap);

/* return true if all the channel clients support the cap */
int red_channel_test_remote_common_cap(RedChannel *channel, uint32_t cap);
int red_channel_test_remote_cap(RedChannel *channel, uint32_t cap);

/* shutdown is the only safe thing to do out of the client/channel
 * thread. It will not touch the rings, just shutdown the socket.
 * It should be followed by some way to gurantee a disconnection. */
void red_channel_client_shutdown(RedChannelClient *rcc);

/* should be called when a new channel is ready to send messages */
void red_channel_init_outgoing_messages_window(RedChannel *channel);

/* handles general channel msgs from the client */
int red_channel_client_handle_message(RedChannelClient *rcc, uint32_t size,
                                      uint16_t type, void *message);

/* when preparing send_data: should call init and then use marshaller */
void red_channel_client_init_send_data(RedChannelClient *rcc, uint16_t msg_type, PipeItem *item);

uint64_t red_channel_client_get_message_serial(RedChannelClient *channel);
void red_channel_client_set_message_serial(RedChannelClient *channel, uint64_t);

/* When sending a msg. Should first call red_channel_client_begin_send_message.
 * It will first send the pending urgent data, if there is any, and then
 * the rest of the data.
 */
void red_channel_client_begin_send_message(RedChannelClient *rcc);

/*
 * Stores the current send data, and switches to urgent send data.
 * When it begins the actual send, it will send first the urgent data
 * and afterward the rest of the data.
 * Should be called only if during the marshalling of on message,
 * the need to send another message, before, rises.
 * Important: the serial of the non-urgent sent data, will be succeeded.
 * return: the urgent send data marshaller
 */
SpiceMarshaller *red_channel_client_switch_to_urgent_sender(RedChannelClient *rcc);

/* returns -1 if we don't have an estimation */
int red_channel_client_get_roundtrip_ms(RedChannelClient *rcc);

/*
 * Checks periodically if the connection is still alive
 */
void red_channel_client_start_connectivity_monitoring(RedChannelClient *rcc, uint32_t timeout_ms);

void red_channel_pipe_item_init(RedChannel *channel, PipeItem *item, int type);

// TODO: add back the channel_pipe_add functionality - by adding reference counting
// to the PipeItem.

// helper to push a new item to all channels
typedef PipeItem *(*new_pipe_item_t)(RedChannelClient *rcc, void *data, int num);
void red_channel_pipes_new_add_push(RedChannel *channel, new_pipe_item_t creator, void *data);
void red_channel_pipes_new_add(RedChannel *channel, new_pipe_item_t creator, void *data);
void red_channel_pipes_new_add_tail(RedChannel *channel, new_pipe_item_t creator, void *data);

void red_channel_client_pipe_add_push(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add_after(RedChannelClient *rcc, PipeItem *item, PipeItem *pos);
int red_channel_client_pipe_item_is_linked(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_remove_and_release(RedChannelClient *rcc, PipeItem *item);
void red_channel_client_pipe_add_tail(RedChannelClient *rcc, PipeItem *item);
/* for types that use this routine -> the pipe item should be freed */
void red_channel_client_pipe_add_type(RedChannelClient *rcc, int pipe_item_type);
void red_channel_pipes_add_type(RedChannel *channel, int pipe_item_type);

void red_channel_client_pipe_add_empty_msg(RedChannelClient *rcc, int msg_type);
void red_channel_pipes_add_empty_msg(RedChannel *channel, int msg_type);

void red_channel_client_ack_zero_messages_window(RedChannelClient *rcc);
void red_channel_client_ack_set_client_window(RedChannelClient *rcc, int client_window);
void red_channel_client_push_set_ack(RedChannelClient *rcc);
void red_channel_push_set_ack(RedChannel *channel);

int red_channel_get_first_socket(RedChannel *channel);

/* return TRUE if all of the connected clients to this channel are blocked */
int red_channel_all_blocked(RedChannel *channel);

/* return TRUE if any of the connected clients to this channel are blocked */
int red_channel_any_blocked(RedChannel *channel);

int red_channel_client_blocked(RedChannelClient *rcc);

/* helper for channels that have complex logic that can possibly ready a send */
int red_channel_client_send_message_pending(RedChannelClient *rcc);

int red_channel_no_item_being_sent(RedChannel *channel);
int red_channel_client_no_item_being_sent(RedChannelClient *rcc);

void red_channel_pipes_remove(RedChannel *channel, PipeItem *item);

// TODO: unstaticed for display/cursor channels. they do some specific pushes not through
// adding elements or on events. but not sure if this is actually required (only result
// should be that they ""try"" a little harder, but if the event system is correct it
// should not make any difference.
void red_channel_push(RedChannel *channel);
void red_channel_client_push(RedChannelClient *rcc);
// TODO: again - what is the context exactly? this happens in channel disconnect. but our
// current red_channel_shutdown also closes the socket - is there a socket to close?
// are we reading from an fd here? arghh
void red_channel_client_pipe_clear(RedChannelClient *rcc);
// Again, used in various places outside of event handler context (or in other event handler
// contexts):
//  flush_display_commands/flush_cursor_commands
//  display_channel_wait_for_init
//  red_wait_outgoing_item
//  red_wait_pipe_item_sent
//  handle_channel_events - this is the only one that was used before, and was in red_channel.c
void red_channel_receive(RedChannel *channel);
void red_channel_client_receive(RedChannelClient *rcc);
// For red_worker
void red_channel_send(RedChannel *channel);
void red_channel_client_send(RedChannelClient *rcc);
// For red_worker
void red_channel_disconnect(RedChannel *channel);
void red_channel_client_disconnect(RedChannelClient *rcc);

/* accessors for RedChannelClient */
/* Note: the valid times to call red_channel_get_marshaller are just during send_item callback. */
SpiceMarshaller *red_channel_client_get_marshaller(RedChannelClient *rcc);
RedsStream *red_channel_client_get_stream(RedChannelClient *rcc);
RedClient *red_channel_client_get_client(RedChannelClient *rcc);

/* Note that the header is valid only between red_channel_reset_send_data and
 * red_channel_begin_send_message.*/
void red_channel_client_set_header_sub_list(RedChannelClient *rcc, uint32_t sub_list);

/* return the sum of all the rcc pipe size */
uint32_t red_channel_max_pipe_size(RedChannel *channel);
/* return the min size of all the rcc pipe */
uint32_t red_channel_min_pipe_size(RedChannel *channel);
/* return the max size of all the rcc pipe */
uint32_t red_channel_sum_pipes_size(RedChannel *channel);

/* apply given function to all connected clients */
typedef void (*channel_client_callback)(RedChannelClient *rcc);
typedef void (*channel_client_callback_data)(RedChannelClient *rcc, void *data);
void red_channel_apply_clients(RedChannel *channel, channel_client_callback v);
void red_channel_apply_clients_data(RedChannel *channel, channel_client_callback_data v, void *data);

struct RedClient {
    RingItem link;
    Ring channels; //本客户端拥有的RCC列表
    int channels_num; //本客户端拥有的RCC数量
    MainChannelClient *mcc; //主通道RCC, 指针说明一个RedClient只有一个主通道RCC

	// different channels can be in different threads
	// 
    pthread_mutex_t lock; 
    		

    pthread_t thread_id;

    int disconnecting; /*RedClient是否正在断开*/
    /* Note that while semi-seamless migration is conducted by the main thread, seamless migration
     * involves all channels, and thus the related varaibles can be accessed from different
     * threads */
    int during_target_migrate; /* if seamless=TRUE, migration_target is turned off when all
                                  the clients received their migration data. Otherwise (semi-seamless),
                                  it is turned off, when red_client_semi_seamless_migrate_complete
                                  is called */
    int seamless_migrate;
    int num_migrated_channels; /* for seamless - number of channels that wait for migrate data*/
    int refs;
};

RedClient *red_client_new(int migrated);

/*
 * disconnects all the client's channels (should be called from the client's thread)
 */
void red_client_destroy(RedClient *client);

RedClient *red_client_ref(RedClient *client);

/*
 * releases the client resources when refs == 0.
 * We assume the red_client_derstroy was called before
 * we reached refs==0
 */
RedClient *red_client_unref(RedClient *client);

MainChannelClient *red_client_get_main(RedClient *client);
// main should be set once before all the other channels are created
void red_client_set_main(RedClient *client, MainChannelClient *mcc);

/* called when the migration handshake results in seamless migration (dst side).
 * By default we assume semi-seamless */
void red_client_set_migration_seamless(RedClient *client);
void red_client_semi_seamless_migrate_complete(RedClient *client); /* dst side */
/* TRUE if the migration is seamless and there are still channels that wait from migration data.
 * Or, during semi-seamless migration, and the main channel still waits for MIGRATE_END
 * from the client.
 * Note: Call it only from the main thread */
int red_client_during_migrate_at_target(RedClient *client);

void red_client_migrate(RedClient *client);

/*
 * blocking functions.
 *
 * timeout is in nano sec. -1 for no timeout.
 *
 * Return: TRUE if waiting succeeded. FALSE if timeout expired.
 */

int red_channel_client_wait_pipe_item_sent(RedChannelClient *rcc,
                                           PipeItem *item,
                                           int64_t timeout);
int red_channel_client_wait_outgoing_item(RedChannelClient *rcc,
                                          int64_t timeout);
int red_channel_wait_all_sent(RedChannel *channel,
                              int64_t timeout);

#endif
