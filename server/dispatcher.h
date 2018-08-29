#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <spice.h>

typedef struct Dispatcher Dispatcher;

//使用消息分发器中占位指针和载荷指针来执行消息响应
//消息响应函数和消息类型一一对应
typedef void (*dispatcher_handle_message)(void *opaque,
                                          void *payload);

typedef void (*dispatcher_handle_async_done)(void *opaque,
                                             uint32_t message_type,
                                             void *payload);


typedef struct DispatcherMessage {
    size_t size; //大小
    int ack; //是不是要ack
    dispatcher_handle_message handler; //消息句柄
} DispatcherMessage;

// 一次消息分发，包括把消息写到send_fd，如果是同步消息，则要等消息响应返回
struct Dispatcher {
    SpiceCoreInterface *recv_core;
    int recv_fd; //消息响应fd
    int send_fd; //消息分发fd，消息通过这个fd发送给消息接受例程
    pthread_t self; //消息线程id，创建Dispatcher的线程id
    pthread_mutex_t lock; //消息发送锁，一次只能发送消息发送完才解锁
    DispatcherMessage *messages; //消息分发器支持的消息列表数组，使用消息类型来定位
    int stage;  /* message parser stage - sender has no stages */
    size_t max_message_type; //支持的消息类型的最大值，或超过
    void *payload; /* allocated as max of message sizes 消息缓冲区内存，存放消息*/
    size_t payload_size; /* used to track realloc calls 消息缓冲区的大小*/
    void *opaque; //消息分发时的占位指针，初始化设置，也可以修改
    dispatcher_handle_async_done handle_async_done; //消息异步处理函数
};

/*
 * dispatcher_send_message
 * @message_type: message type
 * @payload:      payload
 */
void dispatcher_send_message(Dispatcher *dispatcher, uint32_t message_type,
                             void *payload);

/*
 * dispatcher_init
 * @max_message_type: number of message types. Allows upfront allocation
 *  of a DispatcherMessage list.
 * up front, and registration in any order wanted.
 */
void dispatcher_init(Dispatcher *dispatcher, size_t max_message_type,
                     void *opaque);
/* Dispatcher支持3种消息分发类型 */
enum {
    DISPATCHER_NONE = 0, //消息分发
    DISPATCHER_ACK, //消息分发后等待消息的处理结果
    DISPATCHER_ASYNC //消息分发后不等消息被处理完成直接返回，消息的处理是异步的
};

/*
 * dispatcher_register_handler
 * @dispatcher:     dispatcher
 * @messsage_type:  message type
 * @handler:        message handler
 * @size:           message size. Each type has a fixed associated size.
 * @ack:            One of DISPATCHER_NONE, DISPATCHER_ACK, DISPATCHER_ASYNC.
 *                  DISPATCHER_NONE - only send the message
 *                  DISPATCHER_ACK - send an ack after the message
 *                  DISPATCHER_ASYNC - call send an ack. This is per message type - you can't send the
 *                  same message type with and without. Register two different
 *                  messages if that is what you want.
 */
void dispatcher_register_handler(Dispatcher *dispatcher, uint32_t message_type,
                                 dispatcher_handle_message handler, size_t size,
                                 int ack);

/*
 * dispatcher_register_async_done_callback
 * @dispatcher:     dispatcher
 * @handler:        callback on the receiver side called *after* the
 *                  message callback in case ack == DISPATCHER_ASYNC.
 */
void dispatcher_register_async_done_callback(
                                    Dispatcher *dispatcher,
                                    dispatcher_handle_async_done handler);

/*
 *  dispatcher_handle_recv_read
 *  @dispatcher: Dispatcher instance
 */
void dispatcher_handle_recv_read(Dispatcher *);

/*
 *  dispatcher_get_recv_fd
 *  @return: receive file descriptor of the dispatcher
 */
int dispatcher_get_recv_fd(Dispatcher *);

/*
 * dispatcher_set_opaque
 * @dispatcher: Dispatcher instance
 * @opaque: opaque to use for callbacks
 */
void dispatcher_set_opaque(Dispatcher *dispatcher, void *opaque);

#endif //DISPATCHER_H
