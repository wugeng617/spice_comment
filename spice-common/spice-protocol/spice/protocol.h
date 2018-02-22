/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _H_SPICE_PROTOCOL
#define _H_SPICE_PROTOCOL

#include <spice/types.h>
#include <spice/enums.h>
#include <spice/start-packed.h> //这个代码比较有意思，把PACK定义放到头文件里进行包裹

#define SPICE_MAGIC (*(uint32_t*)"REDQ") //SPICE魔数
#define SPICE_VERSION_MAJOR 2 //协议主版本号2
#define SPICE_VERSION_MINOR 2 //协议小版本号2

// Encryption & Ticketing Parameters
#define SPICE_MAX_PASSWORD_LENGTH 60 //最大密码长度
#define SPICE_TICKET_KEY_PAIR_LENGTH 1024 //票据密钥对长度
#define SPICE_TICKET_PUBKEY_BYTES (SPICE_TICKET_KEY_PAIR_LENGTH / 8 + 34) //公

typedef struct SPICE_ATTR_PACKED SpiceLinkHeader {
    uint32_t magic; //LINK魔数
    uint32_t major_version; //SPICE主版本号
    uint32_t minor_version; //SPICE小版本号
    uint32_t size; //LINK消息大小
} SpiceLinkHeader;

//公共特性定义
enum {
    SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION, //是否支持协议认证选择
    SPICE_COMMON_CAP_AUTH_SPICE, //SPICE认知
    SPICE_COMMON_CAP_AUTH_SASL, //是否支持SASL认证
    SPICE_COMMON_CAP_MINI_HEADER, //是否支持迷你头
};

//LINK消息请求结构体
typedef struct SPICE_ATTR_PACKED SpiceLinkMess {
    uint32_t connection_id; //连接ID
    uint8_t channel_type; //通道类型
    uint8_t channel_id; //通道ID
    uint32_t num_common_caps; //公共特性bitmap的长度
    uint32_t num_channel_caps; //通道特性bitmap的长度
    uint32_t caps_offset; //特性bitmap的偏移量
} SpiceLinkMess;

//LINk消息响应结构体
typedef struct SPICE_ATTR_PACKED SpiceLinkReply {
    uint32_t error; //请求响应值
    uint8_t pub_key[SPICE_TICKET_PUBKEY_BYTES];//认证的公钥
    uint32_t num_common_caps; //返回服务端的公共特性bitmap长度
    uint32_t num_channel_caps;//返回服务端的通道特性bitmap长度
    uint32_t caps_offset; //特性bitmap的偏移量
} SpiceLinkReply;

typedef struct SPICE_ATTR_PACKED SpiceLinkEncryptedTicket {
    uint8_t encrypted_data[SPICE_TICKET_KEY_PAIR_LENGTH / 8];
} SpiceLinkEncryptedTicket;

typedef struct SPICE_ATTR_PACKED SpiceLinkAuthMechanism {
    uint32_t auth_mechanism;
} SpiceLinkAuthMechanism;

//标准数据头
typedef struct SPICE_ATTR_PACKED SpiceDataHeader {
    uint64_t serial; //序号
    uint16_t type; //消息类型
    uint32_t size; //消息尺寸
    uint32_t sub_list; //offset to SpiceSubMessageList[]
} SpiceDataHeader;

//迷你数据头-VDI使用此头部
typedef struct SPICE_ATTR_PACKED SpiceMiniDataHeader {
    uint16_t type; //消息类型
    uint32_t size; //消息尺寸
} SpiceMiniDataHeader;

// 子消息 - 还不知道啥意思
typedef struct SPICE_ATTR_PACKED SpiceSubMessage {
    uint16_t type; //消息类型
    uint32_t size; //消息大小
} SpiceSubMessage;

// 子消息列表
typedef struct SPICE_ATTR_PACKED SpiceSubMessageList {
    uint16_t size; //子消息个数
    uint32_t sub_messages[0]; //offsets to SpicedSubMessage
} SpiceSubMessageList;

#define SPICE_INPUT_MOTION_ACK_BUNCH 4 //INPUT通累积ACK个数

// 回放通道的特性定义
enum {
    SPICE_PLAYBACK_CAP_CELT_0_5_1, //回放数据是否支持CELT压缩
    SPICE_PLAYBACK_CAP_VOLUME, //能否调节音量
    SPICE_PLAYBACK_CAP_LATENCY, //是否支持延时反馈
    SPICE_PLAYBACK_CAP_OPUS, //是否支持OPUS压缩
};

// 录音通道的特性定义
enum {
    SPICE_RECORD_CAP_CELT_0_5_1, //录音数据是否支持CELT压缩
    SPICE_RECORD_CAP_VOLUME, //录音是否支持调节音量
    SPICE_RECORD_CAP_OPUS, //录音是否支持OPUS压缩
};

// 主通道的特性定义
enum {
    SPICE_MAIN_CAP_SEMI_SEAMLESS_MIGRATE, //是否支持半无缝迁移
    SPICE_MAIN_CAP_NAME_AND_UUID, //是否支持名称和UUID
    SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS, //是否支持AGENT连接令牌
    SPICE_MAIN_CAP_SEAMLESS_MIGRATE, //是否支持无缝迁移
};

// 显示通道的特性定义
enum {
    SPICE_DISPLAY_CAP_SIZED_STREAM, //是否支持流
    SPICE_DISPLAY_CAP_MONITORS_CONFIG, //是否支持显示器配置
    SPICE_DISPLAY_CAP_COMPOSITE, //组合
    SPICE_DISPLAY_CAP_A8_SURFACE, //A8 SURFACE
    SPICE_DISPLAY_CAP_STREAM_REPORT, //流上报
};

// 输入通道的特性定义
enum {
    SPICE_INPUTS_CAP_KEY_SCANCODE, //键盘是否支持扫描码
};

// PORT事件定义
enum {
    SPICE_PORT_EVENT_OPENED, //打开事件
    SPICE_PORT_EVENT_CLOSED, //关闭事件
    SPICE_PORT_EVENT_BREAK, //断开事件
};

#include <spice/end-packed.h>

#endif /* _H_SPICE_PROTOCOL */
