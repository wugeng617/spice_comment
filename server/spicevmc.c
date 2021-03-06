/* spice-server spicevmc passthrough channel code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h> // IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY

#include "common/generated_server_marshallers.h"

#include "char_device.h"
#include "red_channel.h"
#include "reds.h"
#include "migration_protocol.h"

/* todo: add flow control. i.e.,
 * (a) limit the tokens available for the client
 * (b) limit the tokens available for the server
 */
/* 64K should be enough for all but the largest writes + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)

// SpiceVmc管道项，因为只有数据，所以只有一个缓冲区
typedef struct SpiceVmcPipeItem {
    PipeItem base;
    uint32_t refs;

    /* writes which don't fit this will get split, this is not a problem */
    uint8_t buf[BUF_SIZE]; /* 字符设备32字节头部 + 最多64K数据 */
    uint32_t buf_used;
} SpiceVmcPipeItem;

// SpiceVmcState直接包含一个通道对象
typedef struct SpiceVmcState {
    RedChannel channel; /* Must be the first item */
    RedChannelClient *rcc; //只能支持一个客户端连接，所以直接存放RCC
    SpiceCharDeviceState *chardev_st; //字符设备结构体指针
    SpiceCharDeviceInstance *chardev_sin; //保存实例指针
    SpiceVmcPipeItem *pipe_item; //缓存VPI用于下次读取数据时使用
    SpiceCharDeviceWriteBuffer *recv_from_client_buf; //字符设备的写入缓冲区，
    //就是vmc从客户端直接接受数据的缓冲区，接受的数据直接放到字符设备写入缓冲区
    uint8_t port_opened;
} SpiceVmcState;

typedef struct PortInitPipeItem {
    PipeItem base;
    char* name;
    uint8_t opened;
} PortInitPipeItem;

typedef struct PortEventPipeItem {
    PipeItem base;
    uint8_t event;
} PortEventPipeItem;

enum {
    PIPE_ITEM_TYPE_SPICEVMC_DATA = PIPE_ITEM_TYPE_CHANNEL_BASE, //数据管道项
    PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA, //迁移数据管道项
    PIPE_ITEM_TYPE_PORT_INIT, //PORT初始化管道项
    PIPE_ITEM_TYPE_PORT_EVENT, //端口事件管道项
};

//VPI引用
static SpiceVmcPipeItem *spicevmc_pipe_item_ref(SpiceVmcPipeItem *item)
{
    item->refs++;
    return item;
}

//VPI解引用，最后会释放item
static void spicevmc_pipe_item_unref(SpiceVmcPipeItem *item)
{
    if (!--item->refs) {
        free(item);
    }
}

//服务端消息引用
SpiceCharDeviceMsgToClient *spicevmc_chardev_ref_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                               void *opaque)
{
    return spicevmc_pipe_item_ref((SpiceVmcPipeItem *)msg);
}

//服务端消息解引用
static void spicevmc_chardev_unref_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                 void *opaque)
{
    spicevmc_pipe_item_unref((SpiceVmcPipeItem *)msg);
}

/**
 * spicevmc_chardev_read_msg_from_dev - 从字符设备里把数据读取到缓冲区里
 * 返回void 指针，vmc里返回的就是VPI的地址。也就是说
 * sin spice字符设备实例
 * opaque 占位指针，在vmc字符设备里就是VS（SpiceVmcState）指针
**/
static SpiceCharDeviceMsgToClient *
spicevmc_chardev_read_msg_from_dev(SpiceCharDeviceInstance *sin, void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceCharDeviceInterface *sif;
    SpiceVmcPipeItem *msg_item;
    int n;

    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (!state->rcc) { //未连接，则返回空消息
        return NULL;
    }
	/* state-pipe_item是缓冲的管道项，从字符设备读数据失败时，会缓存下来*/
    if (!state->pipe_item) {//没有管道项，先新建一个管道项
        msg_item = spice_new0(SpiceVmcPipeItem, 1);
        msg_item->refs = 1;
        red_channel_pipe_item_init(&state->channel,
                                   &msg_item->base, PIPE_ITEM_TYPE_SPICEVMC_DATA);
    } else {
		/* 缓存的VPI一定没有有效数据 */
        spice_assert(state->pipe_item->buf_used == 0); 
        msg_item = state->pipe_item;
		/* 取出VPI后置空缓存VPI指针 */
        state->pipe_item = NULL;
    }

	/* 从字符设备的读取数据 */
    n = sif->read(sin, msg_item->buf,
                  sizeof(msg_item->buf));
    if (n > 0) {
        spice_debug("read from dev %d", n);
        msg_item->buf_used = n;
        return msg_item;
    } else { //读取失败返回
        state->pipe_item = msg_item;
        return NULL;
    }
}

// 将服务端消息发给制定
static void spicevmc_chardev_send_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                 RedClient *client,
                                                 void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceVmcPipeItem *vmc_msg = msg;

    spice_assert(state->rcc->client == client);
    spicevmc_pipe_item_ref(vmc_msg);
    red_channel_client_pipe_add_push(state->rcc, &vmc_msg->base);
}

static void spicevmc_port_send_init(RedChannelClient *rcc)
{
    SpiceVmcState *state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    SpiceCharDeviceInstance *sin = state->chardev_sin;
    PortInitPipeItem *item = spice_malloc(sizeof(PortInitPipeItem));

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_PORT_INIT);
    item->name = strdup(sin->portname);
    item->opened = state->port_opened;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void spicevmc_port_send_event(RedChannelClient *rcc, uint8_t event)
{
    PortEventPipeItem *item = spice_malloc(sizeof(PortEventPipeItem));

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_PORT_EVENT);
    item->event = event;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void spicevmc_char_dev_send_tokens_to_client(RedClient *client,
                                                    uint32_t tokens,
                                                    void *opaque)
{
    spice_printerr("Not implemented!");
}

static void spicevmc_char_dev_remove_client(RedClient *client, void *opaque)
{
    SpiceVmcState *state = opaque;

    spice_printerr("vmc state %p, client %p", state, client);
    spice_assert(state->rcc && state->rcc->client == client);

    red_channel_client_shutdown(state->rcc);
}

static int spicevmc_red_channel_client_config_socket(RedChannelClient *rcc)
{
    int delay_val = 1;
    RedsStream *stream = red_channel_client_get_stream(rcc);

    if (rcc->channel->type == SPICE_CHANNEL_USBREDIR) {
        if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY,
                &delay_val, sizeof(delay_val)) != 0) {
            if (errno != ENOTSUP && errno != ENOPROTOOPT) {
                spice_printerr("setsockopt failed, %s", strerror(errno));
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void spicevmc_red_channel_client_on_disconnect(RedChannelClient *rcc)
{
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    if (!rcc) {
        return;
    }

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (state->chardev_st) {
        if (spice_char_device_client_exists(state->chardev_st, rcc->client)) {
            spice_char_device_client_remove(state->chardev_st, rcc->client);
        } else {
            spice_printerr("client %p have already been removed from char dev %p",
                           rcc->client, state->chardev_st);
        }
    }

    /* Don't destroy the rcc if it is already being destroyed, as then
       red_client_destroy/red_channel_client_destroy will already do this! */
    if (!rcc->destroying)
        red_channel_client_destroy(rcc);

    state->rcc = NULL;
    if (sif->state) {
        sif->state(sin, 0);
    }
}

// RCC 转SpiceVmcState
static SpiceVmcState *spicevmc_red_channel_client_get_state(RedChannelClient *rcc)
{
    return SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
}

static int spicevmc_channel_client_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA);
    return TRUE;
}

static int spicevmc_channel_client_handle_migrate_data(RedChannelClient *rcc,
                                                       uint32_t size, void *message)
{
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataSpiceVmc *mig_data;
    SpiceVmcState *state;

    state = spicevmc_red_channel_client_get_state(rcc);

    header = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataSpiceVmc *)(header + 1);
    spice_assert(size >= sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataSpiceVmc));

    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_SPICEVMC_MAGIC,
                                            SPICE_MIGRATE_DATA_SPICEVMC_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return spice_char_device_state_restore(state->chardev_st, &mig_data->base);
}

// 默认的消息分发函数
static int spicevmc_red_channel_client_handle_message(RedChannelClient *rcc,
                                                      uint16_t type,
                                                      uint32_t size,
                                                      uint8_t *msg)
{
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = spicevmc_red_channel_client_get_state(rcc);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        spice_assert(state->recv_from_client_buf->buf == msg);
        state->recv_from_client_buf->buf_used = size;
        spice_char_device_write_buffer_add(state->chardev_st, state->recv_from_client_buf);
        state->recv_from_client_buf = NULL;
        break;
    case SPICE_MSGC_PORT_EVENT:
        if (size != sizeof(uint8_t)) {
            spice_warning("bad port event message size");
            return FALSE;
        }
        if (sif->base.minor_version >= 2 && sif->event != NULL)
            sif->event(sin, *msg);
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, msg);
    }

    return TRUE;
}

// 为type类型的消息分配size大小的接收缓冲区。
// 该函数触发了两块堆内存的分配。
static uint8_t *spicevmc_red_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                                       uint16_t type,
                                                       uint32_t size)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        assert(!state->recv_from_client_buf); //要求接受缓冲区为空
		//调用字符设备结构获取接受缓冲区内存
        state->recv_from_client_buf = spice_char_device_write_buffer_get(state->chardev_st,
                                                                         rcc->client,
                                                                         size);
        if (!state->recv_from_client_buf) {
            spice_error("failed to allocate write buffer");
            return NULL;
        }
        return state->recv_from_client_buf->buf; //这块内存是接收消息的实际存放地方

    default:
        return spice_malloc(size);
    }

}

// 为type类型的
static void spicevmc_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                     uint16_t type,
                                                     uint32_t size,
                                                     uint8_t *msg)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        if (state->recv_from_client_buf) { /* buffer wasn't pushed to device */
            spice_char_device_write_buffer_release(state->chardev_st, state->recv_from_client_buf);
            state->recv_from_client_buf = NULL;
        }
        break;
    default:
        free(msg);
    }
}

//初始化rcc的send_data时会调用hold_item接口
static void spicevmc_red_channel_hold_pipe_item(RedChannelClient *rcc,
    PipeItem *item)
{
    /* NOOP */
}

// spicevmc发送usb映射数据
static void spicevmc_red_channel_send_data(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           PipeItem *item)
{
    SpiceVmcPipeItem *i = SPICE_CONTAINEROF(item, SpiceVmcPipeItem, base);

    red_channel_client_init_send_data(rcc, SPICE_MSG_SPICEVMC_DATA, item);
    spice_marshaller_add_ref(m, i->buf, i->buf_used);
}

static void spicevmc_red_channel_send_migrate_data(RedChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   PipeItem *item)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, item);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_VERSION);

    spice_char_device_state_migrate_data_marshall(state->chardev_st, m);
}

static void spicevmc_red_channel_send_port_init(RedChannelClient *rcc,
                                                SpiceMarshaller *m,
                                                PipeItem *item)
{
    PortInitPipeItem *i = SPICE_CONTAINEROF(item, PortInitPipeItem, base);
    SpiceMsgPortInit init;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_INIT, item);
    init.name = (uint8_t *)i->name;
    init.name_size = strlen(i->name) + 1;
    init.opened = i->opened;
    spice_marshall_msg_port_init(m, &init);
}

static void spicevmc_red_channel_send_port_event(RedChannelClient *rcc,
                                                 SpiceMarshaller *m,
                                                 PipeItem *item)
{
    PortEventPipeItem *i = SPICE_CONTAINEROF(item, PortEventPipeItem, base);
    SpiceMsgPortEvent event;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_EVENT, item);
    event.event = i->event;
    spice_marshall_msg_port_event(m, &event);
}

static void spicevmc_red_channel_send_item(RedChannelClient *rcc,
                                           PipeItem *item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (item->type) {
    case PIPE_ITEM_TYPE_SPICEVMC_DATA:
        spicevmc_red_channel_send_data(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA:
        spicevmc_red_channel_send_migrate_data(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_PORT_INIT:
        spicevmc_red_channel_send_port_init(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_PORT_EVENT:
        spicevmc_red_channel_send_port_event(rcc, m, item);
        break;
    default:
        spice_error("bad pipe item %d", item->type);
        free(item);
        return;
    }
    red_channel_client_begin_send_message(rcc);
}

static void spicevmc_red_channel_release_pipe_item(RedChannelClient *rcc,
    PipeItem *item, int item_pushed)
{
    if (item->type == PIPE_ITEM_TYPE_SPICEVMC_DATA) {
        spicevmc_pipe_item_unref((SpiceVmcPipeItem *)item);
    } else {
        free(item);
    }
}

// 通道连接时的回调函数
static void spicevmc_connect(RedChannel *channel, RedClient *client,
    RedsStream *stream, int migration, int num_common_caps,
    uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    RedChannelClient *rcc;
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(channel, SpiceVmcState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (state->rcc) {
        spice_printerr("channel client %d:%d (%p) already connected, refusing second connection",
                       channel->type, channel->id, state->rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        reds_stream_free(stream);
        return;
    }

    rcc = red_channel_client_create(sizeof(RedChannelClient), channel, client, stream,
                                    FALSE,
                                    num_common_caps, common_caps,
                                    num_caps, caps);
    if (!rcc) {
        return;
    }
    state->rcc = rcc;
    red_channel_client_ack_zero_messages_window(rcc);

    if (strcmp(sin->subtype, "port") == 0) {
        spicevmc_port_send_init(rcc);
    }

    if (!spice_char_device_client_add(state->chardev_st, client, FALSE, 0, ~0, ~0,
                                      red_channel_client_waits_for_migrate_data(rcc))) {
        spice_warning("failed to add client to spicevmc");
        red_channel_client_disconnect(rcc);
        return;
    }

    if (sif->state) {
        sif->state(sin, 1);
    }
}

// 模块入口函数，虚拟机启动时执行
SpiceCharDeviceState *spicevmc_device_connect(SpiceCharDeviceInstance *sin,
                                              uint8_t channel_type)
{
    static uint8_t id[256] = { 0, };//每次通道创建都会增加一个id值，从0开始
    SpiceVmcState *state;
    ChannelCbs channel_cbs = { NULL, };
    ClientCbs client_cbs = { NULL, };
    SpiceCharDeviceCallbacks char_dev_cbs = {NULL, };

    channel_cbs.config_socket = spicevmc_red_channel_client_config_socket;
    channel_cbs.on_disconnect = spicevmc_red_channel_client_on_disconnect;
    channel_cbs.send_item = spicevmc_red_channel_send_item; //怎么发送管道项
    channel_cbs.hold_item = spicevmc_red_channel_hold_pipe_item;
    channel_cbs.release_item = spicevmc_red_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = spicevmc_red_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = spicevmc_red_channel_release_msg_rcv_buf;
    channel_cbs.handle_migrate_flush_mark = spicevmc_channel_client_handle_migrate_flush_mark;
    channel_cbs.handle_migrate_data = spicevmc_channel_client_handle_migrate_data;

	// SpiceVmcState是RedChannel的子类，core在reds.h中声明，在reds.c中定义
	// 通道类型，自然是usb映射，没记错的话VDI里是9，id从静态变量自增，
    state = (SpiceVmcState*)red_channel_create(sizeof(SpiceVmcState),
                                   core, channel_type, id[channel_type]++,
                                   FALSE /* handle_acks， 不做ack控制流控*/,
                                   spicevmc_red_channel_client_handle_message, //默认的消息分发
                                   &channel_cbs,
                                   SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER);
    red_channel_init_outgoing_messages_window(&state->channel);

    client_cbs.connect = spicevmc_connect;
    red_channel_register_client_cbs(&state->channel, &client_cbs);

    char_dev_cbs.read_one_msg_from_device = spicevmc_chardev_read_msg_from_dev;
    char_dev_cbs.ref_msg_to_client = spicevmc_chardev_ref_msg_to_client;
    char_dev_cbs.unref_msg_to_client = spicevmc_chardev_unref_msg_to_client;
    char_dev_cbs.send_msg_to_client = spicevmc_chardev_send_msg_to_client;
    char_dev_cbs.send_tokens_to_client = spicevmc_char_dev_send_tokens_to_client;
    char_dev_cbs.remove_client = spicevmc_char_dev_remove_client;

    state->chardev_st = spice_char_device_state_create(sin,
                                                       0, /* tokens interval */
                                                       ~0, /* self tokens */
                                                       &char_dev_cbs,
                                                       state);
    state->chardev_sin = sin;

    reds_register_channel(&state->channel);
    return state->chardev_st;
}

/* Must be called from RedClient handling thread. */
void spicevmc_device_disconnect(SpiceCharDeviceInstance *sin)
{
    SpiceVmcState *state;

    state = (SpiceVmcState *)spice_char_device_state_opaque_get(sin->st);

    if (state->recv_from_client_buf) {
        spice_char_device_write_buffer_release(state->chardev_st, state->recv_from_client_buf);
    }
    spice_char_device_state_destroy(sin->st);
    state->chardev_st = NULL;

    reds_unregister_channel(&state->channel);
    free(state->pipe_item);
    red_channel_destroy(&state->channel);
}

SPICE_GNUC_VISIBLE void spice_server_port_event(SpiceCharDeviceInstance *sin, uint8_t event)
{
    SpiceVmcState *state;

    state = (SpiceVmcState *)spice_char_device_state_opaque_get(sin->st);
    if (event == SPICE_PORT_EVENT_OPENED) {
        state->port_opened = TRUE;
    } else if (event == SPICE_PORT_EVENT_CLOSED) {
        state->port_opened = FALSE;
    }

    if (state->rcc == NULL) {
        return;
    }

    spicevmc_port_send_event(state->rcc, event);
}
