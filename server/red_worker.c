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

#define SPICE_LOG_DOMAIN "SpiceWorker"

/* Common variable abberiviations:
 *
 * rcc - RedChannelClient
 * ccc - CursorChannelClient (not to be confused with common_cc)
 * common_cc - CommonChannelClient
 * dcc - DisplayChannelClient
 * cursor_red_channel - downcast of CursorChannel to RedChannel
 * display_red_channel - downcast of DisplayChannel to RedChannel
 */

#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <setjmp.h>
#include <openssl/ssl.h>
#include <inttypes.h>

#include <spice/protocol.h>
#include <spice/qxl_dev.h>
#include "common/lz.h"
#include "common/marshaller.h"
#include "common/quic.h"
#include "common/rect.h"
#include "common/region.h"
#include "common/ring.h"
#include "common/generated_server_marshallers.h"

#ifdef USE_OPENGL
#include "common/ogl_ctx.h"
#include "reds_gl_canvas.h"
#endif /* USE_OPENGL */

#include "spice.h"
#include "red_worker.h"
#include "reds_stream.h"
#include "reds_sw_canvas.h"
#include "glz_encoder_dictionary.h"
#include "glz_encoder.h"
#include "stat.h"
#include "reds.h"
#include "mjpeg_encoder.h"
#include "red_memslots.h"
#include "red_parse_qxl.h"
#include "jpeg_encoder.h"
#include "demarshallers.h"
#include "zlib_encoder.h"
#include "red_channel.h"
#include "red_dispatcher.h"
#include "dispatcher.h"
#include "main_channel.h"
#include "migration_protocol.h"
#include "spice_timer_queue.h"
#include "main_dispatcher.h"
#include "spice_server_utils.h"
#include "red_time.h"
#include "spice_bitmap_utils.h"
#include "spice_image_cache.h"

//#define COMPRESS_STAT
//#define DUMP_BITMAP
//#define PIPE_DEBUG
//#define RED_WORKER_STAT
/* TODO: DRAW_ALL is broken. */
//#define DRAW_ALL
//#define COMPRESS_DEBUG
//#define ACYCLIC_SURFACE_DEBUG
//#define DEBUG_CURSORS

//#define UPDATE_AREA_BY_TREE

#define CMD_RING_POLL_TIMEOUT 10 //milli
#define CMD_RING_POLL_RETRIES 200

#define DISPLAY_CLIENT_SHORT_TIMEOUT 15000000000ULL //nano
#define DISPLAY_CLIENT_TIMEOUT 30000000000ULL //nano
#define DISPLAY_CLIENT_MIGRATE_DATA_TIMEOUT 10000000000ULL //nano, 10 sec
#define DISPLAY_CLIENT_RETRY_INTERVAL 10000 //micro

#define DISPLAY_FREE_LIST_DEFAULT_SIZE 128

#define RED_STREAM_DETACTION_MAX_DELTA ((1000 * 1000 * 1000) / 5) // 1/5 sec
#define RED_STREAM_CONTINUS_MAX_DELTA (1000 * 1000 * 1000)
#define RED_STREAM_TIMOUT (1000 * 1000 * 1000)
#define RED_STREAM_FRAMES_START_CONDITION 20
#define RED_STREAM_GRADUAL_FRAMES_START_CONDITION 0.2
#define RED_STREAM_FRAMES_RESET_CONDITION 100
#define RED_STREAM_MIN_SIZE (96 * 96)
#define RED_STREAM_INPUT_FPS_TIMEOUT (5 * 1000) // 5 sec
#define RED_STREAM_CHANNEL_CAPACITY 0.8
/* the client's stream report frequency is the minimum of the 2 values below */
#define RED_STREAM_CLIENT_REPORT_WINDOW 5 // #frames
#define RED_STREAM_CLIENT_REPORT_TIMEOUT 1000 // milliseconds
#define RED_STREAM_DEFAULT_HIGH_START_BIT_RATE (10 * 1024 * 1024) // 10Mbps
#define RED_STREAM_DEFAULT_LOW_START_BIT_RATE (2.5 * 1024 * 1024) // 2.5Mbps

#define FPS_TEST_INTERVAL 1
#define MAX_FPS 30

#define RED_COMPRESS_BUF_SIZE (1024 * 64)

#define ZLIB_DEFAULT_COMPRESSION_LEVEL 3
#define MIN_GLZ_SIZE_FOR_ZLIB 100

#define RECT_FMT "(%d,%d,%d,%d)(%dx%d)"
#define RECT_PARAM(rect) ((rect)->left, (rect)->top, \
	(rect)->right, (rect)->bottom, \
	(rect)->right - (rect)->left, \
	(rect)->bottom - (rect)->top)

typedef int64_t red_time_t;

#define VALIDATE_SURFACE_RET(worker, surface_id) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        return; \
    }

#define VALIDATE_SURFACE_RETVAL(worker, surface_id, ret) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        return ret; \
    }

#define VALIDATE_SURFACE_BREAK(worker, surface_id) \
    if (!validate_surface(worker, surface_id)) { \
        rendering_incorrect(__func__); \
        break; \
    }

static void rendering_incorrect(const char *msg)
{
    spice_warning("rendering incorrect from now on: %s", msg);
}

static inline red_time_t timespec_to_red_time(struct timespec *time)
{
    return time->tv_sec * (1000 * 1000 * 1000) + time->tv_nsec;
}

#if defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
static clockid_t clock_id;

typedef unsigned long stat_time_t;

static stat_time_t stat_now(void)
{
    struct timespec ts;

    clock_gettime(clock_id, &ts);
    return ts.tv_nsec + ts.tv_sec * 1000 * 1000 * 1000;
}

double stat_cpu_time_to_sec(stat_time_t time)
{
    return (double)time / (1000 * 1000 * 1000);
}

typedef struct stat_info_s {
    const char *name;
    uint32_t count;
    stat_time_t max;
    stat_time_t min;
    stat_time_t total;
#ifdef COMPRESS_STAT
    uint64_t orig_size;
    uint64_t comp_size;
#endif
} stat_info_t;

static inline void stat_reset(stat_info_t *info)
{
    info->count = info->max = info->total = 0;
    info->min = ~(stat_time_t)0;
#ifdef COMPRESS_STAT
    info->orig_size = info->comp_size = 0;
#endif
}

#endif

#ifdef RED_WORKER_STAT
static const char *add_stat_name = "add";
static const char *exclude_stat_name = "exclude";
static const char *__exclude_stat_name = "__exclude";

static inline void stat_init(stat_info_t *info, const char *name)
{
    info->name = name;
    stat_reset(info);
}

static inline void stat_add(stat_info_t *info, stat_time_t start)
{
    stat_time_t time;
    ++info->count;
    time = stat_now() - start;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
}

#else
#define stat_add(a, b)
#define stat_init(a, b)
#endif

#ifdef COMPRESS_STAT
static const char *lz_stat_name = "lz";
static const char *glz_stat_name = "glz";
static const char *quic_stat_name = "quic";
static const char *jpeg_stat_name = "jpeg";
static const char *zlib_stat_name = "zlib_glz";
static const char *jpeg_alpha_stat_name = "jpeg_alpha";

static inline void stat_compress_init(stat_info_t *info, const char *name)
{
    info->name = name;
    stat_reset(info);
}

static inline void stat_compress_add(stat_info_t *info, stat_time_t start, int orig_size,
                                     int comp_size)
{
    stat_time_t time;
    ++info->count;
    time = stat_now() - start;
    info->total += time;
    info->max = MAX(info->max, time);
    info->min = MIN(info->min, time);
    info->orig_size += orig_size;
    info->comp_size += comp_size;
}

double inline stat_byte_to_mega(uint64_t size)
{
    return (double)size / (1000 * 1000);
}

#else
#define stat_compress_init(a, b)
#define stat_compress_add(a, b, c, d)
#endif

#define MAX_EVENT_SOURCES 20
#define INF_EVENT_WAIT ~0

struct SpiceWatch {
    struct RedWorker *worker;
    SpiceWatchFunc watch_func;
    void *watch_func_opaque;
};

enum {
    BUF_TYPE_RAW = 1,
};

enum {
    PIPE_ITEM_TYPE_DRAW = PIPE_ITEM_TYPE_CHANNEL_BASE, //DrawPipeItem
    PIPE_ITEM_TYPE_INVAL_ONE,
    PIPE_ITEM_TYPE_CURSOR,
    PIPE_ITEM_TYPE_CURSOR_INIT,
    PIPE_ITEM_TYPE_IMAGE, //SURFACE图像命令
    PIPE_ITEM_TYPE_STREAM_CREATE, //流创建管道项
    PIPE_ITEM_TYPE_STREAM_CLIP, //流裁剪管道项
    PIPE_ITEM_TYPE_STREAM_DESTROY, //流销毁管道项
    PIPE_ITEM_TYPE_UPGRADE,
    PIPE_ITEM_TYPE_VERB,
    PIPE_ITEM_TYPE_MIGRATE_DATA,
    PIPE_ITEM_TYPE_PIXMAP_SYNC, //PIXMAP同步
    PIPE_ITEM_TYPE_PIXMAP_RESET, //PIXMAP重置
    PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE,
    PIPE_ITEM_TYPE_INVAL_PALLET_CACHE,
    PIPE_ITEM_TYPE_CREATE_SURFACE,
    PIPE_ITEM_TYPE_DESTROY_SURFACE,
    PIPE_ITEM_TYPE_MONITORS_CONFIG,
    PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
};

typedef struct VerbItem {
    PipeItem base;
    uint16_t verb;
} VerbItem;

#define MAX_CACHE_CLIENTS 4
#define MAX_LZ_ENCODERS MAX_CACHE_CLIENTS

typedef struct NewCacheItem NewCacheItem;
//pixmapCache的哈希表项
struct NewCacheItem {
    RingItem lru_link; //lru
    NewCacheItem *next; //哈希桶
    uint64_t id; //id 缓存图像的id，客户端生成。告诉服务端，命中时，直接发送id即可
    uint64_t sync[MAX_CACHE_CLIENTS]; //id序号，最多4个缓存客户端
    size_t size; //本项占用缓存空间大小，//PixmapCache模拟的是客户端的缓存，
	//所以不会在服务端分配真正的缓存空间，只记录空间大小和占用量和剩余量
    int lossy; //图像是否是否有损
};

typedef struct CacheItem CacheItem;

struct CacheItem {
    union {
        PipeItem pipe_data;
        struct {
            RingItem lru_link;
            CacheItem *next;
        } cache_data;
    } u;
    uint64_t id;
    size_t size;
    uint32_t inval_type;
};

// SURFACE创建管道项
typedef struct SurfaceCreateItem {
    SpiceMsgSurfaceCreate surface_create;
    PipeItem pipe_item;
} SurfaceCreateItem;

// SURFACE销毁管道项
typedef struct SurfaceDestroyItem {
    SpiceMsgSurfaceDestroy surface_destroy;
    PipeItem pipe_item;
} SurfaceDestroyItem;

typedef struct MonitorsConfig {
    int refs;
    struct RedWorker *worker;
    int count;
    int max_allowed;
    QXLHead heads[0];
} MonitorsConfig;

//显示器配置管道项
typedef struct MonitorsConfigItem {
    PipeItem pipe_item;
    MonitorsConfig *monitors_config;
} MonitorsConfigItem;

//流活动报告管道项
typedef struct StreamActivateReportItem {
    PipeItem pipe_item;
    uint32_t stream_id; //流id
} StreamActivateReportItem;

//光标项,用于光标管道项
typedef struct CursorItem {
    uint32_t group_id; //组id
    int refs; //引用计数
    RedCursorCmd *red_cursor; //光标命令
} CursorItem;

//光标管道项
typedef struct CursorPipeItem {
    PipeItem base;
    CursorItem *cursor_item;
    int refs;
} CursorPipeItem;

typedef struct LocalCursor {
    CursorItem base;
    SpicePoint16 position;
    uint32_t data_size;
    SpiceCursor red_cursor;
} LocalCursor;

#define MAX_PIPE_SIZE 50
#define CHANNEL_RECEIVE_BUF_SIZE 1024

#define WIDE_CLIENT_ACK_WINDOW 40
#define NARROW_CLIENT_ACK_WINDOW 20

#define BITS_CACHE_HASH_SHIFT 10
#define BITS_CACHE_HASH_SIZE (1 << BITS_CACHE_HASH_SHIFT)
#define BITS_CACHE_HASH_MASK (BITS_CACHE_HASH_SIZE - 1)
#define BITS_CACHE_HASH_KEY(id) ((id) & BITS_CACHE_HASH_MASK)

#define CLIENT_CURSOR_CACHE_SIZE 256

#define CURSOR_CACHE_HASH_SHIFT 8
#define CURSOR_CACHE_HASH_SIZE (1 << CURSOR_CACHE_HASH_SHIFT)
#define CURSOR_CACHE_HASH_MASK (CURSOR_CACHE_HASH_SIZE - 1)
#define CURSOR_CACHE_HASH_KEY(id) ((id) & CURSOR_CACHE_HASH_MASK)

#define CLIENT_PALETTE_CACHE_SIZE 128

#define PALETTE_CACHE_HASH_SHIFT 8
#define PALETTE_CACHE_HASH_SIZE (1 << PALETTE_CACHE_HASH_SHIFT)
#define PALETTE_CACHE_HASH_MASK (PALETTE_CACHE_HASH_SIZE - 1)
#define PALETTE_CACHE_HASH_KEY(id) ((id) & PALETTE_CACHE_HASH_MASK)

//surface图像管道项
typedef struct ImageItem {
    PipeItem link; //管道项
    int refs; //引用计数
    SpicePoint pos; //图像显示坐标
    int width; //图像宽
    int height; //图像高
    int stride; //图像行长
    int top_down; //是否从上往下显示
    int surface_id; //显示的surface
    int image_format; //图像格式
    uint32_t image_flags; //标志，如SPICE_IMAGE_FLAGS_HIGH_BITS_SET
    int can_lossy; //能否压缩
    uint8_t data[0]; //图像数据
} ImageItem;

typedef struct Drawable Drawable;

typedef struct DisplayChannel DisplayChannel;
typedef struct DisplayChannelClient DisplayChannelClient;

enum {
    STREAM_FRAME_NONE,
    STREAM_FRAME_NATIVE,
    STREAM_FRAME_CONTAINER,
};

/**
 * 图像流结构，只有surface == 0的绘图命令可以跟流相关
 * 所以没有分surface管理流对象。
**/
typedef struct Stream Stream;
struct Stream {
    uint8_t refs; //引用计数
    Drawable *current; //流的最后一帧的Drawable指针
    red_time_t last_time; //流最后一帧的到达时间，等于最后一帧的创建时间
    int width; //图像流宽
    int height; //图像流高
    SpiceRect dest_area; //目标区域
    int top_down; //图像是否topdown
    Stream *next; //接入空闲流链表，用于stream分配和释放
    RingItem link; //连接到RedWorker->stream链表中，用于从RedWorker索引到所有流

	//流定时器，定时器会定时执行回调函数，回调函数不用再次添加定时器来持续执行
    SpiceTimer *input_fps_timer;
    //进入流的帧数计数，定时器会每次运行重置该计数
    //每关联一帧都会增加一个计数
    uint32_t num_input_frames; 
	//流定时器的创建时间
    uint64_t input_fps_timer_start;
	//流帧率
    uint32_t input_fps;
};

#define STREAM_STATS
#ifdef STREAM_STATS
//流统计信息
typedef struct StreamStats {
   uint64_t num_drops_pipe; //丢掉的管道项计数
   uint64_t num_drops_fps; //每秒丢掉帧计数
   uint64_t num_frames_sent; //发送帧数
   uint64_t num_input_frames; //输入帧数
   uint64_t size_sent; //发送字节数

   uint64_t start; //开始时间
   uint64_t end; //结束时间
} StreamStats;
#endif

// 图像流代理，之所以有StreamAgent，是因为一条流
// 需要跟多个RCC进行关联，所以有个1:n的结构体来表示
// 这种关联关系
typedef struct StreamAgent {
    QRegion vis_region; /* 当前被视频帧占据的surface区域
    					the part of the surface area that is currently occupied by video
                           fragments */
    QRegion clip;       /* 
    					
    					the current video clipping. It can be different from vis_region:
                           for example, let c1 be the clip area at time t1, and c2
                           be the clip area at time t2, where t1 < t2. If c1 contains c2, and
                           at least part of c1/c2, hasn't been covered by a non-video images,
                           vis_region will contain c2 and also the part of c1/c2 that still
                           displays fragments of the video */

    PipeItem create_item; //创建事件管道项
    PipeItem destroy_item; //销毁事件管道项
    Stream *stream; //代理的目标流对象
    uint64_t last_send_time;
    MJpegEncoder *mjpeg_encoder; //mjpeg编码器，
    DisplayChannelClient *dcc; //流代理对应的DCC

    int frames; //总帧数
    int drops; //总丢帧数
    int fps; //每秒帧数

    uint32_t report_id; //通过随机数产生，跟流id不一样
    uint32_t client_required_latency; //客户端要求的帧延时，应该是用于帧控
#ifdef STREAM_STATS
    StreamStats stats; //流统计信息，根据每个DCC进行统计
#endif
} StreamAgent;

// 流裁剪管道项
typedef struct StreamClipItem { //流裁剪
    PipeItem base; //管道项
    int refs; //引用计数
    StreamAgent *stream_agent; ////哪个流代理触发的流裁剪管道项, 每创建一个StreamClipItem都会Stream加个引用
    int clip_type; // 裁剪类型：矩形裁剪或者不裁剪
    SpiceClipRects *rects; //裁剪区域，矩形集合，裁剪矩形集合从StreamAgent的Clip中获取
} StreamClipItem;

/* 压缩缓冲区 */
typedef struct RedCompressBuf RedCompressBuf;
struct RedCompressBuf {
    uint32_t buf[RED_COMPRESS_BUF_SIZE / 4];//64K/4，也就是16K的像素
    RedCompressBuf *next; //缓冲区链表
    RedCompressBuf *send_next;
};

static const int BITMAP_FMT_IS_PLT[] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const int BITMAP_FMP_BYTES_PER_PIXEL[] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 4, 1};

//判断图像是否有渐变，要求是RGB图像且不是8A
#define BITMAP_FMT_HAS_GRADUALITY(f)                                    \
    (bitmap_fmt_is_rgb(f)        &&                                     \
     ((f) != SPICE_BITMAP_FMT_8BIT_A))

pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
Ring pixmap_cache_list = {&pixmap_cache_list, &pixmap_cache_list};

typedef struct PixmapCache PixmapCache;
struct PixmapCache { //PixmapCache模拟的是客户端的缓存，
	//所以不会在服务端分配真正的缓存空间，只记录空间大小和占用量和剩余量
    RingItem base; //连接到全局的pixmap_cache_list列表中
    pthread_mutex_t lock; //缓存保护锁
    uint8_t id; //id
    uint32_t refs; //引用计数
    NewCacheItem *hash_table[BITS_CACHE_HASH_SIZE]; //hash桶
    Ring lru; //lru链表
    int64_t available; //可用空间
    int64_t size; //cache总空间
    int32_t items; //缓存项数量

    int freezed;
    RingItem *freezed_head;
    RingItem *freezed_tail;

    uint32_t generation;
    struct {
        uint8_t client;
        uint64_t message;
    } generation_initiator;
    uint64_t sync[MAX_CACHE_CLIENTS]; // 这里的CLIENTS表示同一个客户端的不同通道客户端
    								  // here CLIENTS refer to different channel
                                      // clients of the same client
    RedClient *client;
};

#define NUM_STREAMS 50

typedef struct WaitForChannels {
    SpiceMsgWaitForChannels header;
    SpiceWaitForChannel buf[MAX_CACHE_CLIENTS];
} WaitForChannels;

typedef struct FreeList {
    int res_size; //资源列表可用量
    SpiceResourceList *res; //资源列表
    uint64_t sync[MAX_CACHE_CLIENTS];
    WaitForChannels wait;
} FreeList;

typedef struct  {
    DisplayChannelClient *dcc;
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            SpiceChunks *chunks;
            int next;
            int stride;
            int reverse;
        } lines_data;
        struct {
            RedCompressBuf* next;
            int size_left;
        } compressed_data; // for encoding data that was already compressed by another method
    } u;
    char message_buf[512];
} EncoderData;

typedef struct {
    QuicUsrContext usr;
    EncoderData data;
} QuicData;

typedef struct {
    LzUsrContext usr;
    EncoderData data;
} LzData;

typedef struct {
    GlzEncoderUsrContext usr;
    EncoderData data;
} GlzData;

typedef struct {
    JpegEncoderUsrContext usr;
    EncoderData data;
} JpegData;

typedef struct {
    ZlibEncoderUsrContext usr;
    EncoderData data;
} ZlibData;

/**********************************/
/* LZ dictionary related entities */
/**********************************/
#define MAX_GLZ_DRAWABLE_INSTANCES 2

typedef struct RedGlzDrawable RedGlzDrawable;

/* for each qxl drawable, there may be several instances of lz drawables */
/* TODO - reuse this stuff for the top level. I just added a second level of multiplicity
 * at the Drawable by keeping a ring, so:
 * Drawable -> (ring of) RedGlzDrawable -> (up to 2) GlzDrawableInstanceItem
 * and it should probably (but need to be sure...) be
 * Drawable -> ring of GlzDrawableInstanceItem.
 */
typedef struct GlzDrawableInstanceItem {
    RingItem glz_link;
    RingItem free_link;
    GlzEncDictImageContext *glz_instance;
    RedGlzDrawable         *red_glz_drawable;
} GlzDrawableInstanceItem;

struct RedGlzDrawable {
    RingItem link;    // ordered by the time it was encoded
    RingItem drawable_link;
    RedDrawable *red_drawable;
    Drawable    *drawable;
    uint32_t     group_id;
    GlzDrawableInstanceItem instances_pool[MAX_GLZ_DRAWABLE_INSTANCES];
    Ring instances;
    uint8_t instances_count;
    DisplayChannelClient *dcc;
};

pthread_mutex_t glz_dictionary_list_lock = PTHREAD_MUTEX_INITIALIZER;
Ring glz_dictionary_list = {&glz_dictionary_list, &glz_dictionary_list};

typedef struct GlzSharedDictionary {
    RingItem base;
    GlzEncDictContext *dict;
    uint32_t refs;
    uint8_t id;
    pthread_rwlock_t encode_lock;
    int migrate_freeze;
    RedClient *client; // channel clients of the same client share the dict
} GlzSharedDictionary;

#define NUM_SURFACES 10000

/*CommonChannel是显示通道和光标通道的基本结构体（类似基类）
  存放了RedWorker的指针，类似声音通道里有SndWorker。
*/
typedef struct CommonChannel {
    RedChannel base; // Must be the first thing
    struct RedWorker *worker; //通道关联的worker
    uint8_t recv_buf[CHANNEL_RECEIVE_BUF_SIZE]; //通道接受缓冲区
    uint32_t id_alloc; // bitfield. TODO - use this instead of shift scheme.
    int during_target_migrate; /* 当连接到本通道的客户端正在迁移时此标记为真。
    							  TRUE when the client that is associated with the channel
                                  is during migration. Turned off when the vm is started.
                                  The flag is used to avoid sending messages that are artifacts
                                  of the transition from stopped vm to loaded vm (e.g., recreation
                                  of the primary surface) */
} CommonChannel;

typedef struct CommonChannelClient {
    RedChannelClient base;
    uint32_t id;
    struct RedWorker *worker;
    int is_low_bandwidth;
} CommonChannelClient;

/* Each drawable can refer to at most 3 images: src, brush and mask 
   一个绘图命令最多引用三帧图像，源图像，画刷，掩码
*/
#define MAX_DRAWABLE_PIXMAP_CACHE_ITEMS 3

struct DisplayChannelClient {
    CommonChannelClient common;

    int expect_init;

    PixmapCache *pixmap_cache; //PixmapCache
    uint32_t pixmap_cache_generation; //PixmapCache代数
    int pending_pixmaps_sync; //正在进行pixmaps同步

    CacheItem *palette_cache[PALETTE_CACHE_HASH_SIZE];
    Ring palette_cache_lru;
    long palette_cache_available;
    uint32_t palette_cache_items;

    struct {
        uint32_t stream_outbuf_size; //DCC发送数据的io流缓冲区大小
        uint8_t *stream_outbuf; // 注意:io流缓冲区也会被当成压缩缓冲区
        //caution stream buffer is also used as compress bufs!!!

        RedCompressBuf *used_compress_bufs;

        FreeList free_list;
		//一个命令命中三副图像，也就是3个id
        uint64_t pixmap_cache_items[MAX_DRAWABLE_PIXMAP_CACHE_ITEMS];
        int num_pixmap_cache_items; //当前缓存匹配的项索引，匹配源、画刷、还是mask
    } send_data;

    /* global lz encoding entities */
    GlzSharedDictionary *glz_dict;
    GlzEncoderContext   *glz;
    GlzData glz_data;

    Ring glz_drawables;               // all the living lz drawable, ordered by encoding time
    Ring glz_drawables_inst_to_free;               // list of instances to be freed
    pthread_mutex_t glz_drawables_inst_to_free_lock;

    uint8_t surface_client_created[NUM_SURFACES]; //记录各个surface在DCC中是否已经创建
    QRegion surface_client_lossy_region[NUM_SURFACES]; //记录各个surface在DCC中的有损区域

    StreamAgent stream_agents[NUM_STREAMS]; //流代理，每条流在显示通道里有个代理对象
    int use_mjpeg_encoder_rate_control; //mjepg是否码率控制
    uint32_t streams_max_latency;
    uint64_t streams_max_bit_rate;
};

struct DisplayChannel {
    CommonChannel common; // Must be the first thing

    int enable_jpeg;
    int jpeg_quality;
    int enable_zlib_glz_wrap;
    int zlib_level;

    RedCompressBuf *free_compress_bufs;

#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *cache_hits_counter;
    uint64_t *add_to_cache_counter;
    uint64_t *non_cache_counter;
#endif
#ifdef COMPRESS_STAT
    stat_info_t lz_stat;
    stat_info_t glz_stat;
    stat_info_t quic_stat;
    stat_info_t jpeg_stat;
    stat_info_t zlib_glz_stat;
    stat_info_t jpeg_alpha_stat;
#endif
};

typedef struct CursorChannelClient {
    CommonChannelClient common;

    CacheItem *cursor_cache[CURSOR_CACHE_HASH_SIZE];
    Ring cursor_cache_lru;
    long cursor_cache_available;
    uint32_t cursor_cache_items;
} CursorChannelClient;

typedef struct CursorChannel {
    CommonChannel common; // Must be the first thing

#ifdef RED_STATISTICS
    StatNodeRef stat;
#endif
} CursorChannel;

enum {
    TREE_ITEM_TYPE_DRAWABLE,
    TREE_ITEM_TYPE_CONTAINER,
    TREE_ITEM_TYPE_SHADOW,
};

//命令树基础构件，树节点
typedef struct TreeItem {
    RingItem siblings_link; //接入兄弟节点
    uint32_t type; //节点类型，SHADOW、CONTAINER、DRAW
    struct Container *container; //如果有container节点
    QRegion rgn; //每个节点都有个目标渲染区域
#ifdef PIPE_DEBUG
    uint32_t id;
#endif
} TreeItem;

#define IS_DRAW_ITEM(item) ((item)->type == TREE_ITEM_TYPE_DRAWABLE)

typedef struct Shadow { //表示copybits的引用关系，
    TreeItem base; //引用的目标区域
    QRegion on_hold;
    struct DrawItem* owner; //哪条copy_bits创建了本shadow，拥有此引用
} Shadow;

typedef struct Container {
    TreeItem base;
    Ring items; //孩子节点链表
} Container;

typedef struct DrawItem {
    TreeItem base;
    uint8_t effect; //效果标记，主要识别是否透明
    uint8_t container_root; //自己是否是所属container的第一个节点
    Shadow *shadow; //是否有shadow，有则是copybits
} DrawItem;

typedef enum {
    BITMAP_GRADUAL_INVALID,
    BITMAP_GRADUAL_NOT_AVAIL,
    BITMAP_GRADUAL_LOW,
    BITMAP_GRADUAL_MEDIUM,
    BITMAP_GRADUAL_HIGH,
} BitmapGradualType;

//表示一个drawable依赖于一个surface
typedef struct DependItem {
    Drawable *drawable; //依赖命令
    RingItem ring_item; //将依赖项链入到surface的depend_on_me链表
} DependItem;

/**
 * DrawablePipeItem，表示Drawable对象和DCC之间的管道命令关系
 * 也就是说，一个DrawablePipeItem表示其中的drawable成员指向
 * 的Drawable对象加入到了dcc指向的DisplayChannelClient的发送管道中。
 * 那为什么有这个东西呢，因为原生的spice支持多个客户端接入同一个vm
 * 而Drawable与vm的画图命令一一对应，所以要在多个客户端同步显示相同的
 * 内容，就需要将绘图的指令发送给每个客户端，这个一个个的客户端在spice-server
 * 代码里就是又DisplayChannelClient来表示
**/
typedef struct DrawablePipeItem {
    RingItem base;  /*接入到Drawable的pipes链表中，对于多客户端，一个Drawable关联多个DrawablePipeItem*/
    PipeItem dpi_pipe_item; /* 接入到rcc的pipe链表中，一个DrawablePipeItem只能加入到一个rcc的pipe链表中 */
    Drawable *drawable; //指向Drawable
    DisplayChannelClient *dcc; //指向dcc，rcc的子类
    uint8_t refs; // 引用计数
} DrawablePipeItem;

/**
 * Drawable是spice用于处理画图命令中的spice结构
 * 它主要用在spice中用于图像优化和命令发送，所以其成员大多数
 * 也是一些图形命令之间的关联关系，并没有直接存储画图命令的
 * 参数和数据信息（这部分在RedDrawable中，因为画图命令数据可能很大，
 * 所以用指针来减少拷贝）。所以这个结构也就只在显示通道中使用，是个
 * 模块内部实现，不像RedDrawable定义在QXL接口头文件中。
**/
struct Drawable {
    uint8_t refs;
    RingItem surface_list_link; //接入RedSurface的current链表中
    RingItem list_link; // 接入到RedWorker的current链表中
    DrawItem tree_item; //命令树节点，用于连接在命令树中
    Ring pipes; //DrawablePipeItem列表，可理解为接入的rcc列表
    PipeItem *pipe_item_rest; //貌似没什么用，VDI代码尝试注释掉也没问题
    uint32_t size_pipe_item_rest; //貌似没什么用，VDI代码尝试注释掉也没问题
#ifdef UPDATE_AREA_BY_TREE //没打开
    RingItem collect_link; //没打开
#endif
    RedDrawable *red_drawable; //指向RedDrawable，来访问具体的画图信息

    Ring glz_ring; //GLZ使用的链表

    red_time_t creation_time; //Drawable的创建时间，在get_drawable是设置
    int frames_count; //用于red_stream流中的id，依靠此值来识别图像流中各帧的顺序
    int gradual_frames_count;  //流中gradual序号
    int last_gradual_frame; //流中最后一个gradual序号
    Stream *stream; //所属的流
    Stream *sized_stream;
    int streamable; //是否可以进行流处理
    BitmapGradualType copy_bitmap_graduality;
    uint32_t group_id;
    DependItem depend_items[3]; //一个命令最多依赖三个surface

    uint8_t *backed_surface_data;
    DependItem pipe_depend_items[3];

    int surface_id; //目标surface
    int surfaces_dest[3]; //依赖的其他surface的id

    uint32_t process_commands_generation;
};

typedef struct _Drawable _Drawable;
struct _Drawable {
    union {
        Drawable drawable;
        _Drawable *next;
    } u;
};

typedef struct _CursorItem _CursorItem;
struct _CursorItem {
    union {
        CursorItem cursor_item;
        _CursorItem *next;
    } u;
};

//
typedef struct UpgradeItem {
    PipeItem base;
    int refs;
    Drawable *drawable; //使用RedDrawable的self_bitmap_image直接渲染
    //此命令要求是Drawcopy, ROP为SPICE_ROPD_OP_PUT，且没有mask图像
    SpiceClipRects *rects; //变成drawcopy的目标裁剪区域
} UpgradeItem;

//画图上下文
typedef struct DrawContext {
    SpiceCanvas *canvas; //Spice画布
    int canvas_draws_on_surface; //SW画布为true，其余类型画布FALSE
    int top_down; //渲染方向，跟stride的正负相关
    uint32_t width; //宽
    uint32_t height; //高
    int32_t stride; //行长，可能为负，则为从后往前画
    uint32_t format;  //数据格式
    void *line_0; //渲染内存地址
} DrawContext;

// RedSurface, 表示显示的一个图层，VDI里只使用0号表面，
typedef struct RedSurface {
    uint32_t refs; //引用计数，一个Drawable只要存活就会对RedSurface持有引用
    Ring current;  //命令树的根节点孩子链表，由各种treeitem组成
    Ring current_list; //Drawable的surface_list_link链表，包含本surface的所有命令
#ifdef ACYCLIC_SURFACE_DEBUG
    int current_gn;
#endif
    DrawContext context; //画布绘画上下文

    Ring depend_on_me; //依赖关系列表，代表着一系列依赖本surface的命令列表
    QRegion draw_dirty_region; //surface的脏区域

    //fix me - better handling here
    QXLReleaseInfoExt create, destroy; //记录create和destroy时的QXL相关信息
} RedSurface;

typedef struct ItemTrace {
    red_time_t time; //跟踪指令的创建时间
    int frames_count; //帧计数
    int gradual_frames_count;
    int last_gradual_frame;
    int width;
    int height;
    SpiceRect dest_area;
} ItemTrace;

#define TRACE_ITEMS_SHIFT 3 //3位跟踪id
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT) //最多可以跟踪8个
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1) //跟踪id掩码

#define NUM_DRAWABLES 1000
#define NUM_CURSORS 100

typedef struct RedWorker {
    DisplayChannel *display_channel;
    CursorChannel *cursor_channel;
    QXLInstance *qxl;
    RedDispatcher *red_dispatcher;

    int channel;
    int id;
    int running;
    uint32_t *pending;
    struct pollfd poll_fds[MAX_EVENT_SOURCES]; //poll_fds和watches是个同槽fd
    struct SpiceWatch watches[MAX_EVENT_SOURCES];
    unsigned int event_timeout;
    uint32_t repoll_cmd_ring;
    uint32_t repoll_cursor_ring;
    uint32_t num_renderers;
    uint32_t renderers[RED_MAX_RENDERERS]; //各个surface的渲染方式可以不一致
    uint32_t renderer; //定义显示通道的同意渲染方式，一般为SW渲染，-1表示没有定义，如果此值有效则上面的值无效

    RedSurface surfaces[NUM_SURFACES]; //所有redsurface的数组
    uint32_t n_surfaces; //实际有效的surface数量
    SpiceImageSurfaces image_surfaces; 

    MonitorsConfig *monitors_config;

    Ring current_list; //Drawable的list_link链表，包含了所有surface的所有命令
    uint32_t current_size; // 当前命令计数，会增加，会递减
    uint32_t drawable_count; //drawable计数
    uint32_t red_drawable_count; //red_drawable计数
    uint32_t glz_drawable_count; //glz压缩的drawable计数
    uint32_t transparent_count; //透明命令计数

    uint32_t shadows_count; //所有命令树中的Shadow节点计数
    uint32_t containers_count; //所有命令树中的容器节点计数
    uint32_t stream_count; //流计数

    uint32_t bits_unique; //BIT唯一值

    CursorItem *cursor;
    int cursor_visible;
    SpicePoint16 cursor_position;
    uint16_t cursor_trail_length;
    uint16_t cursor_trail_frequency;

    _Drawable drawables[NUM_DRAWABLES];
    _Drawable *free_drawables;

    _CursorItem cursor_items[NUM_CURSORS];
    _CursorItem *free_cursor_items;

    RedMemSlotInfo mem_slots;

    uint32_t preload_group_id;

    ImageCache image_cache; //图像缓存

    spice_image_compression_t image_compression; //记录图像压缩方式
    spice_wan_compression_t jpeg_state; 
    spice_wan_compression_t zlib_glz_state;

    uint32_t mouse_mode;

    uint32_t streaming_video;//三种值 ALL、FILTER、OFF
    Stream streams_buf[NUM_STREAMS]; //默认的图像流对象，50条
    Stream *free_streams; //指向空闲流链表
    Ring streams; //所有图像流的链表
    ItemTrace items_trace[NUM_TRACE_ITEMS]; //跟踪项，总共8个，论调跟踪
    uint32_t next_item_trace; //跟踪项计数，递增，但是收到掩码限制，不超过8
    uint64_t streams_size_total; //所有流的尺寸

    QuicData quic_data;
    QuicContext *quic;

    LzData lz_data;
    LzContext  *lz;

    JpegData jpeg_data;
    JpegEncoderContext *jpeg;

    ZlibData zlib_data;
    ZlibEncoder *zlib;

    uint32_t process_commands_generation;
#ifdef PIPE_DEBUG
    uint32_t last_id;
#endif
#ifdef RED_WORKER_STAT
    stat_info_t add_stat;
    stat_info_t exclude_stat;
    stat_info_t __exclude_stat;
    uint32_t add_count;
    uint32_t add_with_shadow_count;
#endif
#ifdef RED_STATISTICS
    StatNodeRef stat;
    uint64_t *wakeup_counter;
    uint64_t *command_counter;
#endif

    int driver_cap_monitors_config;
    int set_client_capabilities_pending;
} RedWorker;

typedef enum {
    BITMAP_DATA_TYPE_INVALID,
    BITMAP_DATA_TYPE_CACHE,
    BITMAP_DATA_TYPE_SURFACE,
    BITMAP_DATA_TYPE_BITMAP,
    BITMAP_DATA_TYPE_BITMAP_TO_CACHE,
} BitmapDataType;

typedef struct BitmapData {
    BitmapDataType type;
    uint64_t id; // surface id or cache item id
    SpiceRect lossy_rect;
} BitmapData;

static void red_draw_qxl_drawable(RedWorker *worker, Drawable *drawable);
static void red_current_flush(RedWorker *worker, int surface_id);
#ifdef DRAW_ALL
#define red_update_area(worker, rect, surface_id)
#define red_draw_drawable(worker, item)
#else
static void red_draw_drawable(RedWorker *worker, Drawable *item);
static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id);
static void red_update_area_till(RedWorker *worker, const SpiceRect *area, int surface_id,
                                 Drawable *last);
#endif
static void red_release_cursor(RedWorker *worker, CursorItem *cursor);
static inline void release_drawable(RedWorker *worker, Drawable *item);
static void red_display_release_stream(RedWorker *worker, StreamAgent *agent);
static inline void red_detach_stream(RedWorker *worker, Stream *stream, int detach_sized);
static void red_stop_stream(RedWorker *worker, Stream *stream);
static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *sect);
static inline void display_begin_send_message(RedChannelClient *rcc);
static void red_release_pixmap_cache(DisplayChannelClient *dcc);
static void red_release_glz(DisplayChannelClient *dcc);
static void red_freeze_glz(DisplayChannelClient *dcc);
static void display_channel_push_release(DisplayChannelClient *dcc, uint8_t type, uint64_t id,
                                         uint64_t* sync_data);
static void red_display_release_stream_clip(RedWorker *worker, StreamClipItem *item);
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc);
static void red_display_free_glz_drawable(DisplayChannelClient *dcc, RedGlzDrawable *drawable);
static ImageItem *red_add_surface_area_image(DisplayChannelClient *dcc, int surface_id,
                                             SpiceRect *area, PipeItem *pos, int can_lossy);
static BitmapGradualType _get_bitmap_graduality_level(RedWorker *worker, SpiceBitmap *bitmap,
                                                      uint32_t group_id);
static inline int _stride_is_extra(SpiceBitmap *bitmap);
static void red_disconnect_cursor(RedChannel *channel);
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item);
static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item);
static void cursor_channel_client_release_item_before_push(CursorChannelClient *ccc,
                                                           PipeItem *item);
static void cursor_channel_client_release_item_after_push(CursorChannelClient *ccc,
                                                          PipeItem *item);

static void red_push_monitors_config(DisplayChannelClient *dcc);

/*
 * Macros to make iterating over stuff easier
 * The two collections we iterate over:
 *  given a channel, iterate over it's clients
 */

/* a generic safe for loop macro  */
#define SAFE_FOREACH(link, next, cond, ring, data, get_data)               \
    for ((((link) = ((cond) ? ring_get_head(ring) : NULL)), \
          ((next) = ((link) ? ring_next((ring), (link)) : NULL)),          \
          ((data) = ((link)? (get_data) : NULL)));                         \
         (link);                                                           \
         (((link) = (next)),                                               \
          ((next) = ((link) ? ring_next((ring), (link)) : NULL)),          \
          ((data) = ((link)? (get_data) : NULL))))

#define LINK_TO_RCC(ptr) SPICE_CONTAINEROF(ptr, RedChannelClient, channel_link)
#define RCC_FOREACH_SAFE(link, next, rcc, channel) \
    SAFE_FOREACH(link, next, channel,  &(channel)->clients, rcc, LINK_TO_RCC(link))


#define LINK_TO_DCC(ptr) SPICE_CONTAINEROF(ptr, DisplayChannelClient,  \
                                      common.base.channel_link)
#define DCC_FOREACH_SAFE(link, next, dcc, channel)                       \
    SAFE_FOREACH(link, next, channel,  &(channel)->clients, dcc, LINK_TO_DCC(link))


#define WORKER_FOREACH_DCC_SAFE(worker, link, next, dcc)      \
    DCC_FOREACH_SAFE(link, next, dcc,                         \
                ((((worker) && (worker)->display_channel))?   \
                 (&(worker)->display_channel->common.base) : NULL))


//LINK_TO_DPI ：从链表节点恢复成DrawablePipeItem，
#define LINK_TO_DPI(ptr) SPICE_CONTAINEROF((ptr), DrawablePipeItem, base)
#define DRAWABLE_FOREACH_DPI_SAFE(drawable, link, next, dpi)          \
    SAFE_FOREACH(link, next, drawable,  &(drawable)->pipes, dpi, LINK_TO_DPI(link))


#define LINK_TO_GLZ(ptr) SPICE_CONTAINEROF((ptr), RedGlzDrawable, \
                                           drawable_link)
#define DRAWABLE_FOREACH_GLZ_SAFE(drawable, link, next, glz) \
    SAFE_FOREACH(link, next, drawable, &(drawable)->glz_ring, glz, LINK_TO_GLZ(link))


#define DCC_TO_WORKER(dcc) \
    (SPICE_CONTAINEROF((dcc)->common.base.channel, CommonChannel, base)->worker)

// TODO: replace with DCC_FOREACH when it is introduced
#define WORKER_TO_DCC(worker) \
    (worker->display_channel ? SPICE_CONTAINEROF(worker->display_channel->common.base.rcc,\
                       DisplayChannelClient, common.base) : NULL)

#define DCC_TO_DC(dcc) SPICE_CONTAINEROF((dcc)->common.base.channel,\
                                         DisplayChannel, common.base)

#define RCC_TO_DCC(rcc) SPICE_CONTAINEROF((rcc), DisplayChannelClient, common.base)
#define RCC_TO_CCC(rcc) SPICE_CONTAINEROF((rcc), CursorChannelClient, common.base)



#ifdef COMPRESS_STAT
static void print_compress_stats(DisplayChannel *display_channel)
{
    uint64_t glz_enc_size;

    if (!display_channel) {
        return;
    }

    glz_enc_size = display_channel->enable_zlib_glz_wrap ?
                       display_channel->zlib_glz_stat.comp_size :
                       display_channel->glz_stat.comp_size;

    spice_info("==> Compression stats for display %u", display_channel->common.id);
    spice_info("Method   \t  count  \torig_size(MB)\tenc_size(MB)\tenc_time(s)");
    spice_info("QUIC     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->quic_stat.count,
               stat_byte_to_mega(display_channel->quic_stat.orig_size),
               stat_byte_to_mega(display_channel->quic_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->quic_stat.total)
               );
    spice_info("GLZ      \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->glz_stat.count,
               stat_byte_to_mega(display_channel->glz_stat.orig_size),
               stat_byte_to_mega(display_channel->glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->glz_stat.total)
               );
    spice_info("ZLIB GLZ \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->zlib_glz_stat.count,
               stat_byte_to_mega(display_channel->zlib_glz_stat.orig_size),
               stat_byte_to_mega(display_channel->zlib_glz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->zlib_glz_stat.total)
               );
    spice_info("LZ       \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total)
               );
    spice_info("JPEG     \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_stat.count,
               stat_byte_to_mega(display_channel->jpeg_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_stat.total)
               );
    spice_info("JPEG-RGBA\t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->jpeg_alpha_stat.total)
               );
    spice_info("-------------------------------------------------------------------");
    spice_info("Total    \t%8d\t%13.2f\t%12.2f\t%12.2f",
               display_channel->lz_stat.count + display_channel->glz_stat.count +
                                                display_channel->quic_stat.count +
                                                display_channel->jpeg_stat.count +
                                                display_channel->jpeg_alpha_stat.count,
               stat_byte_to_mega(display_channel->lz_stat.orig_size +
                                 display_channel->glz_stat.orig_size +
                                 display_channel->quic_stat.orig_size +
                                 display_channel->jpeg_stat.orig_size +
                                 display_channel->jpeg_alpha_stat.orig_size),
               stat_byte_to_mega(display_channel->lz_stat.comp_size +
                                 glz_enc_size +
                                 display_channel->quic_stat.comp_size +
                                 display_channel->jpeg_stat.comp_size +
                                 display_channel->jpeg_alpha_stat.comp_size),
               stat_cpu_time_to_sec(display_channel->lz_stat.total +
                                    display_channel->glz_stat.total +
                                    display_channel->zlib_glz_stat.total +
                                    display_channel->quic_stat.total +
                                    display_channel->jpeg_stat.total +
                                    display_channel->jpeg_alpha_stat.total)
               );
}

#endif

static MonitorsConfig *monitors_config_getref(MonitorsConfig *monitors_config)
{
    monitors_config->refs++;

    return monitors_config;
}

static void monitors_config_decref(MonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        return;
    }
    if (--monitors_config->refs > 0) {
        return;
    }

    spice_debug("freeing monitors config");
    free(monitors_config);
}

static inline int is_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    if (surface_id == 0) {
        return TRUE;
    }
    return FALSE;
}

static inline void __validate_surface(RedWorker *worker, uint32_t surface_id)
{
    spice_warn_if(surface_id >= worker->n_surfaces);
}

static inline int validate_surface(RedWorker *worker, uint32_t surface_id)
{
    spice_warn_if(surface_id >= worker->n_surfaces);
    if (!worker->surfaces[surface_id].context.canvas) {
        spice_warning("canvas address is %p for %d (and is NULL)\n",
                   &(worker->surfaces[surface_id].context.canvas), surface_id);
        spice_warning("failed on %d", surface_id);
        spice_warn_if(!worker->surfaces[surface_id].context.canvas);
        return 0;
    }
    return 1;
}

static const char *draw_type_to_str(uint8_t type)
{
    switch (type) {
    case QXL_DRAW_FILL:
        return "QXL_DRAW_FILL";
    case QXL_DRAW_OPAQUE:
        return "QXL_DRAW_OPAQUE";
    case QXL_DRAW_COPY:
        return "QXL_DRAW_COPY";
    case QXL_DRAW_TRANSPARENT:
        return "QXL_DRAW_TRANSPARENT";
    case QXL_DRAW_ALPHA_BLEND:
        return "QXL_DRAW_ALPHA_BLEND";
    case QXL_COPY_BITS:
        return "QXL_COPY_BITS";
    case QXL_DRAW_BLEND:
        return "QXL_DRAW_BLEND";
    case QXL_DRAW_BLACKNESS:
        return "QXL_DRAW_BLACKNESS";
    case QXL_DRAW_WHITENESS:
        return "QXL_DRAW_WHITENESS";
    case QXL_DRAW_INVERS:
        return "QXL_DRAW_INVERS";
    case QXL_DRAW_ROP3:
        return "QXL_DRAW_ROP3";
    case QXL_DRAW_COMPOSITE:
        return "QXL_DRAW_COMPOSITE";
    case QXL_DRAW_STROKE:
        return "QXL_DRAW_STROKE";
    case QXL_DRAW_TEXT:
        return "QXL_DRAW_TEXT";
    default:
        return "?";
    }
}

static void show_red_drawable(RedWorker *worker, RedDrawable *drawable, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }

    printf("%s effect %d bbox(%d %d %d %d)",
           draw_type_to_str(drawable->type),
           drawable->effect,
           drawable->bbox.top,
           drawable->bbox.left,
           drawable->bbox.bottom,
           drawable->bbox.right);

    switch (drawable->type) {
    case QXL_DRAW_FILL:
    case QXL_DRAW_OPAQUE:
    case QXL_DRAW_COPY:
    case QXL_DRAW_TRANSPARENT:
    case QXL_DRAW_ALPHA_BLEND:
    case QXL_COPY_BITS:
    case QXL_DRAW_BLEND:
    case QXL_DRAW_BLACKNESS:
    case QXL_DRAW_WHITENESS:
    case QXL_DRAW_INVERS:
    case QXL_DRAW_ROP3:
    case QXL_DRAW_COMPOSITE:
    case QXL_DRAW_STROKE:
    case QXL_DRAW_TEXT:
        break;
    default:
        spice_error("bad drawable type");
    }
    printf("\n");
}

static void show_draw_item(RedWorker *worker, DrawItem *draw_item, const char *prefix)
{
    if (prefix) {
        printf("%s: ", prefix);
    }
    printf("effect %d bbox(%d %d %d %d)\n",
           draw_item->effect,
           draw_item->base.rgn.extents.x1,
           draw_item->base.rgn.extents.y1,
           draw_item->base.rgn.extents.x2,
           draw_item->base.rgn.extents.y2);
}

static inline int pipe_item_is_linked(PipeItem *item)
{
    return ring_item_is_linked(&item->link);
}

static void red_pipe_add_verb(RedChannelClient* rcc, uint16_t verb)
{
    VerbItem *item = spice_new(VerbItem, 1);

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_VERB);
    item->verb = verb;
    red_channel_client_pipe_add(rcc, &item->base);
}

static inline void red_create_surface_item(DisplayChannelClient *dcc, int surface_id);
static void red_push_surface_image(DisplayChannelClient *dcc, int surface_id);

static void red_pipes_add_verb(RedChannel *channel, uint16_t verb)
{
    RedChannelClient *rcc;
    RingItem *link, *next;

    RCC_FOREACH_SAFE(link, next, rcc, channel) {
        red_pipe_add_verb(rcc, verb);
    }
}

//创建drawable的目标画布和依赖画布
static inline void red_handle_drawable_surfaces_client_synced(
                        DisplayChannelClient *dcc, Drawable *drawable)
{
    RedWorker *worker = DCC_TO_WORKER(dcc);
    int x;

	//要求客户端端先创建依赖画布，并且将依赖画布的内容画到客户端
    for (x = 0; x < 3; ++x) {
        int surface_id;

        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1) {
            if (dcc->surface_client_created[surface_id] == TRUE) {
                continue;
            }
            red_create_surface_item(dcc, surface_id);
            red_current_flush(worker, surface_id);
            red_push_surface_image(dcc, surface_id);
        }
    }

	// 客户端已经创建了这个surface，不用通知创建surface
    if (dcc->surface_client_created[drawable->surface_id] == TRUE) {
        return;
    }
	// 通知客户端创建surface
    red_create_surface_item(dcc, drawable->surface_id);
	// 将surface的所有命令
    red_current_flush(worker, drawable->surface_id);
	// 将画布图像发送到客户端
    red_push_surface_image(dcc, drawable->surface_id);
}

static int display_is_connected(RedWorker *worker)
{
    return (worker->display_channel && red_channel_is_connected(
        &worker->display_channel->common.base));
}

static int cursor_is_connected(RedWorker *worker)
{
    return (worker->cursor_channel && red_channel_is_connected(
        &worker->cursor_channel->common.base));
}

static void put_drawable_pipe_item(DrawablePipeItem *dpi)
{
    RedWorker *worker = DCC_TO_WORKER(dpi->dcc);

    if (--dpi->refs) {
        return;
    }

    spice_assert(!ring_item_is_linked(&dpi->dpi_pipe_item.link));
    spice_assert(!ring_item_is_linked(&dpi->base));
    release_drawable(worker, dpi->drawable);
    free(dpi);
}

static inline DrawablePipeItem *get_drawable_pipe_item(DisplayChannelClient *dcc,
                                                       Drawable *drawable)
{
    DrawablePipeItem *dpi;

    dpi = spice_malloc0(sizeof(*dpi));
    dpi->drawable = drawable;
    dpi->dcc = dcc;
    ring_item_init(&dpi->base);
    ring_add(&drawable->pipes, &dpi->base);
    red_channel_pipe_item_init(dcc->common.base.channel, &dpi->dpi_pipe_item, PIPE_ITEM_TYPE_DRAW);
    dpi->refs++;
    drawable->refs++; //一个DPI对Drawable添加一次引用
    return dpi;
}

static inline DrawablePipeItem *ref_drawable_pipe_item(DrawablePipeItem *dpi)
{
    spice_assert(dpi->drawable);
    dpi->refs++;
    return dpi;
}

static inline void red_pipe_add_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = get_drawable_pipe_item(dcc, drawable);
    red_channel_client_pipe_add(&dcc->common.base, &dpi->dpi_pipe_item);
}

static inline void red_pipes_add_drawable(RedWorker *worker, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item, *next;

    spice_warn_if(!ring_is_empty(&drawable->pipes));
    WORKER_FOREACH_DCC_SAFE(worker, dcc_ring_item, next, dcc) {
        red_pipe_add_drawable(dcc, drawable);
    }
}

static inline void red_pipe_add_drawable_to_tail(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;

    if (!dcc) {
        return;
    }
    red_handle_drawable_surfaces_client_synced(dcc, drawable);
    dpi = get_drawable_pipe_item(dcc, drawable);
    red_channel_client_pipe_add_tail(&dcc->common.base, &dpi->dpi_pipe_item);
}

//将drawable添加到pos_after命令的管道位置后面
static inline void red_pipes_add_drawable_after(RedWorker *worker,
	Drawable *drawable, Drawable *pos_after)
{
    DrawablePipeItem *dpi, *dpi_pos_after;
    RingItem *dpi_link, *dpi_next;
    DisplayChannelClient *dcc;
    int num_other_linked = 0; //pos_after要显示的客户端数

	//迭代pos_after的所有的管道项，创建drawable的管道项，添加在pos_after的管道项后
    DRAWABLE_FOREACH_DPI_SAFE(pos_after, dpi_link, dpi_next, dpi_pos_after) {
        num_other_linked++; //统计pos_after的显示客户端数
        dcc = dpi_pos_after->dcc; //恢复DCC
        // 如果客户端还没创建好surface
        red_handle_drawable_surfaces_client_synced(dcc, drawable);
		//创建一个新的DrawablePipeItem
        dpi = get_drawable_pipe_item(dcc, drawable);
		//天骄到dpi_pos_after的管道的后面，也就是说优先发送，因为管道命令从后开始发送
        red_channel_client_pipe_add_after(&dcc->common.base, &dpi->dpi_pipe_item,
                                          &dpi_pos_after->dpi_pipe_item);
    }
	// 如果一次都没迭代，添加到管道的头部
    if (num_other_linked == 0) {
        red_pipes_add_drawable(worker, drawable);
        return;
    }
    if (num_other_linked != worker->display_channel->common.base.clients_num) {
        RingItem *worker_item, *next;
        spice_debug("TODO: not O(n^2)");
        WORKER_FOREACH_DCC_SAFE(worker, worker_item, next, dcc) {
            int sent = 0;
            DRAWABLE_FOREACH_DPI_SAFE(pos_after, dpi_link, dpi_next, dpi_pos_after) {
                if (dpi_pos_after->dcc == dcc) {
                    sent = 1;
                    break;
                }
            }
            if (!sent) {
                red_pipe_add_drawable(dcc, drawable);
            }
        }
    }
}

static inline PipeItem *red_pipe_get_tail(DisplayChannelClient *dcc)
{
    if (!dcc) {
        return NULL;
    }

    return (PipeItem*)ring_get_tail(&dcc->common.base.pipe);
}

static inline void red_destroy_surface(RedWorker *worker, uint32_t surface_id);

static inline void red_pipes_remove_drawable(Drawable *drawable)
{
    DrawablePipeItem *dpi;
    RingItem *item, *next;

    RING_FOREACH_SAFE(item, next, &drawable->pipes) {
        dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, base);
        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
            red_channel_client_pipe_remove_and_release(&dpi->dcc->common.base,
                                                       &dpi->dpi_pipe_item);
        }
    }
}

static inline void red_pipe_add_image_item(DisplayChannelClient *dcc, ImageItem *item)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add(&dcc->common.base, &item->link);
}

static inline void red_pipe_add_image_item_after(DisplayChannelClient *dcc, ImageItem *item,
                                                 PipeItem *pos)
{
    if (!dcc) {
        return;
    }
    item->refs++;
    red_channel_client_pipe_add_after(&dcc->common.base, &item->link, pos);
}

static void release_image_item(ImageItem *item)
{
    if (!--item->refs) {
        free(item);
    }
}

static void release_upgrade_item(RedWorker* worker, UpgradeItem *item)
{
    if (!--item->refs) {
        release_drawable(worker, item->drawable);
        free(item->rects);
        free(item);
    }
}

static uint8_t *common_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    CommonChannel *common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);

    /* SPICE_MSGC_MIGRATE_DATA is the only client message whose size is dynamic */
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        return spice_malloc(size);
    }

    if (size > CHANNEL_RECEIVE_BUF_SIZE) {
        spice_critical("unexpected message size %u (max is %d)", size, CHANNEL_RECEIVE_BUF_SIZE);
        return NULL;
    }
    return common->recv_buf;
}

static void common_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size,
                                    uint8_t* msg)
{
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        free(msg);
    }
}

#define CLIENT_PIXMAPS_CACHE
#include "red_client_shared_cache.h"
#undef CLIENT_PIXMAPS_CACHE

#define CLIENT_CURSOR_CACHE
#include "red_client_cache.h"
#undef CLIENT_CURSOR_CACHE

#define CLIENT_PALETTE_CACHE
#include "red_client_cache.h"
#undef CLIENT_PALETTE_CACHE

static void red_reset_palette_cache(DisplayChannelClient *dcc)
{
    red_palette_cache_reset(dcc, CLIENT_PALETTE_CACHE_SIZE);
}

static void red_reset_cursor_cache(RedChannelClient *rcc)
{
    red_cursor_cache_reset(RCC_TO_CCC(rcc), CLIENT_CURSOR_CACHE_SIZE);
}

static inline Drawable *alloc_drawable(RedWorker *worker)
{
    Drawable *drawable;
    if (!worker->free_drawables) {
        return NULL;
    }
    drawable = &worker->free_drawables->u.drawable;
    worker->free_drawables = worker->free_drawables->u.next;
    return drawable;
}

static inline void free_drawable(RedWorker *worker, Drawable *item)
{
    ((_Drawable *)item)->u.next = worker->free_drawables;
    worker->free_drawables = (_Drawable *)item;
}

static void drawables_init(RedWorker *worker)
{
    int i;

    worker->free_drawables = NULL;
    for (i = 0; i < NUM_DRAWABLES; i++) {
        free_drawable(worker, &worker->drawables[i].u.drawable);
    }
}


static void red_reset_stream_trace(RedWorker *worker);

static SurfaceDestroyItem *get_surface_destroy_item(RedChannel *channel,
                                                    uint32_t surface_id)
{
    SurfaceDestroyItem *destroy;

    destroy = (SurfaceDestroyItem *)malloc(sizeof(SurfaceDestroyItem));
    spice_warn_if(!destroy);

    destroy->surface_destroy.surface_id = surface_id;

    red_channel_pipe_item_init(channel,
        &destroy->pipe_item, PIPE_ITEM_TYPE_DESTROY_SURFACE);

    return destroy;
}

static inline void red_destroy_surface_item(RedWorker *worker,
    DisplayChannelClient *dcc, uint32_t surface_id)
{
    SurfaceDestroyItem *destroy;
    RedChannel *channel;

    if (!dcc || worker->display_channel->common.during_target_migrate ||
        !dcc->surface_client_created[surface_id]) {
        return;
    }
    dcc->surface_client_created[surface_id] = FALSE;
    channel = &worker->display_channel->common.base;
    destroy = get_surface_destroy_item(channel, surface_id);
    red_channel_client_pipe_add(&dcc->common.base, &destroy->pipe_item);
}

// 解除对目标redsurface的引用，如果引用计数为0，则销毁redsurface
static inline void red_destroy_surface(RedWorker *worker, uint32_t surface_id)
{
    RedSurface *surface = &worker->surfaces[surface_id];
    DisplayChannelClient *dcc;
    RingItem *link, *next;

    if (!--surface->refs) {
        // only primary surface streams are supported
        if (is_primary_surface(worker, surface_id)) {
            red_reset_stream_trace(worker);
        }
        spice_assert(surface->context.canvas);

        surface->context.canvas->ops->destroy(surface->context.canvas);
        if (surface->create.info) {
            worker->qxl->st->qif->release_resource(worker->qxl, surface->create);
        }
        if (surface->destroy.info) {
            worker->qxl->st->qif->release_resource(worker->qxl, surface->destroy);
        }

        region_destroy(&surface->draw_dirty_region);
        surface->context.canvas = NULL;
        WORKER_FOREACH_DCC_SAFE(worker, link, next, dcc) {
            red_destroy_surface_item(worker, dcc, surface_id);
        }

        spice_warn_if(!ring_is_empty(&surface->depend_on_me));
    }
}

static inline void set_surface_release_info(RedWorker *worker, uint32_t surface_id, int is_create,
                                            QXLReleaseInfo *release_info, uint32_t group_id)
{
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];

    if (is_create) {
        surface->create.info = release_info;
        surface->create.group_id = group_id;
    } else {
        surface->destroy.info = release_info;
        surface->destroy.group_id = group_id;
    }
}

static RedDrawable *ref_red_drawable(RedDrawable *drawable)
{
    drawable->refs++;
    return drawable;
}


static inline void put_red_drawable(RedWorker *worker, RedDrawable *red_drawable,
                                    uint32_t group_id)
{
    QXLReleaseInfoExt release_info_ext;

    if (--red_drawable->refs) {
        return;
    }
    worker->red_drawable_count--;
    release_info_ext.group_id = group_id;
    release_info_ext.info = red_drawable->release_info;
    worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
    red_put_drawable(red_drawable);
    free(red_drawable);
}

// 重置依赖关系实体，即删除依赖关系
static void remove_depended_item(DependItem *item)
{
    spice_assert(item->drawable);
    spice_assert(ring_item_is_linked(&item->ring_item));
    item->drawable = NULL;
    ring_remove(&item->ring_item);
}

// 解除drawable对象对其依赖的redsurface的引用关系
static inline void 
red_dec_surfaces_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id == -1) {
            continue;
        }
		//对依赖的surface减引用
        red_destroy_surface(worker, surface_id);
    }
}

// 移除drawable对象的所有depend_on_me依赖关系
static void remove_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
        }
    }
}

// 释放Drawable对象的引用，如果引用为0，则释放
static inline void release_drawable(RedWorker *worker, Drawable *drawable)
{
    RingItem *item, *next;

    if (!--drawable->refs) {
		// 引用计数为0时，drawable的命令树节点必然没有shadow
		// 而且从所有发送管道中摘除了，或者没有加入到发送管道
        spice_assert(!drawable->tree_item.shadow);
        spice_assert(ring_is_empty(&drawable->pipes)); 

        if (drawable->stream) { //关联了流，解除流关联
            red_detach_stream(worker, drawable->stream, TRUE);
        }
		
		// 销毁命令树节点的rgn对象
        region_destroy(&drawable->tree_item.base.rgn);

		// 干掉drawable依赖关系
        remove_drawable_dependencies(worker, drawable);
		
		// 释放drawable时才解除对依赖surface的引用
        red_dec_surfaces_drawable_dependencies(worker, drawable);
		
		// 解除对目标surface的引用
        red_destroy_surface(worker, drawable->surface_id);

		// 解除RedGlzDrawable跟drawable的关联
        RING_FOREACH_SAFE(item, next, &drawable->glz_ring) {
            SPICE_CONTAINEROF(item, RedGlzDrawable, drawable_link)->drawable = NULL;
            ring_remove(item);
        }
		// 释放red_drawable
        put_red_drawable(worker, drawable->red_drawable, drawable->group_id);
		// 归还Drawable内存池
        free_drawable(worker, drawable);
		// 更新red_worker的命令计数
        worker->drawable_count--;
    }
}

/* 把命令树Draw节点的shadow从命令树中摘除并干掉，shadow被改掉后，
   该命令树draw节点的shadow指针指向NULL，如果该命令树draw节点没有shadow则直接返回。
*/
static inline void remove_shadow(RedWorker *worker, DrawItem *item)
{
    Shadow *shadow;

    if (!item->shadow) { //没有shadow则直接返回
        return;
    }
    shadow = item->shadow;
    item->shadow = NULL; //取消shadow关联
    ring_remove(&shadow->base.siblings_link); //将shadow从兄弟节点中摘除
    region_destroy(&shadow->base.rgn); //干掉shadow的区域
	//没有处理shadow->base.container，说明shadow的treeitem没有container节点
    region_destroy(&shadow->on_hold); //干掉shadow的on_hold
    free(shadow); //释放shadow节点
    worker->shadows_count--; //shadow计数递减
}

// 从命令树种移除container，要求container节点没有孩子节点了
static inline void current_remove_container(RedWorker *worker, Container *container)
{	
	//container所有的孩子节点都摘除了后才能删除
    spice_assert(ring_is_empty(&container->items));
	//更新redworker的container计数
    worker->containers_count--;
	//container节点移出命令树
    ring_remove(&container->base.siblings_link);
	//销毁container的rgn
    region_destroy(&container->base.rgn);
	//释放container内存
    free(container);
}

// 如果container只有一个孩子节点，或者没有孩子节点，则对container节点进行清理，
// 如果container只有一个孩子，则孩子替代container的位置，然后container被销毁，
// 如果container没有孩子，则直接删除container。
// 该过程会对container的祖先进行判断，直到上面的条件不满足时退出。
static inline void container_cleanup(RedWorker *worker, Container *container)
{
	// 如果container为真，且container只有一个孩子节点，或者没有孩子节点
    while (container && container->items.next == container->items.prev) {
		//container指向父节点
        Container *next = container->base.container;
		// 如果container的还有一个孩子节点
        if (container->items.next != &container->items) {
			//取出孩子节点
            TreeItem *item = (TreeItem *)ring_get_head(&container->items);
            spice_assert(item);
            ring_remove(&item->siblings_link); //孩子节点从container的items链表摘除
            //此时，container没有孩子节点了，孩子节点要取代container在container兄弟链表
            //中的位置，所以先将孩子节点插入到container的当前位置，后面current_remove_container
            //会将container从next的孩子链表中删除，从而达到取代位置的效果
            ring_add_after(&item->siblings_link, &container->base.siblings_link);
            item->container = container->base.container; //更新item的父节点
        }
		//移除container
        current_remove_container(worker, container);
        container = next;
    }
}

/* 根据Drawable创建流跟踪项 */
static inline void red_add_item_trace(RedWorker *worker, Drawable *item)
{
    ItemTrace *trace;
    if (!item->streamable) {//绘图命令不具备成流的条件，不用创建跟踪项
        return;
    }

	//取得跟踪项，循环跟踪，最多跟踪8个Drawcopy渲染命令
    trace = &worker->items_trace[worker->next_item_trace++ & ITEMS_TRACE_MASK];
    trace->time = item->creation_time; //复制绘图指令到达时间
    trace->frames_count = item->frames_count; //复制帧序号
    trace->gradual_frames_count = item->gradual_frames_count; //复制gradual序号
    trace->last_gradual_frame = item->last_gradual_frame;//最后一个gradual序号
    SpiceRect* src_area = &item->red_drawable->u.copy.src_area; //源矩形，源矩形可能跟目标矩形大小不一致，因为有scale的情况
    trace->width = src_area->right - src_area->left; //跟踪项的宽是源矩形的宽
    trace->height = src_area->bottom - src_area->top; //跟踪项的高是源矩形的高
    trace->dest_area = item->red_drawable->bbox; //流跟踪的目标区域取bbox矩形
}

static void surface_flush(RedWorker *worker, int surface_id, SpiceRect *rect)
{
    red_update_area(worker, rect, surface_id);
}

/* 刷新drawable的依赖surface的依赖区域 */
static void red_flush_source_surfaces(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            remove_depended_item(&drawable->depend_items[x]);
            surface_flush(worker, surface_id, &drawable->red_drawable->surfaces_rects[x]);
        }
    }
}

// 将一个drawable从命令树、redworker、surface中摘除
// 如果drawable是copybits,还会删除其shadow节点
// 摘除时会触发流跟踪
static inline void current_remove_drawable(RedWorker *worker, Drawable *item)
{
    if (item->tree_item.effect != QXL_EFFECT_OPAQUE) {
		//不是OPAQUE(非透明)渲染命令，就是透明渲染命令
        worker->transparent_count--; //透明命令数减去1
    }
    if (!item->stream) { //如果Drawable还关联到流，进行流跟踪
    	//当然只有可能进行流处理的才会跟踪，删除命令时跟踪，很有意思
    	//因为删除渲染命令，很可能是因为渲染命令完全被覆盖了。符合流的特性
        red_add_item_trace(worker, item);
    }
	//移除掉绘图命令的shadow
    remove_shadow(worker, &item->tree_item);
	//绘图命令从命令树摘除
    ring_remove(&item->tree_item.base.siblings_link);
	//从redworker的命令列表中摘除
    ring_remove(&item->list_link);
	//从redsurface的命令列表中摘除
    ring_remove(&item->surface_list_link);
	//从命令树中摘除，释放Drawable对象引用计数，当然也可能触发Drawable的释放
    release_drawable(worker, item);
	//更新redworker的绘图命令计数
    worker->current_size--;
}

// 从命令管道和命令树种移除drawable
static void remove_drawable(RedWorker *worker, Drawable *drawable)
{
	//从发送管道中摘除drawable
    red_pipes_remove_drawable(drawable);
	//从命令树种摘除drawable
    current_remove_drawable(worker, drawable);
}

// 将TreeItem节点及其子节点从命令树和发送管道中摘除，可能触发对应drawable的释放。
// treeitem只能是DRAWABLE和container类型，如果是container类型，要求其子孙节点中没有
// shadow节点了。
static inline void current_remove(RedWorker *worker, TreeItem *item)
{
    TreeItem *now = item;

    for (;;) {
        Container *container = now->container; //记下父节点
        RingItem *ring_item;

        if (now->type == TREE_ITEM_TYPE_DRAWABLE) {
            ring_item = now->siblings_link.prev; //记下命令树的前一个节点
            remove_drawable(worker, SPICE_CONTAINEROF(now, Drawable, tree_item));
        } else {
			//item不可能是shadow？这里直接转成Container了
            Container *container = (Container *)now;

            spice_assert(now->type == TREE_ITEM_TYPE_CONTAINER);

			//取container的第一个孩子节点，转到第一个
            if ((ring_item = ring_get_head(&container->items))) {
                now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
                continue;
            }
			//如果container没有孩子节点，移除container
            ring_item = now->siblings_link.prev;
            current_remove_container(worker, container);
        }

        if (now == item) { //item节点及其所有字节点都remove完毕，返回
            return;
        }

		// 因为是ring_next，所以，兄弟节点是从
        if ((ring_item = ring_next(&container->items, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        } else {
            now = (TreeItem *)container; //本层已经全部删除，往上回溯，
            //此时的container的孩子已经全部删除，可以删除container了
        }
    }
}

// 
static void current_tree_for_each(Ring *ring, void (*f)(TreeItem *, void *), void * data)
{
    RingItem *ring_item;
    Ring *top_ring;
	
	// current_tree 命令树为空
    if (!(ring_item = ring_get_head(ring))) {
        return;
    }
	// 顶层列表
    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);

        f(now, data);

        if (now->type == TREE_ITEM_TYPE_CONTAINER) {
            Container *container = (Container *)now;

            if ((ring_item = ring_get_head(&container->items))) {
                ring = &container->items;
                continue;
            }
        }
        for (;;) {
            ring_item = ring_next(ring, &now->siblings_link);
            if (ring_item) {
                break;
            }
            if (ring == top_ring) {
                return;
            }
            now = (TreeItem *)now->container;
            ring = (now->container) ? &now->container->items : top_ring;
        }
    }
}

static void red_current_clear(RedWorker *worker, int surface_id)
{
    RingItem *ring_item;

    while ((ring_item = ring_get_head(&worker->surfaces[surface_id].current))) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        current_remove(worker, now);
    }
}

/*
 * Return: TRUE if wait_if_used == FALSE, or otherwise, if all of the pipe items that
 * are related to the surface have been cleared (or sent) from the pipe.
 */
static int red_clear_surface_drawables_from_pipe(DisplayChannelClient *dcc, int surface_id,
                                                 int wait_if_used)
{
    Ring *ring;
    PipeItem *item;
    int x;
    RedChannelClient *rcc;

    if (!dcc) {
        return TRUE;
    }

    /* removing the newest drawables that their destination is surface_id and
       no other drawable depends on them */

    rcc = &dcc->common.base;
    ring = &dcc->common.base.pipe;
    item = (PipeItem *) ring;
    while ((item = (PipeItem *)ring_next(ring, (RingItem *)item))) {
        Drawable *drawable;
        DrawablePipeItem *dpi = NULL;
        int depend_found = FALSE;

        if (item->type == PIPE_ITEM_TYPE_DRAW) {
            dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
            drawable = dpi->drawable;
        } else if (item->type == PIPE_ITEM_TYPE_UPGRADE) {
            drawable = ((UpgradeItem *)item)->drawable;
        } else {
            continue;
        }

        if (drawable->surface_id == surface_id) {
            PipeItem *tmp_item = item;
            item = (PipeItem *)ring_prev(ring, (RingItem *)item);
            red_channel_client_pipe_remove_and_release(rcc, tmp_item);
            if (!item) {
                item = (PipeItem *)ring;
            }
            continue;
        }

        for (x = 0; x < 3; ++x) {
            if (drawable->surfaces_dest[x] == surface_id) {
                depend_found = TRUE;
                break;
            }
        }

        if (depend_found) {
            spice_debug("surface %d dependent item found %p, %p", surface_id, drawable, item);
            if (wait_if_used) {
                break;
            } else {
                return TRUE;
            }
        }
    }

    if (!wait_if_used) {
        return TRUE;
    }

    if (item) {
        return red_channel_client_wait_pipe_item_sent(&dcc->common.base, item,
                                                      DISPLAY_CLIENT_TIMEOUT);
    } else {
        /*
         * in case that the pipe didn't contain any item that is dependent on the surface, but
         * there is one during sending. Use a shorter timeout, since it is just one item
         */
        return red_channel_client_wait_outgoing_item(&dcc->common.base,
                                                     DISPLAY_CLIENT_SHORT_TIMEOUT);
    }
    return TRUE;
}

static void red_clear_surface_drawables_from_pipes(RedWorker *worker,
                                                   int surface_id,
                                                   int wait_if_used)
{
    RingItem *item, *next;
    DisplayChannelClient *dcc;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        if (!red_clear_surface_drawables_from_pipe(dcc, surface_id, wait_if_used)) {
            red_channel_client_disconnect(&dcc->common.base);
        }
    }
}

#ifdef PIPE_DEBUG

static void print_rgn(const char* prefix, const QRegion* rgn)
{
    int i;
    printf("TEST: %s: RGN: bbox %u %u %u %u\n",
           prefix,
           rgn->bbox.top,
           rgn->bbox.left,
           rgn->bbox.bottom,
           rgn->bbox.right);

    for (i = 0; i < rgn->num_rects; i++) {
        printf("TEST: %s: RECT %u %u %u %u\n",
               prefix,
               rgn->rects[i].top,
               rgn->rects[i].left,
               rgn->rects[i].bottom,
               rgn->rects[i].right);
    }
}

static void print_draw_item(const char* prefix, const DrawItem *draw_item)
{
    const TreeItem *base = &draw_item->base;
    const Drawable *drawable = SPICE_CONTAINEROF(draw_item, Drawable, tree_item);
    printf("TEST: %s: draw id %u container %u effect %u",
           prefix,
           base->id, base->container ? base->container->base.id : 0,
           draw_item->effect);
    if (draw_item->shadow) {
        printf(" shadow %u\n", draw_item->shadow->base.id);
    } else {
        printf("\n");
    }
    print_rgn(prefix, &base->rgn);
}

static void print_shadow_item(const char* prefix, const Shadow *item)
{
    printf("TEST: %s: shadow %p id %d\n", prefix, item, item->base.id);
    print_rgn(prefix, &item->base.rgn);
}

static void print_container_item(const char* prefix, const Container *item)
{
    printf("TEST: %s: container %p id %d\n", prefix, item, item->base.id);
    print_rgn(prefix, &item->base.rgn);
}

static void print_base_item(const char* prefix, const TreeItem *base)
{
    switch (base->type) {
    case TREE_ITEM_TYPE_DRAWABLE:
        print_draw_item(prefix, (const DrawItem *)base);
        break;
    case TREE_ITEM_TYPE_SHADOW:
        print_shadow_item(prefix, (const Shadow *)base);
        break;
    case TREE_ITEM_TYPE_CONTAINER:
        print_container_item(prefix, (const Container *)base);
        break;
    default:
        spice_error("invalid type %u", base->type);
    }
}

void __show_current(TreeItem *item, void *data)
{
    print_base_item("TREE", item);
}

static void show_current(RedWorker *worker, Ring *ring)
{
    if (ring_is_empty(ring)) {
        spice_info("TEST: TREE: EMPTY");
        return;
    }
    current_tree_for_each(ring, __show_current, NULL);
}

#else
#define print_rgn(a, b)
#define print_draw_private(a, b)
#define show_current(a, r)
#define print_shadow_item(a, b)
#define print_base_item(a, b)
#endif


static inline Shadow *__find_shadow(TreeItem *item)
{
	// 迭代container的子孙中，最后一个非container节点
    while (item->type == TREE_ITEM_TYPE_CONTAINER) {
		// 取container的孩子节点的最后一个节点。如果container孩子为空，返回空
        if (!(item = (TreeItem *)ring_get_tail(&((Container *)item)->items))) {
			//如果container是空链表返回NULL.
            return NULL;
        }
    }

	// 如果最后一个节点不是drawable(到这一行，item也一定不是container)
	// 即item是shadow，返回NULL
    if (item->type != TREE_ITEM_TYPE_DRAWABLE) {
        return NULL;
    }

	// 返回最后一个Drawable的shadow指针，（不确定是否一定为真）
    return ((DrawItem *)item)->shadow;
}

/* 如果item有container（父节点），返回item所在的兄弟链表，
 * 如果item没有container（父节点）了，则返回ring参数
 */
static inline Ring *ring_of(RedWorker *worker, Ring *ring, TreeItem *item)
{
    return (item->container) ? &item->container->items : ring;
}

// 判断item是否是ring这个链表树的孩子
static inline int __contained_by(RedWorker *worker, TreeItem *item, Ring *ring)
{
    spice_assert(item && ring);
	/* 1. item 没有父节点，直接返回true */
    do {
		// 如果item没有父节点了，ring_of返回ring，则now必然等于ring，函数返回true
		// 如果item有父节点，判断item所在的链表是否是ring，如果是返回true，如果不是
		// item变成item的节点，继续迭代
        Ring *now = ring_of(worker, ring, item);
        if (now == ring) {
            return TRUE;
        }
		// 迭代item的祖先节点，直到找到ring == item或者root节点
    } while ((item = (TreeItem *)item->container));
	
	// 如果item有父节点，但是其祖先都不是ring, 返回false。
    return FALSE;
}


/**
 * frame_candidate 当前调用环境里一直为空
**/
static inline void __exclude_region(RedWorker *worker, 
	Ring *ring, TreeItem *item, QRegion *rgn,
	Ring **top_ring, Drawable *frame_candidate)
{
    QRegion and_rgn;
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif

    region_clone(&and_rgn, rgn);//复制rgn

	//跟TreeItem求交集
    region_and(&and_rgn, &item->rgn); 

	//
	if (region_is_empty(&and_rgn))
		goto CLEAN;
	
	//跟Drawable排除
    if (IS_DRAW_ITEM(item)) {
        DrawItem *draw = (DrawItem *)item;

		//命令树命令是非透明命令，要干掉rgn中的交集部分
        if (draw->effect == QXL_EFFECT_OPAQUE) { 
            region_exclude(rgn, &and_rgn);
        }

		// 干掉命令树中命令可视部分的交集部分
		//当前命令有shadow（copybits命令，引用同一surface上的图像）
        if (draw->shadow) {
            Shadow *shadow;
			//取出当前命令的目标显示区域的外围矩形左上角顶点坐标
            int32_t x = item->rgn.extents.x1;
            int32_t y = item->rgn.extents.y1;

			//copybits命令排除掉交集部分
            region_exclude(&draw->base.rgn, &and_rgn);
            shadow = draw->shadow;
			
			//交集区域进行反向平移，交集区域变换到shawdow的坐标了
            region_offset(&and_rgn, shadow->base.rgn.extents.x1 - x,
                          shadow->base.rgn.extents.y1 - y);
			
			//排除掉shadow区域中的交集区域
            region_exclude(&shadow->base.rgn, &and_rgn);

			//求shadow中on_hold跟覆盖区域的交集，再次更新到and_rgn中
            region_and(&and_rgn, &shadow->on_hold);
            if (!region_is_empty(&and_rgn)) { //跟on_hold有交集
            	//shadow中on_hold区域也把排除区域干掉。
                region_exclude(&shadow->on_hold, &and_rgn);
				//原区域rgn加上与on_hold交集区域，为什么？
                region_or(rgn, &and_rgn);
                // in flat representation of current, shadow is always his owner next
				// 如果shadow不是top_ring链表树的孩子节点
				if (!__contained_by(worker, (TreeItem*)shadow, *top_ring)) {
					//更新top_ring链表为shadow所在的链表。
                    *top_ring = ring_of(worker, ring, (TreeItem*)shadow);
                }
            }
        } else {//非copybits
        	//此分支不生效，先注释掉
        	/*
            if (frame_candidate) {
                Drawable *drawable = SPICE_CONTAINEROF(draw, Drawable, tree_item);
                red_stream_maintenance(worker, frame_candidate, drawable);
            }
            */
            region_exclude(&draw->base.rgn, &and_rgn);
        }
    } else if (item->type == TREE_ITEM_TYPE_CONTAINER) { //CONTAINER类型
        region_exclude(&item->rgn, &and_rgn); //排除掉container中的rgn

        if (region_is_empty(&item->rgn)) {  //assume container removal will follow
            Shadow *shadow;

            region_exclude(rgn, &and_rgn);
            if ((shadow = __find_shadow(item))) {
                region_or(rgn, &shadow->on_hold);
				//
                if (!__contained_by(worker, (TreeItem*)shadow, *top_ring)) {
                    *top_ring = ring_of(worker, ring, (TreeItem*)shadow);
                }
            }
        }
    } else { //item是SHADOW类型
        Shadow *shadow;

        spice_assert(item->type == TREE_ITEM_TYPE_SHADOW);
        shadow = (Shadow *)item;
        region_exclude(rgn, &and_rgn); //rgn区域中排除掉与相交item的区域
        region_or(&shadow->on_hold, &and_rgn); //item的on_hold记录下相交区域
    }
    
CLEAN:
    region_destroy(&and_rgn);
    stat_add(&worker->__exclude_stat, start_time);
}

static void exclude_region(RedWorker *worker, Ring *ring, RingItem *ring_item, QRegion *rgn,
                           TreeItem **last, Drawable *frame_candidate)
{
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    Ring *top_ring;

    if (!ring_item) {
        return;
    }

    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container; //当前节点父节点

        spice_assert(!region_is_empty(&now->rgn));

        if (region_intersects(rgn, &now->rgn)) { //当前命令树节点与目标rgn相交
            print_base_item("EXCLUDE2", now);
            __exclude_region(worker, ring, now, rgn, &top_ring, frame_candidate);
            print_base_item("EXCLUDE3", now);

            if (region_is_empty(&now->rgn)) {
                spice_assert(now->type != TREE_ITEM_TYPE_SHADOW);
                ring_item = now->siblings_link.prev;
                print_base_item("EXCLUDE_REMOVE", now);
                current_remove(worker, now);
                if (last && *last == now) {
                    *last = (TreeItem *)ring_next(ring, ring_item);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                Container *container = (Container *)now;
                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    spice_assert(((TreeItem *)ring_item)->container);
                    continue;
                }
                ring_item = &now->siblings_link;
            }

            if (region_is_empty(rgn)) {
                stat_add(&worker->exclude_stat, start_time);
                return;
            }
        }

        while ((last && *last == (TreeItem *)ring_item) ||
               !(ring_item = ring_next(ring, ring_item))) {
            if (ring == top_ring) {
                stat_add(&worker->exclude_stat, start_time);
                return;
            }
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

// 为item创建一个Container，这个Container是item的父节点
static inline Container *__new_container(RedWorker *worker, DrawItem *item)
{
    Container *container = spice_new(Container, 1);
    worker->containers_count++;
#ifdef PIPE_DEBUG
    container->base.id = ++worker->last_id;
#endif
    container->base.type = TREE_ITEM_TYPE_CONTAINER;
    container->base.container = item->base.container; //container的contianer从子节点继承
    item->base.container = container;//更新叶子节点的container为新创建的界定啊
    item->container_root = TRUE; //叶子的container_root设置为真，表示是container的原生叶子
	//容器节点的区域和drawitem的区域一致
    region_clone(&container->base.rgn, &item->base.rgn); //复制目标区域
	//container替代drawitem的兄弟先后关系
    ring_item_init(&container->base.siblings_link);
	//先把container的sibling_link添加到drawitem的sibling_link之后，然后
	//将drawitem的sibling_link从兄弟节点中摘除，这样，container就替代了drawitem的位置了
    ring_add_after(&container->base.siblings_link, &item->base.siblings_link);
    ring_remove(&item->base.siblings_link);
	//初始化容器的孩子节点链表
    ring_init(&container->items);
	//drawitem成为container的第一个孩子节点
    ring_add(&container->items, &item->base.siblings_link);

    return container;
}

// 判断一个命令树节点是否为非透明节点
// 1. container节点是非透明的，
// 2. OPAQUE特效的drawitem也是非透明的
static inline int is_opaque_item(TreeItem *item)
{
    return item->type == TREE_ITEM_TYPE_CONTAINER ||
           (IS_DRAW_ITEM(item) && ((DrawItem *)item)->effect == QXL_EFFECT_OPAQUE);
}

//将drawable添加到命令树的pos位置之后，同时将drawable的接入到
//redworker的current_list的链表头部，将drawable的surface_list_link添加到redsurface的
//current_list链表头部
static inline void 
__current_add_drawable(RedWorker *worker, Drawable *drawable, RingItem *pos)
{
    RedSurface *surface;
    uint32_t surface_id = drawable->surface_id;

    surface = &worker->surfaces[surface_id];
	//drawable嵌入到命令树的pos位置之后
    ring_add_after(&drawable->tree_item.base.siblings_link, pos);
	//drawable分别添加到redworker和surface的对应链表头部
    ring_add(&worker->current_list, &drawable->list_link);
    ring_add(&surface->current_list, &drawable->surface_list_link);
	//增加redworker的命令计数
    worker->current_size++;
	//添加到命令树中，增加引用
    drawable->refs++;
}

static int is_equal_path(RedWorker *worker, SpicePath *path1, SpicePath *path2)
{
    SpicePathSeg *seg1, *seg2;
    int i, j;

    if (path1->num_segments != path2->num_segments)
        return FALSE;

    for (i = 0; i < path1->num_segments; i++) {
        seg1 = path1->segments[i];
        seg2 = path2->segments[i];

        if (seg1->flags != seg2->flags ||
            seg1->count != seg2->count) {
            return FALSE;
        }
        for (j = 0; j < seg1->count; j++) {
            if (seg1->points[j].x != seg2->points[j].x ||
                seg1->points[j].y != seg2->points[j].y) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

// partial imp
static int is_equal_brush(SpiceBrush *b1, SpiceBrush *b2)
{
    return b1->type == b2->type &&
           b1->type == SPICE_BRUSH_TYPE_SOLID &&
           b1->u.color == b2->u.color;
}

// partial imp
static int is_equal_line_attr(SpiceLineAttr *a1, SpiceLineAttr *a2)
{
    return a1->flags == a2->flags &&
           a1->style_nseg == a2->style_nseg &&
           a1->style_nseg == 0;
}

// partial imp
static int is_same_geometry(RedWorker *worker, Drawable *d1, Drawable *d2)
{
    if (d1->red_drawable->type != d2->red_drawable->type) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_line_attr(&d1->red_drawable->u.stroke.attr,
                                  &d2->red_drawable->u.stroke.attr) &&
               is_equal_path(worker, d1->red_drawable->u.stroke.path,
                             d2->red_drawable->u.stroke.path);
    case QXL_DRAW_FILL:
        return rect_is_equal(&d1->red_drawable->bbox, &d2->red_drawable->bbox);
    default:
        return FALSE;
    }
}

static int is_same_drawable(RedWorker *worker, Drawable *d1, Drawable *d2)
{
    if (!is_same_geometry(worker, d1, d2)) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_brush(&d1->red_drawable->u.stroke.brush,
                              &d2->red_drawable->u.stroke.brush);
    case QXL_DRAW_FILL:
        return is_equal_brush(&d1->red_drawable->u.fill.brush,
                              &d2->red_drawable->u.fill.brush);
    default:
        return FALSE;
    }
}
//释放流，归还到worker的空闲流链表
static inline void red_free_stream(RedWorker *worker, Stream *stream)
{
    if (stream->input_fps_timer) {
        spice_timer_remove(stream->input_fps_timer);
    }
    stream->next = worker->free_streams;
    worker->free_streams = stream;
}

//释放流引用
static void red_release_stream(RedWorker *worker, Stream *stream)
{
    if (!--stream->refs) {
        spice_assert(!ring_item_is_linked(&stream->link));
        red_free_stream(worker, stream);
        worker->stream_count--;
    }
}

// 将Stream的当前渲染命令与Stream取消关联，并将流的当前命令清空
// detach_sized控制是否干掉流当前命令的sized_stream关联
static inline void 
red_detach_stream(RedWorker *worker, Stream *stream, int detach_sized)
{
    spice_assert(stream->current && stream->current->stream);
    spice_assert(stream->current->stream == stream);
    stream->current->stream = NULL;
    if (detach_sized) {
        stream->current->sized_stream = NULL;
    }
    stream->current = NULL;
}

// 创建一个流裁剪管道项
static StreamClipItem *__new_stream_clip(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = spice_new(StreamClipItem, 1);
    red_channel_pipe_item_init(dcc->common.base.channel,
                    (PipeItem *)item, PIPE_ITEM_TYPE_STREAM_CLIP);

    item->stream_agent = agent; //设置流代理对象指针
    agent->stream->refs++; //对流增加引用计数
    item->refs = 1; //流裁剪管道项引用计数设置为1
    return item;
}

// 向DCC中推送一个流裁剪管道项，因为是管道项，所以直接记录的是流代理指针
// 但是裁剪区域从此刻的流代理裁剪区域中取出，因为流代理的裁剪区域可能会变化
static void push_stream_clip(DisplayChannelClient* dcc, StreamAgent *agent)
{
    StreamClipItem *item = __new_stream_clip(dcc, agent);
    int n_rects;

    if (!item) {
        spice_critical("alloc failed");
    }
    item->clip_type = SPICE_CLIP_TYPE_RECTS; //矩形裁剪

    n_rects = pixman_region32_n_rects(&agent->clip); //矩形数量

    item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
    item->rects->num_rects = n_rects;
	//裁剪QRegion转成SpiceRect数组
    region_ret_rects(&agent->clip, item->rects->rects, n_rects);
	//传入流裁剪管道项
    red_channel_client_pipe_add(&dcc->common.base, (PipeItem *)item);
}

// 释放流裁剪对象
static void red_display_release_stream_clip(RedWorker *worker, StreamClipItem *item)
{
    if (!--item->refs) { //引用为0 
        red_display_release_stream(worker, item->stream_agent);
        free(item->rects); //释放矩形
        free(item); //释放本身
    }
}

// 获取流id，stream对象一定来自于worker的streams_buf，所以直接用地址下标作为流的id
static inline int get_stream_id(RedWorker *worker, Stream *stream)
{
    return (int)(stream - worker->streams_buf);
}

// 将一个drawabe和stream关联起来
static void red_attach_stream(RedWorker *worker, Drawable *drawable, Stream *stream)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    spice_assert(!drawable->stream && !stream->current);
    spice_assert(drawable && stream);
    stream->current = drawable; //更新流的最后一个drawable，drawable没加引用没问题吗
    drawable->stream = stream; //流关联到drawable
    stream->last_time = drawable->creation_time; //更新流的最后到达时间
    //流增加一帧
    stream->num_input_frames++;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        StreamAgent *agent;
        QRegion clip_in_draw_dest;
		//获取流代理，在创建流时会为每个dcc创建流代理
        agent = &dcc->stream_agents[get_stream_id(worker, stream)];
		//更新流代理的可视区域，为什么用or, 因为drawable的bbox可能匹配
		//到流了，但是这条流中各个drawable的clip可能会变，如视频区域上
		//有个窗口，对窗口进行拖动，底层的视频贴图命令的bbox不会变，但是
		//其裁剪区域会变化，造成drawable的tree_item的rgn会变化，也就是视频
		//流的可视区域会变化，因为drawable可能积累起来了，流代理中原来可能就有
		//可视区域，所以agent->vis_region区域是积累的视频流可视的最大区域。
		//如果遮挡变大，vis_region是最早命令的rgn，如果遮挡变小，vis_region是
		//最后命令的rgn。总之回事drawable积累的最大视频可视区域。
        region_or(&agent->vis_region, &drawable->tree_item.base.rgn);

		//计算当前drawable经过流代理的裁剪后区域
        region_init(&clip_in_draw_dest);
        region_add(&clip_in_draw_dest, &drawable->red_drawable->bbox);
        region_and(&clip_in_draw_dest, &agent->clip); 

		// 流代理的裁剪区域和drawable的裁剪区域不一致时，更新流代理的裁剪区域
        if (!region_is_equal(&clip_in_draw_dest, &drawable->tree_item.base.rgn)) {
			//先把bbox整个从clip中干掉，然后把当前drawable的裁剪后区域加到clip
			//中，形成新的裁剪区域
            region_remove(&agent->clip, &drawable->red_drawable->bbox);//
            region_or(&agent->clip, &drawable->tree_item.base.rgn);
			//让客户端使用agent的clip重现裁剪流区域。
            push_stream_clip(dcc, agent);
        }
#ifdef STREAM_STATS
        agent->stats.num_input_frames++;
#endif
    }
}

static void red_print_stream_stats(DisplayChannelClient *dcc, StreamAgent *agent)
{
#ifdef STREAM_STATS
    StreamStats *stats = &agent->stats;
    double passed_mm_time = (stats->end - stats->start) / 1000.0;
    MJpegEncoderStats encoder_stats = {0};

    if (agent->mjpeg_encoder) {
        mjpeg_encoder_get_stats(agent->mjpeg_encoder, &encoder_stats);
    }

    spice_debug("stream=%"PRIdPTR" dim=(%dx%d) #in-frames=%"PRIu64" #in-avg-fps=%.2f #out-frames=%"PRIu64" "
                "out/in=%.2f #drops=%"PRIu64" (#pipe=%"PRIu64" #fps=%"PRIu64") out-avg-fps=%.2f "
                "passed-mm-time(sec)=%.2f size-total(MB)=%.2f size-per-sec(Mbps)=%.2f "
                "size-per-frame(KBpf)=%.2f avg-quality=%.2f "
                "start-bit-rate(Mbps)=%.2f end-bit-rate(Mbps)=%.2f",
                agent - dcc->stream_agents, agent->stream->width, agent->stream->height,
                stats->num_input_frames,
                stats->num_input_frames / passed_mm_time,
                stats->num_frames_sent,
                (stats->num_frames_sent + 0.0) / stats->num_input_frames,
                stats->num_drops_pipe +
                stats->num_drops_fps,
                stats->num_drops_pipe,
                stats->num_drops_fps,
                stats->num_frames_sent / passed_mm_time,
                passed_mm_time,
                stats->size_sent / 1024.0 / 1024.0,
                ((stats->size_sent * 8.0) / (1024.0 * 1024)) / passed_mm_time,
                stats->size_sent / 1000.0 / stats->num_frames_sent,
                encoder_stats.avg_quality,
                encoder_stats.starting_bit_rate / (1024.0 * 1024),
                encoder_stats.cur_bit_rate / (1024.0 * 1024));
#endif
}

static void red_stop_stream(RedWorker *worker, Stream *stream)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    spice_assert(ring_item_is_linked(&stream->link));
	//停流的时候current指针必须为空
    spice_assert(!stream->current);
    spice_debug("stream %d", get_stream_id(worker, stream));
    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        StreamAgent *stream_agent;

        stream_agent = &dcc->stream_agents[get_stream_id(worker, stream)];
        region_clear(&stream_agent->vis_region);
        region_clear(&stream_agent->clip);
        spice_assert(!pipe_item_is_linked(&stream_agent->destroy_item));
        if (stream_agent->mjpeg_encoder && dcc->use_mjpeg_encoder_rate_control) {
            uint64_t stream_bit_rate = mjpeg_encoder_get_bit_rate(stream_agent->mjpeg_encoder);

            if (stream_bit_rate > dcc->streams_max_bit_rate) {
                spice_debug("old max-bit-rate=%.2f new=%.2f",
                dcc->streams_max_bit_rate / 8.0 / 1024.0 / 1024.0,
                stream_bit_rate / 8.0 / 1024.0 / 1024.0);
                dcc->streams_max_bit_rate = stream_bit_rate;
            }
        }
		//增加是一个引用，因为stream_agent->destroy_item命令会
		//释放视频流，而此函数后面也会调用red_release_stream
		//用于事件响应后最后释放
        stream->refs++;
        red_channel_client_pipe_add(&dcc->common.base, &stream_agent->destroy_item);
        red_print_stream_stats(dcc, stream_agent);
    }
    worker->streams_size_total -= stream->width * stream->height;
    ring_remove(&stream->link);
    red_release_stream(worker, stream);
}

/* 判断渲染命令是否在dcc的发送管道中 */
static int red_display_drawable_is_in_pipe(DisplayChannelClient *dcc, Drawable *drawable)
{
    DrawablePipeItem *dpi;
    RingItem *dpi_link, *dpi_next;

    DRAWABLE_FOREACH_DPI_SAFE(drawable, dpi_link, dpi_next, dpi) {
        if (dpi->dcc == dcc) {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * after red_display_detach_stream_gracefully is called for all the display channel clients,
 * red_detach_stream should be called. See comment (1).
 * 当对显示通道的所有DCC调用了red_display_detach_stream_gracefully之后，必须调用
 * red_detach_stream。本函数让dcc不再跟stream关联，将让流的最后一帧刷新到流的显示区域
 * 
 */
static inline void red_display_detach_stream_gracefully(DisplayChannelClient *dcc,
                                                        Stream *stream,
                                                        Drawable *update_area_limit)
{
    int stream_id = get_stream_id(dcc->common.worker, stream);
    StreamAgent *agent = &dcc->stream_agents[stream_id]; //stream在dcc中的代理

    /* stopping the client from playing older frames at once*/
	// 马上通知客户端不要播放老的帧
	// 清空流代理的裁剪区域，也就是说裁剪区域变成0了，也就是流没有显示区域了。
	// 并且让客户端对流进行重新裁剪，从而达到让客户端停止播放老帧的目的
    region_clear(&agent->clip);
    push_stream_clip(dcc, agent);

	// 如果流代理的可视区域为空，那么不用处理了
    if (region_is_empty(&agent->vis_region)) {
        spice_debug("stream %d: vis region empty", stream_id);
        return;
    }

	// 如果流的最后一条显示命令不空且最后一条命令包含了流代理的可视区域
    if (stream->current &&
        region_contains(&stream->current->tree_item.base.rgn, &agent->vis_region)) {
        //如果流当前有帧，且当前帧覆盖了显示区域的全部.
        RedChannel *channel;
        RedChannelClient *rcc;
        UpgradeItem *upgrade_item;
        int n_rects;

        /* (1) The caller should detach the drawable from the stream. This will
         * lead to sending the drawable losslessly, as an ordinary drawable. */
        //流的当前命令已经在dcc管道中 
        if (red_display_drawable_is_in_pipe(dcc, stream->current)) {			
            spice_debug("stream %d: upgrade by linked drawable. sized %d, box ==>",
                        stream_id, stream->current->sized_stream != NULL);
            rect_debug(&stream->current->red_drawable->bbox);
            goto clear_vis_region;
        }
		//流当前drawable还不在管道中，将流的drawable对象作为drawcopy发送出去
		//就是直接渲染current?
        spice_debug("stream %d: upgrade by drawable. sized %d, box ==>",
                    stream_id, stream->current->sized_stream != NULL);
        rect_debug(&stream->current->red_drawable->bbox);
        rcc = &dcc->common.base;
        channel = rcc->channel;
        upgrade_item = spice_new(UpgradeItem, 1);
        upgrade_item->refs = 1;
        red_channel_pipe_item_init(channel,
                &upgrade_item->base, PIPE_ITEM_TYPE_UPGRADE);
        upgrade_item->drawable = stream->current;
        upgrade_item->drawable->refs++;
		//流当前drawable
        n_rects = pixman_region32_n_rects(&upgrade_item->drawable->tree_item.base.rgn);
        upgrade_item->rects = spice_malloc_n_m(n_rects, sizeof(SpiceRect), sizeof(SpiceClipRects));
        upgrade_item->rects->num_rects = n_rects;
        region_ret_rects(&upgrade_item->drawable->tree_item.base.rgn,
                         upgrade_item->rects->rects, n_rects);
        red_channel_client_pipe_add(rcc, &upgrade_item->base);

    } else {
        SpiceRect upgrade_area;
		//流的可视区域作为更新区域
        region_extents(&agent->vis_region, &upgrade_area);
        spice_debug("stream %d: upgrade by screenshot. has current %d. box ==>",
                    stream_id, stream->current != NULL);
        rect_debug(&upgrade_area);
        if (update_area_limit) {
            red_update_area_till(dcc->common.worker, &upgrade_area, 0, update_area_limit);
        } else {
            red_update_area(dcc->common.worker, &upgrade_area, 0);
        }
		//刷新DCC的主surface的upgrade_area
        red_add_surface_area_image(dcc, 0, &upgrade_area, NULL, FALSE);
    }
clear_vis_region:
	//流的可视区域变0
    region_clear(&agent->vis_region);
}

static inline void red_detach_stream_gracefully(RedWorker *worker, Stream *stream,
                                                Drawable *update_area_limit)
{
    RingItem *item, *next;
    DisplayChannelClient *dcc;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        red_display_detach_stream_gracefully(dcc, stream, update_area_limit);
    }
    if (stream->current) {
        red_detach_stream(worker, stream, TRUE);
    }
}

/*
 * region  : a primary surface region. Streams that intersects with the given
 *           region will be detached.//主surface区域，如果流与给定region相交，该流会detach
 * drawable: If detaching the stream is triggered by the addition of a new drawable
 *           that is dependent on the given region, and the drawable is already a part
 *           of the "current tree", the drawable parameter should be set with
 *           this drawable, otherwise, it should be NULL. Then, if detaching the stream
 *           involves sending an upgrade image to the client, this drawable won't be rendered
 *           (see red_display_detach_stream_gracefully).
 * 如果正在detaching的流是因为添加一个依赖于指定区域的drawable造成的，并且这个drawable已经是
 * current命令树的一部分，必须设置drawable参数，否则drawable应该设置为NULL。而且，如果detaching的
 * 流正在发送一副更新图像到客户端，这个drawable将不会被渲染(见red_display_detach_stream_gracefully)
 */
static void red_detach_streams_behind(RedWorker *worker, QRegion *region, Drawable *drawable)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);
    RingItem *dcc_ring_item, *next;
    DisplayChannelClient *dcc;
    int has_clients = display_is_connected(worker); //显示通道是否有客户端连接

	//迭代显示通道所有流
    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        int detach_stream = 0;
        item = ring_next(ring, item);

		//迭代所有dcc
        WORKER_FOREACH_DCC_SAFE(worker, dcc_ring_item, next, dcc) {
        	//找到dcc上的流代理
            StreamAgent *agent = &dcc->stream_agents[get_stream_id(worker, stream)];

            if (region_intersects(&agent->vis_region, region)) { //与流可视区域相交
                red_display_detach_stream_gracefully(dcc, stream, drawable);
                detach_stream = 1;
                spice_debug("stream %d", get_stream_id(worker, stream));
            }
        }
        if (detach_stream && stream->current) {//如果detach了流，并且当前流有drawable
            red_detach_stream(worker, stream, TRUE);
        } else if (!has_clients) { //如果没有客户端，并且流有当前命令，且与当前命令相交
            if (stream->current &&
                region_intersects(&stream->current->tree_item.base.rgn, region)) {
                red_detach_stream(worker, stream, TRUE);
            }
        }
    }
}

static void red_streams_update_visible_region(RedWorker *worker, Drawable *drawable)
{
    Ring *ring;
    RingItem *item;
    RingItem *dcc_ring_item, *next;
    DisplayChannelClient *dcc;

    if (!display_is_connected(worker)) {//显示通道未连接不更新
        return;
    }

    if (!is_primary_surface(worker, drawable->surface_id)) {//非主通道不更新
        return;
    }

    ring = &worker->streams; //取流列表
    item = ring_get_head(ring); 

	//迭代流列表
    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        StreamAgent *agent;

        item = ring_next(ring, item);

		// 跳过当前命令的那条流，因为该命令对画面的影响已经反馈到流中间
        if (stream->current == drawable) {
            continue;
        }
		// 当前命令不属于当前stream，判断一下是否会覆盖流的可视区域
		//迭代当前显示通道的所有dcc
        WORKER_FOREACH_DCC_SAFE(worker, dcc_ring_item, next, dcc) {
        	//获取流在dcc中的流代理
            agent = &dcc->stream_agents[get_stream_id(worker, stream)];
			//如果当前命令的可视区域跟流代理的可视区域相交
            if (region_intersects(&agent->vis_region, &drawable->tree_item.base.rgn)) {
				//有相交则排除掉可视命令的区域
                region_exclude(&agent->vis_region, &drawable->tree_item.base.rgn);
				//同时也排除掉裁剪区域
                region_exclude(&agent->clip, &drawable->tree_item.base.rgn);
                push_stream_clip(dcc, agent);
            }
        }
    }
}

static inline unsigned int red_get_streams_timout(RedWorker *worker)
{
    unsigned int timout = -1;
    Ring *ring = &worker->streams;
    RingItem *item = ring;
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    red_time_t now = timespec_to_red_time(&time);
    while ((item = ring_next(ring, item))) {
        Stream *stream;

        stream = SPICE_CONTAINEROF(item, Stream, link);
        red_time_t delta = (stream->last_time + RED_STREAM_TIMOUT) - now;

        if (delta < 1000 * 1000) {
            return 0;
        }
        timout = MIN(timout, (unsigned int)(delta / (1000 * 1000)));
    }
    return timout;
}

static inline void red_handle_streams_timout(RedWorker *worker)
{
    Ring *ring = &worker->streams;
    struct timespec time;
    RingItem *item;

    clock_gettime(CLOCK_MONOTONIC, &time);
    red_time_t now = timespec_to_red_time(&time);
    item = ring_get_head(ring);
    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (now >= (stream->last_time + RED_STREAM_TIMOUT)) {
            red_detach_stream_gracefully(worker, stream, NULL);
            red_stop_stream(worker, stream);
        }
    }
}

//解除对StreamAgent的引用
static void red_display_release_stream(RedWorker *worker, StreamAgent *agent)
{
    spice_assert(agent->stream);
    red_release_stream(worker, agent->stream);
}

static inline Stream *red_alloc_stream(RedWorker *worker)
{
    Stream *stream;
    if (!worker->free_streams) {
        return NULL;
    }
    stream = worker->free_streams;
    worker->free_streams = worker->free_streams->next;
    return stream;
}

static uint64_t red_stream_get_initial_bit_rate(DisplayChannelClient *dcc,
                                                Stream *stream)
{
    char *env_bit_rate_str;
    uint64_t bit_rate = 0;

    env_bit_rate_str = getenv("SPICE_BIT_RATE");
    if (env_bit_rate_str != NULL) {
        double env_bit_rate;

        errno = 0;
        env_bit_rate = strtod(env_bit_rate_str, NULL);
        if (errno == 0) {
            bit_rate = env_bit_rate * 1024 * 1024;
        } else {
            spice_warning("error parsing SPICE_BIT_RATE: %s", strerror(errno));
        }
    }

    if (!bit_rate) {
        MainChannelClient *mcc;
        uint64_t net_test_bit_rate;

        mcc = red_client_get_main(dcc->common.base.client);
        net_test_bit_rate = main_channel_client_is_network_info_initialized(mcc) ?
                                main_channel_client_get_bitrate_per_sec(mcc) :
                                0;
        bit_rate = MAX(dcc->streams_max_bit_rate, net_test_bit_rate);
        if (bit_rate == 0) {
            /*
             * In case we are after a spice session migration,
             * the low_bandwidth flag is retrieved from migration data.
             * If the network info is not initialized due to another reason,
             * the low_bandwidth flag is FALSE.
             */
            bit_rate = dcc->common.is_low_bandwidth ?
                RED_STREAM_DEFAULT_LOW_START_BIT_RATE :
                RED_STREAM_DEFAULT_HIGH_START_BIT_RATE;
        }
    }

    spice_debug("base-bit-rate %.2f (Mbps)", bit_rate / 1024.0 / 1024.0);
    /* dividing the available bandwidth among the active streams, and saving
     * (1-RED_STREAM_CHANNEL_CAPACITY) of it for other messages */
    return (RED_STREAM_CHANNEL_CAPACITY * bit_rate *
           stream->width * stream->height) / dcc->common.worker->streams_size_total;
}

static uint32_t red_stream_mjpeg_encoder_get_roundtrip(void *opaque)
{
    StreamAgent *agent = opaque;
    int roundtrip;

    spice_assert(agent);
    roundtrip = red_channel_client_get_roundtrip_ms(&agent->dcc->common.base);
    if (roundtrip < 0) { //如果rcc还没有获得rtt，则返回主通道的rtt
        MainChannelClient *mcc = 
			red_client_get_main(agent->dcc->common.base.client);

        /*
         * the main channel client roundtrip might not have been
         * calculated (e.g., after migration). In such case,
         * main_channel_client_get_roundtrip_ms returns 0.
         */
        roundtrip = main_channel_client_get_roundtrip_ms(mcc);
    }

    return roundtrip;
}

static uint32_t red_stream_mjpeg_encoder_get_source_fps(void *opaque)
{
    StreamAgent *agent = opaque;

    spice_assert(agent);
    return agent->stream->input_fps; //返回输入的fps
}

static void red_display_update_streams_max_latency(DisplayChannelClient *dcc, StreamAgent *remove_agent)
{
    uint32_t new_max_latency = 0;
    int i;

    if (dcc->streams_max_latency != remove_agent->client_required_latency) {
        return;
    }

    dcc->streams_max_latency = 0;
    if (dcc->common.worker->stream_count == 1) {
        return;
    }
    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *other_agent = &dcc->stream_agents[i];
        if (other_agent == remove_agent || !other_agent->mjpeg_encoder) {
            continue;
        }
        if (other_agent->client_required_latency > new_max_latency) {
            new_max_latency = other_agent->client_required_latency;
        }
    }
    dcc->streams_max_latency = new_max_latency;
}

static void red_display_stream_agent_stop(DisplayChannelClient *dcc, StreamAgent *agent)
{
    red_display_update_streams_max_latency(dcc, agent);
    if (agent->mjpeg_encoder) {
        mjpeg_encoder_destroy(agent->mjpeg_encoder);
        agent->mjpeg_encoder = NULL;
    }
}

static void red_stream_update_client_playback_latency(void *opaque, uint32_t delay_ms)
{
    StreamAgent *agent = opaque;
    DisplayChannelClient *dcc = agent->dcc;

    red_display_update_streams_max_latency(dcc, agent);

    agent->client_required_latency = delay_ms;
    if (delay_ms > agent->dcc->streams_max_latency) {
         agent->dcc->streams_max_latency = delay_ms;
    }
    spice_debug("reseting client latency: %u", agent->dcc->streams_max_latency);
    main_dispatcher_set_mm_time_latency(agent->dcc->common.base.client, agent->dcc->streams_max_latency);
}

/**  
 * 当显示通道创建一条图像流时调用
**/
static void red_display_create_stream(DisplayChannelClient *dcc, Stream *stream)
{
    StreamAgent *agent = &dcc->stream_agents[get_stream_id(dcc->common.worker, stream)];

    stream->refs++; //每个DCC都持有stream，所以stream加引用
    spice_assert(region_is_empty(&agent->vis_region));
    if (stream->current) { //有当前drawable
        agent->frames = 1; //创建时有drawable则帧数为1，否则为0
        //复制区域
        region_clone(&agent->vis_region, &stream->current->tree_item.base.rgn);
		//裁剪区域等于vis_region
        region_clone(&agent->clip, &agent->vis_region);
    } else {
        agent->frames = 0;
    }
    agent->drops = 0; //丢帧数
    agent->fps = MAX_FPS; //每秒帧数
    agent->dcc = dcc; //StreamAgent关联到dcc

    if (dcc->use_mjpeg_encoder_rate_control) {
        MJpegEncoderRateControlCbs mjpeg_cbs;
        uint64_t initial_bit_rate;

        mjpeg_cbs.get_roundtrip_ms = red_stream_mjpeg_encoder_get_roundtrip;
        mjpeg_cbs.get_source_fps = red_stream_mjpeg_encoder_get_source_fps;
        mjpeg_cbs.update_client_playback_delay = 
			red_stream_update_client_playback_latency;
		//获取配置的比特率，码率
        initial_bit_rate = red_stream_get_initial_bit_rate(dcc, stream);
		//创建mjpeg编码器
        agent->mjpeg_encoder = 
        	mjpeg_encoder_new(TRUE, initial_bit_rate, &mjpeg_cbs, agent);
    } else {
		//直接创建mjpeg编码器
        agent->mjpeg_encoder = mjpeg_encoder_new(FALSE, 0, NULL, NULL);
    }
	//项发送管道添加创建命令，貌似都没设置命令类型啊
    red_channel_client_pipe_add(&dcc->common.base, &agent->create_item);

    if (red_channel_client_test_remote_cap(&dcc->common.base, 
		SPICE_DISPLAY_CAP_STREAM_REPORT)) //客户端是否支持流报告
	{
        StreamActivateReportItem *report_pipe_item = 
			spice_malloc0(sizeof(*report_pipe_item));

        agent->report_id = rand();
		//初始化管道项
        red_channel_pipe_item_init(dcc->common.base.channel, 
			&report_pipe_item->pipe_item,
			PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT);
		//设置流id
        report_pipe_item->stream_id = get_stream_id(dcc->common.worker, stream);
		//添加管道命令
        red_channel_client_pipe_add(&dcc->common.base, &report_pipe_item->pipe_item);
    }
#ifdef STREAM_STATS
    memset(&agent->stats, 0, sizeof(StreamStats));
    if (stream->current) { //mm_time当开始时间
        agent->stats.start = stream->current->red_drawable->mm_time;
    }
#endif
}

//流定时器的定时执行函数
static void red_stream_input_fps_timer_cb(void *opaque)
{
    Stream *stream = opaque;
    uint64_t now = red_now();
    double duration_sec;

    spice_assert(opaque);
	//刚刚启动定时器时不执行，跳过这一次
    if (now == stream->input_fps_timer_start) {
        spice_warning("timer start and expiry time are equal");
        return;
    }
	//计算持续时间单位秒，double型
    duration_sec = (now - stream->input_fps_timer_start)/(1000.0*1000*1000);
	//更新帧率
    stream->input_fps = stream->num_input_frames / duration_sec;
    spice_debug("input-fps=%u", stream->input_fps);
	//重置帧率计数器
    stream->num_input_frames = 0;
    stream->input_fps_timer_start = now;
}

// 根据当前的drawable创建图像流
static void red_create_stream(RedWorker *worker, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    RingItem *dcc_ring_item, *next;
    Stream *stream;
    SpiceRect* src_rect;

    spice_assert(!drawable->stream);

	// 从空闲图像流列表分配一个空闲图像流
    if (!(stream = red_alloc_stream(worker))) {
        return;
    }

	//只有Draw_COPY才能创建图像流
    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);
	//取出目标区域
    src_rect = &drawable->red_drawable->u.copy.src_area;

	//添加到使用的流列表
    ring_add(&worker->streams, &stream->link);
	//设置图像流的当前Drawable
    stream->current = drawable;
	//设置上一个drawable的创建时间（到达时间）
    stream->last_time = drawable->creation_time;
    stream->width = src_rect->right - src_rect->left; //计算宽
    stream->height = src_rect->bottom - src_rect->top; //计算稿
    stream->dest_area = drawable->red_drawable->bbox; //drawcopy的目标区域
    stream->refs = 1;
    SpiceBitmap *bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;
    stream->top_down = !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN);//TOP_DOWN标记
    drawable->stream = stream;//设置Drawable的图像流指针
    //添加图像流计时器
    stream->input_fps_timer = spice_timer_queue_add(red_stream_input_fps_timer_cb, stream);
    spice_assert(stream->input_fps_timer);
	//五秒运行一次定时器
    spice_timer_set(stream->input_fps_timer, RED_STREAM_INPUT_FPS_TIMEOUT);
    stream->num_input_frames = 0; //帧数为0
    stream->input_fps_timer_start = red_now();
    stream->input_fps = MAX_FPS; //最大30帧每秒
    worker->streams_size_total += stream->width * stream->height; //宽乘以高
    worker->stream_count++; //图像流数量
    WORKER_FOREACH_DCC_SAFE(worker, dcc_ring_item, next, dcc) { 
		//告知每个DCC，创建了一个流代理
        red_display_create_stream(dcc, stream);
    }
    spice_debug("stream %d %dx%d (%d, %d) (%d, %d)", (int)(stream - worker->streams_buf), stream->width,
                stream->height, stream->dest_area.left, stream->dest_area.top,
                stream->dest_area.right, stream->dest_area.bottom);
    return;
}

// 为
static void red_disply_start_streams(DisplayChannelClient *dcc)
{
    Ring *ring = &dcc->common.worker->streams;
    RingItem *item = ring;

    while ((item = ring_next(ring, item))) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        red_display_create_stream(dcc, stream);
    }
}

static void red_display_client_init_streams(DisplayChannelClient *dcc)
{
    int i;
    RedWorker *worker = dcc->common.worker;
    RedChannel *channel = dcc->common.base.channel;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        agent->stream = &worker->streams_buf[i];
        region_init(&agent->vis_region);
        region_init(&agent->clip);
        red_channel_pipe_item_init(channel, &agent->create_item, PIPE_ITEM_TYPE_STREAM_CREATE);
        red_channel_pipe_item_init(channel, &agent->destroy_item, PIPE_ITEM_TYPE_STREAM_DESTROY);
    }
    dcc->use_mjpeg_encoder_rate_control =
        red_channel_client_test_remote_cap(&dcc->common.base, SPICE_DISPLAY_CAP_STREAM_REPORT);
}

static void red_display_destroy_streams_agents(DisplayChannelClient *dcc)
{
    int i;

    for (i = 0; i < NUM_STREAMS; i++) {
        StreamAgent *agent = &dcc->stream_agents[i];
        region_destroy(&agent->vis_region);
        region_destroy(&agent->clip);
        if (agent->mjpeg_encoder) {
            mjpeg_encoder_destroy(agent->mjpeg_encoder);
            agent->mjpeg_encoder = NULL;
        }
    }
}

// 初始化RedWorker的图像流
static void red_init_streams(RedWorker *worker)
{
    int i;

    ring_init(&worker->streams); //初始化流链表
    worker->free_streams = NULL; //空闲流为空
    for (i = 0; i < NUM_STREAMS; i++) { 
		//将静态图像流对象加入到空闲图像流链表，便于alloc
        Stream *stream = &worker->streams_buf[i];
        ring_item_init(&stream->link); //流空闲
        red_free_stream(worker, stream); //加入空闲流链表
    }
}

/**
 * __red_is_next_stream_frame - 判断candidate是否为stream的下一帧
 * 这里流可能不存在，仅仅实在流跟踪时stream为NULL
**/
static inline int __red_is_next_stream_frame(RedWorker *worker,
	const Drawable *candidate,
	const int other_src_width, //对比帧源宽
	const int other_src_height, //对比帧源高
	const SpiceRect *other_dest, //对比帧目标区域
	const red_time_t other_time,
	const Stream *stream,
	int container_candidate_allowed) //是否允许包含
{
    RedDrawable *red_drawable;
    int is_frame_container = FALSE;

    if (!candidate->streamable) { //当前帧不可进入流
        return STREAM_FRAME_NONE;
    }

	// 当前帧跟对比时间之间超过限定值
    if (candidate->creation_time - other_time >
		(stream ? RED_STREAM_CONTINUS_MAX_DELTA : //1s
			RED_STREAM_DETACTION_MAX_DELTA)) // 200ms
	{
        return STREAM_FRAME_NONE;
    }

    red_drawable = candidate->red_drawable;
    if (!container_candidate_allowed) {//不允许包含
        SpiceRect* candidate_src;

		// 比较目标区域大小和坐标
        if (!rect_is_equal(&red_drawable->bbox, other_dest)) {
            return STREAM_FRAME_NONE;
        }

		// 比较源区域大小，不要求坐标相同，但大小必须相同
        candidate_src = &red_drawable->u.copy.src_area;
        if (candidate_src->right - candidate_src->left != other_src_width ||
            candidate_src->bottom - candidate_src->top != other_src_height) {
            return STREAM_FRAME_NONE;
        }
    } else {
    	// 候选帧目标区域包含对比目标区域
        if (rect_contains(&red_drawable->bbox, other_dest)) {
            int candidate_area = rect_get_area(&red_drawable->bbox);
            int other_area = rect_get_area(other_dest);
            /* do not stream drawables that are significantly
             * bigger than the original frame */
            if (candidate_area > 2 * other_area) { //超过两倍大小，返回NONE
                spice_debug("too big candidate:");
                spice_debug("prev box ==>");
                rect_debug(other_dest);
                spice_debug("new box ==>");
                rect_debug(&red_drawable->bbox);
                return STREAM_FRAME_NONE;
            }

            if (candidate_area > other_area) { //包含区域
                is_frame_container = TRUE;
            }
        } else { //非包含
            return STREAM_FRAME_NONE;
        }
    }

    if (stream) { // 如果是跟已有流比较还要比较topdown属性
        SpiceBitmap *bitmap = &red_drawable->u.copy.src_bitmap->u.bitmap;
        if (stream->top_down != !!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
            return STREAM_FRAME_NONE;
        }
    }
	// 返回此帧是下一帧，如果包含区域返回CONTAINER，否则NATIVE
    if (is_frame_container) { //
        return STREAM_FRAME_CONTAINER;
    } else {
        return STREAM_FRAME_NATIVE;
    }
}

static inline int red_is_next_stream_frame(RedWorker *worker, const Drawable *candidate,
                                           const Drawable *prev)
{
    if (!candidate->streamable) {
        return FALSE;
    }

    SpiceRect* prev_src = &prev->red_drawable->u.copy.src_area;
    return __red_is_next_stream_frame(worker, candidate, prev_src->right - prev_src->left,
                                      prev_src->bottom - prev_src->top,
                                      &prev->red_drawable->bbox, prev->creation_time,
                                      prev->stream,
                                      FALSE);
}

static inline void pre_stream_item_swap(RedWorker *worker, Stream *stream, Drawable *new_frame)
{
    DrawablePipeItem *dpi;
    DisplayChannelClient *dcc;
    int index;
    StreamAgent *agent;
    RingItem *ring_item, *next;

    spice_assert(stream->current);

    if (!display_is_connected(worker)) {
        return;
    }

    if (new_frame->process_commands_generation == stream->current->process_commands_generation) {
        spice_debug("ignoring drop, same process_commands_generation as previous frame");
        return;
    }

    index = get_stream_id(worker, stream);
    DRAWABLE_FOREACH_DPI_SAFE(stream->current, ring_item, next, dpi) {
        dcc = dpi->dcc;
        agent = &dcc->stream_agents[index];

        if (!dcc->use_mjpeg_encoder_rate_control &&
            !dcc->common.is_low_bandwidth) {
            continue;
        }

        if (pipe_item_is_linked(&dpi->dpi_pipe_item)) {
#ifdef STREAM_STATS
            agent->stats.num_drops_pipe++;
#endif
            if (dcc->use_mjpeg_encoder_rate_control) {
                mjpeg_encoder_notify_server_frame_drop(agent->mjpeg_encoder);
            } else {
                ++agent->drops;
            }
        }
    }


    WORKER_FOREACH_DCC_SAFE(worker, ring_item, next, dcc) {
        double drop_factor;

        agent = &dcc->stream_agents[index];

        if (dcc->use_mjpeg_encoder_rate_control) {
            continue;
        }
        if (agent->frames / agent->fps < FPS_TEST_INTERVAL) {
            agent->frames++;
            continue;
        }
        drop_factor = ((double)agent->frames - (double)agent->drops) /
            (double)agent->frames;
        spice_debug("stream %d: #frames %u #drops %u", index, agent->frames, agent->drops);
        if (drop_factor == 1) {
            if (agent->fps < MAX_FPS) {
                agent->fps++;
                spice_debug("stream %d: fps++ %u", index, agent->fps);
            }
        } else if (drop_factor < 0.9) {
            if (agent->fps > 1) {
                agent->fps--;
                spice_debug("stream %d: fps--%u", index, agent->fps);
            }
        }
        agent->frames = 1;
        agent->drops = 0;
    }
}

static inline void red_update_copy_graduality(RedWorker* worker, Drawable *drawable)
{
    SpiceBitmap *bitmap;
    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);

    if (worker->streaming_video != STREAM_VIDEO_FILTER) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_INVALID;
        return;
    }

    if (drawable->copy_bitmap_graduality != BITMAP_GRADUAL_INVALID) {
        return; // already set
    }

    bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;

    if (!BITMAP_FMT_HAS_GRADUALITY(bitmap->format) || _stride_is_extra(bitmap) ||
        (bitmap->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_NOT_AVAIL;
    } else  {
        drawable->copy_bitmap_graduality =
            _get_bitmap_graduality_level(worker, bitmap,drawable->group_id);
    }
}

static inline int red_is_stream_start(Drawable *drawable)
{
    return ((drawable->frames_count >= RED_STREAM_FRAMES_START_CONDITION) &&
            (drawable->gradual_frames_count >=
            (RED_STREAM_GRADUAL_FRAMES_START_CONDITION * drawable->frames_count)));
}

// returns whether a stream was created
static int red_stream_add_frame(RedWorker *worker,
                                Drawable *frame_drawable,
                                int frames_count,
                                int gradual_frames_count,
                                int last_gradual_frame)
{
    red_update_copy_graduality(worker, frame_drawable);
    frame_drawable->frames_count = frames_count + 1;
    frame_drawable->gradual_frames_count  = gradual_frames_count;

    if (frame_drawable->copy_bitmap_graduality != BITMAP_GRADUAL_LOW) {
        if ((frame_drawable->frames_count - last_gradual_frame) >
            RED_STREAM_FRAMES_RESET_CONDITION) {
            frame_drawable->frames_count = 1;
            frame_drawable->gradual_frames_count = 1;
        } else {
            frame_drawable->gradual_frames_count++;
        }

        frame_drawable->last_gradual_frame = frame_drawable->frames_count;
    } else {
        frame_drawable->last_gradual_frame = last_gradual_frame;
    }

    if (red_is_stream_start(frame_drawable)) {
        red_create_stream(worker, frame_drawable);
        return TRUE;
    }
    return FALSE;
}

static inline void red_stream_maintenance(RedWorker *worker, Drawable *candidate, Drawable *prev)
{
    Stream *stream;

	// 已经属于流则不处理
    if (candidate->stream) { //已经添加
        return;
    }

    if ((stream = prev->stream)) {//对比节点有流
        int is_next_frame = __red_is_next_stream_frame(worker,
                                                       candidate,
                                                       stream->width,
                                                       stream->height,
                                                       &stream->dest_area,
                                                       stream->last_time,
                                                       stream,
                                                       TRUE);
        if (is_next_frame != STREAM_FRAME_NONE) { //是对比节点流的下一帧
            pre_stream_item_swap(worker, stream, candidate);
            red_detach_stream(worker, stream, FALSE);
            prev->streamable = FALSE; //prevent item trace
            red_attach_stream(worker, candidate, stream);
            if (is_next_frame == STREAM_FRAME_CONTAINER) {
                candidate->sized_stream = stream;
            }
        }
    } else {
        if (red_is_next_stream_frame(worker, candidate, prev) != STREAM_FRAME_NONE) {
            red_stream_add_frame(worker, candidate,
                                 prev->frames_count,
                                 prev->gradual_frames_count,
                                 prev->last_gradual_frame);
        }
    }
}

// 判断渲染命令是否跟其他surface无关
// 为什么有渲染命令却会跟surface无关呢？
// 因为渲染的指令可能没有实际显示在画布上，如视频播放时最小会窗口
static inline int is_drawable_independent_from_surfaces(Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        if (drawable->surfaces_dest[x] != -1) { //
            return FALSE;
        }
    }
    return TRUE;
}

/* 将绘图命令树节点添加到命令树，绘图命令区域跟比较节点的区域完全一致
   item是待添加节点，other是命令树中比较节点，它跟item的目标区域完全一致
   成功返回TRUE，添加失败返回false
*/
static inline int red_current_add_equal(RedWorker *worker, DrawItem *item, TreeItem *other)
{
    DrawItem *other_draw_item;
    Drawable *drawable;
    Drawable *other_drawable;

	// 如果是CONTAINER，直接返回添加失败，只有是DRAWABLE类型才回做后续处理
    if (other->type != TREE_ITEM_TYPE_DRAWABLE) {
        return FALSE;
    }
    other_draw_item = (DrawItem *)other;//DrawItem的TreeItem是第一个成员，可直接强转

	//如果添加命令有shadow，
	//命令树节点有shadow
	//特效参数不一致
	//上面三种情况不进行后续操作
    if (item->shadow || other_draw_item->shadow || item->effect != other_draw_item->effect) {
        return FALSE;
    }

	//恢复Drawable 
    drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
    other_drawable = SPICE_CONTAINEROF(other_draw_item, Drawable, tree_item);

    if (item->effect == QXL_EFFECT_OPAQUE) { //视频流帧必走此流程
		//非透明渲染，则直接盖住了other_drawable, 要进行流识别

		// add_after，如果已经对比节点有流并且当前命令
		// 跟其他surface无关，应该是新的流帧
        int add_after = !!other_drawable->stream &&
        	is_drawable_independent_from_surfaces(drawable);

		// 流维护
        red_stream_maintenance(worker, drawable, other_drawable);
		// 将drawable的命令添加成other的兄弟节点，并添加到other之后
        __current_add_drawable(worker, drawable, &other->siblings_link);
		
		//other_drawable的引用计数为什么也加1，
		//因为current_remove_drawable从命令树中摘除，会减一个引用计数
		//这里先增加1，再调用current_remove_drawable，既可以干掉Drawable
		//的命令树关系，又不会释放掉Drawable对象，便于add_after再添加
        other_drawable->refs++;
		//从命令树中摘除，可能触发流跟踪
        current_remove_drawable(worker, other_drawable);
		
        if (add_after) {
            red_pipes_add_drawable_after(worker, drawable, other_drawable);
        } else {
            red_pipes_add_drawable(worker, drawable);
        }
        red_pipes_remove_drawable(other_drawable);
        release_drawable(worker, other_drawable);
        return TRUE;
    }

    switch (item->effect) {
    case QXL_EFFECT_REVERT_ON_DUP:
        if (is_same_drawable(worker, drawable, other_drawable)) {

            DisplayChannelClient *dcc;
            DrawablePipeItem *dpi;
            RingItem *worker_ring_item, *dpi_ring_item;

            other_drawable->refs++;
            current_remove_drawable(worker, other_drawable);

            /* sending the drawable to clients that already received
             * (or will receive) other_drawable */
            worker_ring_item = ring_get_head(&worker->display_channel->common.base.clients);
            dpi_ring_item = ring_get_head(&other_drawable->pipes);
            /* dpi contains a sublist of dcc's, ordered the same */
            while (worker_ring_item) {
                dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                        common.base.channel_link);
                dpi = SPICE_CONTAINEROF(dpi_ring_item, DrawablePipeItem, base);
                while (worker_ring_item && (!dpi || dcc != dpi->dcc)) {
                    red_pipe_add_drawable(dcc, drawable);
                    worker_ring_item = ring_next(&worker->display_channel->common.base.clients,
                                                 worker_ring_item);
                    dcc = SPICE_CONTAINEROF(worker_ring_item, DisplayChannelClient,
                                            common.base.channel_link);
                }

                if (dpi_ring_item) {
                    dpi_ring_item = ring_next(&other_drawable->pipes, dpi_ring_item);
                }
                if (worker_ring_item) {
                    worker_ring_item = ring_next(&worker->display_channel->common.base.clients,
                                                 worker_ring_item);
                }
            }
            /* not sending other_drawable where possible */
            red_pipes_remove_drawable(other_drawable);

            release_drawable(worker, other_drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_OPAQUE_BRUSH:
        if (is_same_geometry(worker, drawable, other_drawable)) {
            __current_add_drawable(worker, drawable, &other->siblings_link);
            remove_drawable(worker, other_drawable);
            red_pipes_add_drawable(worker, drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_NOP_ON_DUP:
        if (is_same_drawable(worker, drawable, other_drawable)) {
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static inline void red_use_stream_trace(RedWorker *worker, Drawable *drawable)
{
    ItemTrace *trace;
    ItemTrace *trace_end;
    Ring *ring;
    RingItem *item;

    if (drawable->stream || !drawable->streamable || drawable->frames_count) {
        return;
    }


    ring = &worker->streams;
    item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        int is_next_frame = __red_is_next_stream_frame(worker,
                                                       drawable,
                                                       stream->width,
                                                       stream->height,
                                                       &stream->dest_area,
                                                       stream->last_time,
                                                       stream,
                                                       TRUE);
        if (is_next_frame != STREAM_FRAME_NONE) {
            if (stream->current) {
                stream->current->streamable = FALSE; //prevent item trace
                pre_stream_item_swap(worker, stream, drawable);
                red_detach_stream(worker, stream, FALSE);
            }
            red_attach_stream(worker, drawable, stream);
            if (is_next_frame == STREAM_FRAME_CONTAINER) {
                drawable->sized_stream = stream;
            }
            return;
        }
        item = ring_next(ring, item);
    }

    trace = worker->items_trace;
    trace_end = trace + NUM_TRACE_ITEMS;
    for (; trace < trace_end; trace++) {
        if (__red_is_next_stream_frame(worker, drawable, trace->width, trace->height,
                                       &trace->dest_area, trace->time, NULL, FALSE) !=
                                       STREAM_FRAME_NONE) {
            if (red_stream_add_frame(worker, drawable,
                                     trace->frames_count,
                                     trace->gradual_frames_count,
                                     trace->last_gradual_frame)) {
                return;
            }
        }
    }
}

static void red_reset_stream_trace(RedWorker *worker)
{
    Ring *ring = &worker->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        Stream *stream = SPICE_CONTAINEROF(item, Stream, link);
        item = ring_next(ring, item);
        if (!stream->current) {
            red_stop_stream(worker, stream);
        } else {
            spice_info("attached stream");
        }
    }

    worker->next_item_trace = 0;
    memset(worker->items_trace, 0, sizeof(worker->items_trace));
}

/**
 * 将非copybits命令添加到当前surface的ring中
 * worker 当前显示通道RedWorker
 * ring RedSurface的current链表，ring指针在迭代过程中可能会变
 * drawable 绘图命令
**/
static inline int red_current_add(RedWorker *worker, Ring *ring, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item; //取出绘图命令的命令树构建，此指针不变
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif
    RingItem *now; //当前迭代的命令树节点
    QRegion exclude_rgn; //排除区域
    RingItem *exclude_base = NULL; //排除基础

    print_base_item("ADD", &item->base);
    spice_assert(!region_is_empty(&item->base.rgn));
    region_init(&exclude_rgn); //初始化排除区域
    now = ring_next(ring, ring); //从current链表开始迭代
	//迭代命令树
    while (now) {
		//兄弟节点
        TreeItem *sibling = SPICE_CONTAINEROF(now, TreeItem, siblings_link);
        int test_res;

		// 如果当前drawable的目标区域与跟比较节点的目标区域的外围矩形不相交，
		// 那不用继续比较了，直接比较链表中下一个节点
        if (!region_bounds_intersects(&item->base.rgn, &sibling->rgn)) {
            print_base_item("EMPTY", sibling);
            now = ring_next(ring, now);
            continue;
        }
		// 外围矩形相交的情况下，将drawable的区域跟比较节点进行详细测试，
		// item是左区域，sibling是右区域
        test_res = region_test(&item->base.rgn, &sibling->rgn, REGION_TEST_ALL);
		// 如果区域没有相交，继续比较后续节点
        if (!(test_res & REGION_TEST_SHARED)) {
            print_base_item("EMPTY", sibling);
            now = ring_next(ring, now);
            continue;
		// 以下代码表示item和比较节点区域相交
        } else if (sibling->type != TREE_ITEM_TYPE_SHADOW) {
        //如果比较节点不是shadow节点，是container或者drawitem节点

			// 区域完全相等时test_res只设置了shared标记，且没有左排除和右排除标记。
			// 调用red_current_add_equal尝试走等区域流程，成功直接返回，失败继续走下面的流程
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) &&
				!(test_res & REGION_TEST_LEFT_EXCLUSIVE) &&
				red_current_add_equal(worker, item, sibling)) //sibling是container时返回false
			{
                stat_add(&worker->add_stat, start_time);
                return FALSE;
            }

			// sibling没有完全包含item区域，而且item是非透明绘图命令
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) && item->effect == QXL_EFFECT_OPAQUE) {
                Shadow *shadow;
                int skip = now == exclude_base; //now == exclude_base是否相同
                print_base_item("CONTAIN", sibling);

                if ((shadow = __find_shadow(sibling))) { //sibling的最老子孙有shadow
                    if (exclude_base) {
                        TreeItem *next = sibling;
                        exclude_region(worker, ring, exclude_base, &exclude_rgn, &next, NULL);
                        if (next != sibling) {
                            now = next ? &next->siblings_link : NULL;
                            exclude_base = NULL;
                            continue;
                        }
                    }
                    region_or(&exclude_rgn, &shadow->on_hold);
                }
                now = now->prev; //往前找
                current_remove(worker, sibling); //将sibling及其子孙从命令树移除
                now = ring_next(ring, now);
                if (shadow || skip) {
                    exclude_base = now;
                }
                continue;
            }

            if (!(test_res & REGION_TEST_LEFT_EXCLUSIVE) && is_opaque_item(sibling)) {
                Container *container;

                if (exclude_base) {
                    exclude_region(worker, ring, exclude_base, &exclude_rgn, NULL, NULL);
                    region_clear(&exclude_rgn);
                    exclude_base = NULL;
                }
                print_base_item("IN", sibling);
                if (sibling->type == TREE_ITEM_TYPE_CONTAINER) {
                    container = (Container *)sibling;
                    ring = &container->items;
                    item->base.container = container;
                    now = ring_next(ring, ring);
                    continue;
                }
                spice_assert(IS_DRAW_ITEM(sibling));
                if (!((DrawItem *)sibling)->container_root) {//如果不是容器的根节点（第一个节点）
                	// 以sibling绘图节点创建一个容器节点
                    container = __new_container(worker, (DrawItem *)sibling); 
                    if (!container) { //创建失败，从代码来看不会失败，无效代码
                        spice_warning("create new container failed");
                        region_destroy(&exclude_rgn);
                        return FALSE;
                    }
                    item->base.container = container;
                    ring = &container->items; //
                }
            }
        }
        if (!exclude_base) {
            exclude_base = now;
        }
        break;
    }
    if (item->effect == QXL_EFFECT_OPAQUE) { //当前是非透明命令
        region_or(&exclude_rgn, &item->base.rgn);
        exclude_region(worker, ring, exclude_base, &exclude_rgn, NULL, drawable);
        red_use_stream_trace(worker, drawable);
        red_streams_update_visible_region(worker, drawable);
        /*
         * Performing the insertion after exclude_region for
         * safety (todo: Not sure if exclude_region can affect the drawable
         * if it is added to the tree before calling exclude_region).
         */
        __current_add_drawable(worker, drawable, ring);
    } else {
        /*
         * red_detach_streams_behind can affect the current tree since it may
         * trigger calls to update_area. Thus, the drawable should be added to the tree
         * before calling red_detach_streams_behind
         */
        __current_add_drawable(worker, drawable, ring);
        if (drawable->surface_id == 0) {
            red_detach_streams_behind(worker, &drawable->tree_item.base.rgn, drawable);
        }
    }
    region_destroy(&exclude_rgn);
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

static void add_clip_rects(QRegion *rgn, SpiceClipRects *data)
{
    int i;

    for (i = 0; i < data->num_rects; i++) {
        region_add(rgn, data->rects + i);
    }
}

//创建一个命令树shadow，创建者是item，delta是copybits命令便宜量，源坐标-目的坐标
static inline Shadow *__new_shadow(RedWorker *worker, Drawable *item, SpicePoint *delta)
{
    if (!delta->x && !delta->y) { 
		/* copybits的引用没有偏移，则copybits没有含义，不用处理该copybits命令 */
        return NULL;
    }

    Shadow *shadow = spice_new(Shadow, 1);
    worker->shadows_count++; //命令树shadows节点计数自增
#ifdef PIPE_DEBUG
    shadow->base.id = ++worker->last_id;
#endif
    shadow->base.type = TREE_ITEM_TYPE_SHADOW;
    shadow->base.container = NULL; //shadow默认没有container
    shadow->owner = &item->tree_item; //触发shadow生成的copybits命令树节点
    //设置copybits引用的区域，注意shadow的rgn中存的是被引用的区域，而不是目标区域
    //目标区域在copybits中
    region_clone(&shadow->base.rgn, &item->tree_item.base.rgn); //shadow的rgn为copybits的rgn
    region_offset(&shadow->base.rgn, delta->x, delta->y);//将rgn进行偏移，得到引用的区域
    ring_item_init(&shadow->base.siblings_link);//还没加入命令树
    region_init(&shadow->on_hold); //初始化on_hold区域
    item->tree_item.shadow = shadow; //设置copybits的shadow
    //创建时shadow没有直接加到树中
    return shadow;
}

/* 将copybits命令item添加到surface的ring命令树，
 * delta是copybits的图像偏移量, 是copybits的src坐标-bbox的坐上顶点坐标
 */
static inline int red_current_add_with_shadow(RedWorker *worker, Ring *ring, Drawable *item,
                                              SpicePoint *delta)
{
#ifdef RED_WORKER_STAT
    stat_time_t start_time = stat_now();
#endif

	//先创建shadow节点
    Shadow *shadow = __new_shadow(worker, item, delta);
    if (!shadow) { //copybits没偏移则不用添加命令树，返回添加命令树失败
        stat_add(&worker->add_stat, start_time);
        return FALSE;
    }
    print_base_item("ADDSHADOW", &item->tree_item.base);
    /* item and his shadow must initially be placed in the same container.
   	   for now putting them on root.
   	   drawable和相应的shadow开始时是放在同一个容器里，先暂时放在root里
    */

    // only primary surface streams are supported
    if (is_primary_surface(worker, item->surface_id)) {
		/* 如果是主surface的copybits，需要将引用区域的 */
        red_detach_streams_behind(worker, &shadow->base.rgn, NULL);
    }
	//直接将shadow直接挂到命令式最前面
    ring_add(ring, &shadow->base.siblings_link);
    __current_add_drawable(worker, item, ring);
	// 只有非透明命令要更新显示区域
    if (item->tree_item.effect == QXL_EFFECT_OPAQUE) {
        QRegion exclude_rgn;
        region_clone(&exclude_rgn, &item->tree_item.base.rgn);
        exclude_region(worker, ring, &shadow->base.siblings_link, &exclude_rgn, NULL, NULL);
        region_destroy(&exclude_rgn);
        red_streams_update_visible_region(worker, item);
    } else { //透明引用
        if (item->surface_id == 0) {
            red_detach_streams_behind(worker, &item->tree_item.base.rgn, item);
        }
    }
    stat_add(&worker->add_stat, start_time);
    return TRUE;
}

static inline int has_shadow(RedDrawable *drawable)
{
    return drawable->type == QXL_COPY_BITS;
}

static inline void red_update_streamable(RedWorker *worker, Drawable *drawable,
                                         RedDrawable *red_drawable)
{
    SpiceImage *image;

    if (worker->streaming_video == STREAM_VIDEO_OFF) {
        return;
    }

    if (!is_primary_surface(worker, drawable->surface_id)) {
        return;
    }
	// 必须是DRAWCOPY命令，非透明效果，ROP必须是PUT
    if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE ||
		red_drawable->type != QXL_DRAW_COPY ||
		red_drawable->u.copy.rop_descriptor != SPICE_ROPD_OP_PUT) {
        return;
    }

	// 必须有图像，且图像的类型是BITMAP
    image = red_drawable->u.copy.src_bitmap;
    if (image == NULL ||
        image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return;
    }

    if (worker->streaming_video == STREAM_VIDEO_FILTER) {
        SpiceRect* rect;
        int size;

        rect = &drawable->red_drawable->u.copy.src_area;
        size = (rect->right - rect->left) * (rect->bottom - rect->top);
        if (size < RED_STREAM_MIN_SIZE) {
            return;
        }
    }

    drawable->streamable = TRUE;
}

// 将Drawable添加到命令树，drawable和red_drawable相关
static inline int red_current_add_qxl(RedWorker *worker, Ring *ring, Drawable *drawable,
                                      RedDrawable *red_drawable)
{
    int ret;
	
    if (has_shadow(red_drawable)) {//如果是copybits命令
        SpicePoint delta;

#ifdef RED_WORKER_STAT
        ++worker->add_with_shadow_count;
#endif
		//copybits是同surface的图像引用，所以先得到矩形区域的偏移
        delta.x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
        delta.y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;
		//添加带shadows的命令
        ret = red_current_add_with_shadow(worker, ring, drawable, &delta);
    } else { //非COPYBITS命令
		//先更新视频流
        red_update_streamable(worker, drawable, red_drawable);
        ret = red_current_add(worker, ring, drawable);
    }
#ifdef RED_WORKER_STAT
    if ((++worker->add_count % 100) == 0) {
        stat_time_t total = worker->add_stat.total;
        spice_info("add with shadow count %u",
                   worker->add_with_shadow_count);
        worker->add_with_shadow_count = 0;
        spice_info("add[%u] %f exclude[%u] %f __exclude[%u] %f",
                   worker->add_stat.count,
                   stat_cpu_time_to_sec(total),
                   worker->exclude_stat.count,
                   stat_cpu_time_to_sec(worker->exclude_stat.total),
                   worker->__exclude_stat.count,
                   stat_cpu_time_to_sec(worker->__exclude_stat.total));
        spice_info("add %f%% exclude %f%% exclude2 %f%% __exclude %f%%",
                   (double)(total - worker->exclude_stat.total) / total * 100,
                   (double)(worker->exclude_stat.total) / total * 100,
                   (double)(worker->exclude_stat.total -
                            worker->__exclude_stat.total) / worker->exclude_stat.total * 100,
                   (double)(worker->__exclude_stat.total) / worker->exclude_stat.total * 100);
        stat_reset(&worker->add_stat);
        stat_reset(&worker->exclude_stat);
        stat_reset(&worker->__exclude_stat);
    }
#endif
    return ret;
}

static void red_get_area(RedWorker *worker, int surface_id, const SpiceRect *area, uint8_t *dest,
                         int dest_stride, int update)
{
    SpiceCanvas *canvas;
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];
    if (update) {
        red_update_area(worker, area, surface_id);
    }

    canvas = surface->context.canvas;
    canvas->ops->read_bits(canvas, dest, dest_stride, area);
}

/** 判断RGB32图像数据里是否里是否有alpha通道
 *  如果有一个像素的alpha通道数据不为0，则返回有alpha通道，否则没有
 *  在有alpha通道的情况下，只要有一个alpha通道的值不为0xff，设置all_set_out为FALSE
 *  只有所有alpha通道的值都为0xff，all_set_out为真
**/
static int rgb32_data_has_alpha(int width, int height, size_t stride,
                                uint8_t *data, int *all_set_out)
{
    uint32_t *line, *end, alpha;
    int has_alpha;

    has_alpha = FALSE;
    while (height-- > 0) {
        line = (uint32_t *)data;
        end = line + width;
        data += stride;
        while (line != end) {
            alpha = *line & 0xff000000U;
            if (alpha != 0) {
                has_alpha = TRUE;
                if (alpha != 0xff000000U) {
                    *all_set_out = FALSE;
                    return TRUE;
                }
            }
            line++;
        }
    }

    *all_set_out = has_alpha;
    return has_alpha;
}

/* 如果有self_bitmap图像，
   将显示命令里的self_bitmap部分图像 */
static inline int red_handle_self_bitmap(RedWorker *worker, Drawable *drawable)
{
    SpiceImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    RedSurface *surface;
    int bpp;
    int all_set;
    RedDrawable *red_drawable = drawable->red_drawable;

    if (!red_drawable->self_bitmap) { //如果没有self_bitmap图像，直接返回
        return TRUE;
    }

    surface = &worker->surfaces[drawable->surface_id];

    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;

    width = red_drawable->self_bitmap_area.right
            - red_drawable->self_bitmap_area.left;
    height = red_drawable->self_bitmap_area.bottom
            - red_drawable->self_bitmap_area.top;
    dest_stride = SPICE_ALIGN(width * bpp, 4);

    image = spice_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    image->u.bitmap.flags = surface->context.top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    image->u.bitmap.format = spice_bitmap_from_surface_type(surface->context.format);
    image->u.bitmap.stride = dest_stride;
    image->descriptor.width = image->u.bitmap.x = width;
    image->descriptor.height = image->u.bitmap.y = height;
    image->u.bitmap.palette = NULL;

    dest = (uint8_t *)spice_malloc_n(height, dest_stride);
    image->u.bitmap.data = spice_chunks_new_linear(dest, height * dest_stride);
    image->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_FREE;

    red_get_area(worker, drawable->surface_id,
                 &red_drawable->self_bitmap_area, dest, dest_stride, TRUE);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(worker, drawable->surface_id) &&
        image->u.bitmap.format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(width, height, dest_stride, dest, &all_set)) {
        if (all_set) {
            image->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            image->u.bitmap.format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    red_drawable->self_bitmap_image = image;
    return TRUE;
}

// 释放一个drawable
static void free_one_drawable(RedWorker *worker, int force_glz_free)
{
	// 从末尾取一个ring_time
    RingItem *ring_item = ring_get_tail(&worker->current_list);
    Drawable *drawable;
    Container *container;

    spice_assert(ring_item);
	//恢复出命令
    drawable = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
    if (force_glz_free) {
        RingItem *glz_item, *next_item;
        RedGlzDrawable *glz;
        DRAWABLE_FOREACH_GLZ_SAFE(drawable, glz_item, next_item, glz) {
			//释放RedGlzDrawable, RedGlzDrawable跟DCC一一对应
            red_display_free_glz_drawable(glz->dcc, glz);
        }
    }
	// 将drawable渲染到画布
    red_draw_drawable(worker, drawable);
	// container
    container = drawable->tree_item.base.container;
	// 从container中清楚
    current_remove_drawable(worker, drawable);
	// 更新container关系
    container_cleanup(worker, container);
}

static Drawable *get_drawable(RedWorker *worker, uint8_t effect, RedDrawable *red_drawable,
                              uint32_t group_id)
{
    Drawable *drawable;
    struct timespec time;
    int x;

    while (!(drawable = alloc_drawable(worker))) {
        free_one_drawable(worker, FALSE);
    }
    worker->drawable_count++;
    worker->red_drawable_count++;
    memset(drawable, 0, sizeof(Drawable));
    drawable->refs = 1; //初始引用计数
    clock_gettime(CLOCK_MONOTONIC, &time);
    drawable->creation_time = timespec_to_red_time(&time);
#ifdef PIPE_DEBUG
    drawable->tree_item.base.id = ++worker->last_id;
#endif
    ring_item_init(&drawable->list_link);
    ring_item_init(&drawable->surface_list_link);
#ifdef UPDATE_AREA_BY_TREE
    ring_item_init(&drawable->collect_link);
#endif
    ring_item_init(&drawable->tree_item.base.siblings_link);
    drawable->tree_item.base.type = TREE_ITEM_TYPE_DRAWABLE;
    region_init(&drawable->tree_item.base.rgn);
    drawable->tree_item.effect = effect;
    drawable->red_drawable = ref_red_drawable(red_drawable);
    drawable->group_id = group_id;

    drawable->surface_id = red_drawable->surface_id;
    VALIDATE_SURFACE_RETVAL(worker, drawable->surface_id, NULL)
    for (x = 0; x < 3; ++x) {
        drawable->surfaces_dest[x] = red_drawable->surfaces_dest[x];
        if (drawable->surfaces_dest[x] != -1) {
            VALIDATE_SURFACE_RETVAL(worker, drawable->surfaces_dest[x], NULL)
        }
    }
    ring_init(&drawable->pipes);
    ring_init(&drawable->glz_ring);

    drawable->process_commands_generation = worker->process_commands_generation;
    return drawable;
}

/*  */ 
static inline int 
red_handle_depends_on_target_surface(RedWorker *worker, uint32_t surface_id)
{
    RedSurface *surface;
    RingItem *ring_item;

    surface = &worker->surfaces[surface_id];

	// 将依赖在本surface的绘图命令渲染掉，从后开始渲染，因为最后的命令最老
    while ((ring_item = ring_get_tail(&surface->depend_on_me))) {
        Drawable *drawable;
        DependItem *depended_item = SPICE_CONTAINEROF(ring_item, DependItem, ring_item);
		// 找回当前依赖本surface的绘图命令列表
        drawable = depended_item->drawable;
		// 刷新依赖命令surface的绘图命令区域
        surface_flush(worker, drawable->surface_id, &drawable->red_drawable->bbox);
    }

    return TRUE;
}

/** 添加surface依赖关系
 * depend_on_surface_id，drawable依赖哪个surface
 * depend_item, 被操作的依赖关系表示实体
 * drawable， 依赖surface的drawable对象
**/
static inline void add_to_surface_dependency(RedWorker *worker, 
	int depend_on_surface_id, DependItem *depend_item, Drawable *drawable)
{
    RedSurface *surface;

	// 依赖surface的id为-1表示不存在依赖关系，依赖关系的源drawable设置为空
    if (depend_on_surface_id == -1) {
        depend_item->drawable = NULL;
        return;
    }

	//获取依赖的redsurface
    surface = &worker->surfaces[depend_on_surface_id];

	// 使用DependItem来表示一个drawable依赖一个surface，DependItem会被连接到
	// redsurface的depend_on_me的链表中，也就是说，可以通过depend_on_me列表
	// 找到所有依赖这个surface的drawable对象
    depend_item->drawable = drawable;
	//新的依赖关系添加的depend_on_me的表头，也就是说，depend_on_me链表中
	//是从新到老的顺序记录依赖关系的。
    ring_add(&surface->depend_on_me, &depend_item->ring_item);
}

// 处理surfaces的依赖关系
static inline int 
red_handle_surfaces_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        // surface self dependency is handled by shadows in "current", or by
        // handle_self_bitmap，surface内部的依赖关系，由shadows或者self_bitmap
        // 来处理，这里的依赖关系，指的是不同surface之间的依赖关系。
        if (drawable->surfaces_dest[x] != drawable->surface_id) {
			// 如果依赖surface的为-1，只会置空drawable依赖关系数组
            add_to_surface_dependency(worker, drawable->surfaces_dest[x],
                                      &drawable->depend_items[x], drawable);
			//如果依赖于主surface，需要处理流相关问题
            if (drawable->surfaces_dest[x] == 0) {
                QRegion depend_region;
                region_init(&depend_region);
                region_add(&depend_region, &drawable->red_drawable->surfaces_rects[x]);
                red_detach_streams_behind(worker, &depend_region, NULL);
            }
        }
    }

    return TRUE;
}

// 增加drawable对依赖surface的redsurface的引用计数
static inline void 
red_inc_surfaces_drawable_dependencies(RedWorker *worker, Drawable *drawable)
{
    int x;
    int surface_id;
    RedSurface *surface;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surfaces_dest[x];
        if (surface_id == -1) {
            continue;
        }
        surface = &worker->surfaces[surface_id];
        surface->refs++;
    }
}


static inline void red_process_drawable(RedWorker *worker, RedDrawable *red_drawable,
                                        uint32_t group_id)
{
    int surface_id;
    Drawable *drawable = get_drawable(worker, red_drawable->effect, red_drawable, group_id);

    if (!drawable) {
        rendering_incorrect("failed to get_drawable");
        return;
    }

    surface_id = drawable->surface_id;

    worker->surfaces[surface_id].refs++;

	// 目标区域赋值到rgn中，如果有裁剪，下面进行裁剪
    region_add(&drawable->tree_item.base.rgn, &red_drawable->bbox);
#ifdef PIPE_DEBUG
    printf("TEST: DRAWABLE: id %u type %s effect %u bbox %u %u %u %u\n",
           drawable->tree_item.base.id,
           draw_type_to_str(red_drawable->type),
           drawable->tree_item.effect,
           red_drawable->bbox.top, red_drawable->bbox.left,
           red_drawable->bbox.bottom, red_drawable->bbox.right);
#endif

	// 设置drawable的drawitem的裁剪区域
    if (red_drawable->clip.type == SPICE_CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        add_clip_rects(&rgn, red_drawable->clip.rects);
        region_and(&drawable->tree_item.base.rgn, &rgn); //对目标区域进行裁剪
        region_destroy(&rgn);
    }
    /*
        surface->refs is affected by a drawable (that is
        dependent on the surface) as long as the drawable is alive.
        However, surface->depend_on_me is affected by a drawable only
        as long as it is in the current tree (hasn't been rendered yet).
        surface->refs和surface->depend_on_me都描述了其他surface的drawable对
        本surface的依赖关系，但两个关系的生命周期不一样，只要drawable存在，则
        refs不会递减，depend_on_me则和current树相关，只要还在树种（还未被渲染）
        就会有depend_on_me。
    */
    red_inc_surfaces_drawable_dependencies(worker, drawable);

    if (region_is_empty(&drawable->tree_item.base.rgn)) {
		//裁剪后目标区域为空，不用处理，直接释放绘图命令
        goto cleanup;
    }

    if (!red_handle_self_bitmap(worker, drawable)) {//返回值一直是TRUE
        goto cleanup;
    }

	// 将依赖surface_id的绘图命令的目标surface的目标区域刷新一下
    if (!red_handle_depends_on_target_surface(worker, surface_id)) {
        goto cleanup;
    }

	// 设置drawable的依赖关系
    if (!red_handle_surfaces_dependencies(worker, drawable)) {
        goto cleanup;
    }

	// 将drawable添加到red_surface的current命令树
    if (red_current_add_qxl(worker, &worker->surfaces[surface_id].current, 
			drawable, red_drawable))
    {
        if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE) {
            worker->transparent_count++; //添加命令树成功，对透明命令进行计数
        }
		//将drawable添加到命令管道
        red_pipes_add_drawable(worker, drawable);
#ifdef DRAW_ALL
        red_draw_qxl_drawable(worker, drawable);
#endif
    }
cleanup:
    release_drawable(worker, drawable);
}

static inline void red_create_surface(RedWorker *worker, uint32_t surface_id,uint32_t width,
                                      uint32_t height, int32_t stride, uint32_t format,
                                      void *line_0, int data_is_valid, int send_client);

/**
 * red_process_surface - 处理QXL画布命令
 * worker 对应RedWorker
 * surface Surface命令
 * loadvm 是否正在加载vm时调用
**/
static inline void red_process_surface(RedWorker *worker, 
	RedSurfaceCmd *surface,
	uint32_t group_id, int loadvm)
{
    int surface_id;
    RedSurface *red_surface;
    uint8_t *data;

    surface_id = surface->surface_id;
	//surface_id校验
    __validate_surface(worker, surface_id);

	//获得RedSurface对象
    red_surface = &worker->surfaces[surface_id]; 

    switch (surface->type) {
    case QXL_SURFACE_CMD_CREATE: { //创建命令
        uint32_t height = surface->u.surface_create.height;
        int32_t stride = surface->u.surface_create.stride;
		//是否重现加载surface
        int reloaded_surface = loadvm || (surface->flags & QXL_SURF_FLAG_KEEP_DATA);

		//数据地址计算，如果stride< 0，那么数据的起始地址前移
        data = surface->u.surface_create.data;
        if (stride < 0) {
            data -= (int32_t)(stride * (height - 1));
        }
        red_create_surface(worker, surface_id, 
        	surface->u.surface_create.width,  height, stride, 
        	surface->u.surface_create.format, data, 
        	reloaded_surface, !reloaded_surface);
        set_surface_release_info(worker, surface_id, 1, surface->release_info, group_id);
        break;
    }
    case QXL_SURFACE_CMD_DESTROY:
        spice_warn_if(!red_surface->context.canvas);
        set_surface_release_info(worker, surface_id, 0, surface->release_info, group_id);
        red_handle_depends_on_target_surface(worker, surface_id);
        /* note that red_handle_depends_on_target_surface must be called before red_current_clear.
           otherwise "current" will hold items that other drawables may depend on, and then
           red_current_clear will remove them from the pipe. */
        red_current_clear(worker, surface_id);
        red_clear_surface_drawables_from_pipes(worker, surface_id, FALSE);
        red_destroy_surface(worker, surface_id);
        break;
    default:
            spice_error("unknown surface command");
    };
    red_put_surface_cmd(surface);
    free(surface);
}

static SpiceCanvas *image_surfaces_get(SpiceImageSurfaces *surfaces,
                                       uint32_t surface_id)
{
    RedWorker *worker;

    worker = SPICE_CONTAINEROF(surfaces, RedWorker, image_surfaces);
    VALIDATE_SURFACE_RETVAL(worker, surface_id, NULL);

    return worker->surfaces[surface_id].context.canvas;
}

static void image_surface_init(RedWorker *worker)
{
    static SpiceImageSurfacesOps image_surfaces_ops = {
        image_surfaces_get,
    };

    worker->image_surfaces.ops = &image_surfaces_ops;
}

// 将bitmap spice图像本地化
static void localize_bitmap(RedWorker *worker, SpiceImage **image_ptr,
	SpiceImage *image_store, Drawable *drawable)
{
    SpiceImage *image = *image_ptr;

    if (image == NULL) { //图像为空，图像指针指向self_bitmap_image指向的图像
        spice_assert(drawable != NULL);
        spice_assert(drawable->red_drawable->self_bitmap_image != NULL);
        *image_ptr = drawable->red_drawable->self_bitmap_image;
        return;
    }

	// 图像缓存查找
    if (image_cache_hit(&worker->image_cache, image->descriptor.id)) {
        image_store->descriptor = image->descriptor;
        image_store->descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
        image_store->descriptor.flags = 0;
        *image_ptr = image_store;
        return;
    }

	// 驱动只会发下来三类图像：QUIC、BITMAP、SURFACE，其他类型是spice处理过程
	// 中创建的
    switch (image->descriptor.type) {
    case SPICE_IMAGE_TYPE_QUIC: {
        image_store->descriptor = image->descriptor;
        image_store->u.quic = image->u.quic;
        *image_ptr = image_store;
#ifdef IMAGE_CACHE_AGE
        image_store->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
#else
        if (image_store->descriptor.width * image->descriptor.height >= 640 * 480) {
            image_store->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
        }
#endif
        break;
    }
    case SPICE_IMAGE_TYPE_BITMAP: //BITMAP、SURFACE图像没有做处理
    case SPICE_IMAGE_TYPE_SURFACE:
        /* nothing */
        break;
    default:
        spice_error("invalid image type");
    }
}

//画刷本地化
static void localize_brush(RedWorker *worker, SpiceBrush *brush, SpiceImage *image_store)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        localize_bitmap(worker, &brush->u.pattern.pat, image_store, NULL);
    }
}

//蒙版本地化
static void localize_mask(RedWorker *worker, SpiceQMask *mask, SpiceImage *image_store)
{
    if (mask->bitmap) {
        localize_bitmap(worker, &mask->bitmap, image_store, NULL);
    }
}

//讲drawable对应的red_drawable渲染到画布上
static void red_draw_qxl_drawable(RedWorker *worker, Drawable *drawable)
{
    RedSurface *surface;
    SpiceCanvas *canvas;
    SpiceClip clip = drawable->red_drawable->clip; //裁剪区域

    surface = &worker->surfaces[drawable->surface_id]; //目标surface
    canvas = surface->context.canvas; //surface的画布

    image_cache_aging(&worker->image_cache); //执行缓存老化

    worker->preload_group_id = drawable->group_id; //更新预加载组id

	// 将当前命令的目标区域增加到surface的脏区域
    region_add(&surface->draw_dirty_region, &drawable->red_drawable->bbox);

    switch (drawable->red_drawable->type) {
    case QXL_DRAW_FILL: {
        SpiceFill fill = drawable->red_drawable->u.fill;
        SpiceImage img1, img2;
        localize_brush(worker, &fill.brush, &img1);
        localize_mask(worker, &fill.mask, &img2);
        canvas->ops->draw_fill(canvas, &drawable->red_drawable->bbox,
                               &clip, &fill);
        break;
    }
    case QXL_DRAW_OPAQUE: {
        SpiceOpaque opaque = drawable->red_drawable->u.opaque;
        SpiceImage img1, img2, img3;
        localize_brush(worker, &opaque.brush, &img1);
        localize_bitmap(worker, &opaque.src_bitmap, &img2, drawable);
        localize_mask(worker, &opaque.mask, &img3);
        canvas->ops->draw_opaque(canvas, &drawable->red_drawable->bbox, &clip, &opaque);
        break;
    }
    case QXL_DRAW_COPY: {
        SpiceCopy copy = drawable->red_drawable->u.copy;
        SpiceImage img1, img2;
        localize_bitmap(worker, &copy.src_bitmap, &img1, drawable);
        localize_mask(worker, &copy.mask, &img2);
        canvas->ops->draw_copy(canvas, &drawable->red_drawable->bbox,
                               &clip, &copy);
        break;
    }
    case QXL_DRAW_TRANSPARENT: {
        SpiceTransparent transparent = drawable->red_drawable->u.transparent;
        SpiceImage img1;
        localize_bitmap(worker, &transparent.src_bitmap, &img1, drawable);
        canvas->ops->draw_transparent(canvas,
                                      &drawable->red_drawable->bbox, &clip, &transparent);
        break;
    }
    case QXL_DRAW_ALPHA_BLEND: {
        SpiceAlphaBlend alpha_blend = drawable->red_drawable->u.alpha_blend;
        SpiceImage img1;
        localize_bitmap(worker, &alpha_blend.src_bitmap, &img1, drawable);
        canvas->ops->draw_alpha_blend(canvas,
                                      &drawable->red_drawable->bbox, &clip, &alpha_blend);
        break;
    }
    case QXL_COPY_BITS: {
        canvas->ops->copy_bits(canvas, &drawable->red_drawable->bbox,
                               &clip, &drawable->red_drawable->u.copy_bits.src_pos);
        break;
    }
    case QXL_DRAW_BLEND: {
        SpiceBlend blend = drawable->red_drawable->u.blend;
        SpiceImage img1, img2;
        localize_bitmap(worker, &blend.src_bitmap, &img1, drawable);
        localize_mask(worker, &blend.mask, &img2);
        canvas->ops->draw_blend(canvas, &drawable->red_drawable->bbox,
                                &clip, &blend);
        break;
    }
    case QXL_DRAW_BLACKNESS: {
        SpiceBlackness blackness = drawable->red_drawable->u.blackness;
        SpiceImage img1;
        localize_mask(worker, &blackness.mask, &img1);
        canvas->ops->draw_blackness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &blackness);
        break;
    }
    case QXL_DRAW_WHITENESS: {
        SpiceWhiteness whiteness = drawable->red_drawable->u.whiteness;
        SpiceImage img1;
        localize_mask(worker, &whiteness.mask, &img1);
        canvas->ops->draw_whiteness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &whiteness);
        break;
    }
    case QXL_DRAW_INVERS: {
        SpiceInvers invers = drawable->red_drawable->u.invers;
        SpiceImage img1;
        localize_mask(worker, &invers.mask, &img1);
        canvas->ops->draw_invers(canvas,
                                 &drawable->red_drawable->bbox, &clip, &invers);
        break;
    }
    case QXL_DRAW_ROP3: {
        SpiceRop3 rop3 = drawable->red_drawable->u.rop3;
        SpiceImage img1, img2, img3;
        localize_brush(worker, &rop3.brush, &img1);
        localize_bitmap(worker, &rop3.src_bitmap, &img2, drawable);
        localize_mask(worker, &rop3.mask, &img3);
        canvas->ops->draw_rop3(canvas, &drawable->red_drawable->bbox,
                               &clip, &rop3);
        break;
    }
    case QXL_DRAW_COMPOSITE: {
        SpiceComposite composite = drawable->red_drawable->u.composite;
        SpiceImage src, mask;
        localize_bitmap(worker, &composite.src_bitmap, &src, drawable);
        if (composite.mask_bitmap)
            localize_bitmap(worker, &composite.mask_bitmap, &mask, drawable);
        canvas->ops->draw_composite(canvas, &drawable->red_drawable->bbox,
                                    &clip, &composite);
        break;
    }
    case QXL_DRAW_STROKE: {
        SpiceStroke stroke = drawable->red_drawable->u.stroke;
        SpiceImage img1;
        localize_brush(worker, &stroke.brush, &img1);
        canvas->ops->draw_stroke(canvas,
                                 &drawable->red_drawable->bbox, &clip, &stroke);
        break;
    }
    case QXL_DRAW_TEXT: {
        SpiceText text = drawable->red_drawable->u.text;
        SpiceImage img1, img2;
        localize_brush(worker, &text.fore_brush, &img1);
        localize_brush(worker, &text.back_brush, &img2);
        canvas->ops->draw_text(canvas, &drawable->red_drawable->bbox,
                               &clip, &text);
        break;
    }
    default:
        spice_warning("invalid type");
    }
}

#ifndef DRAW_ALL //没有定义Draw_ALL，所以此处代码移植有效

// 渲染一个drawable对象，会触发渲染qxl_drawable对象
static void red_draw_drawable(RedWorker *worker, Drawable *drawable)
{
#ifdef UPDATE_AREA_BY_TREE
    SpiceCanvas *canvas;

    canvas = surface->context.canvas;
    //todo: add need top mask flag
    canvas->ops->group_start(canvas,
                             &drawable->tree_item.base.rgn);
#endif
	//先刷依赖画布的图像
    red_flush_source_surfaces(worker, drawable);
	//将drawable渲染到画布
    red_draw_qxl_drawable(worker, drawable);
#ifdef UPDATE_AREA_BY_TREE
    canvas->ops->group_end(canvas);
#endif
}

static void validate_area(RedWorker *worker, const SpiceRect *area, uint32_t surface_id)
{
    RedSurface *surface;

    surface = &worker->surfaces[surface_id];
    if (!surface->context.canvas_draws_on_surface) {//SW画布不走此流程
        SpiceCanvas *canvas = surface->context.canvas;
        int h;
        int stride = surface->context.stride;
        uint8_t *line_0 = surface->context.line_0;

        if (!(h = area->bottom - area->top)) { //高度为0，直接返回
            return;
        }

        spice_assert(stride < 0);
        uint8_t *dest = line_0 + (area->top * stride) + area->left * sizeof(uint32_t);
        dest += (h - 1) * stride;
        canvas->ops->read_bits(canvas, dest, -stride, area);
    }
}

#ifdef UPDATE_AREA_BY_TREE //没定义

static inline void __red_collect_for_update(RedWorker *worker, Ring *ring, RingItem *ring_item,
                                            QRegion *rgn, Ring *items)
{
    Ring *top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;
        if (region_intersects(rgn, &now->rgn)) {
            if (IS_DRAW_ITEM(now)) {
                Drawable *drawable = SPICE_CONTAINEROF(now, Drawable, tree_item);

                ring_add(items, &drawable->collect_link);
                region_or(rgn, &now->rgn);
                if (drawable->tree_item.shadow) {
                    region_or(rgn, &drawable->tree_item.shadow->base.rgn);
                }
            } else if (now->type == TREE_ITEM_TYPE_SHADOW) {
                Drawable *owner = SPICE_CONTAINEROF(((Shadow *)now)->owner, Drawable, tree_item);
                if (!ring_item_is_linked(&owner->collect_link)) {
                    region_or(rgn, &now->rgn);
                    region_or(rgn, &owner->tree_item.base.rgn);
                    ring_add(items, &owner->collect_link);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                Container *container = (Container *)now;

                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    spice_assert(((TreeItem *)ring_item)->container);
                    continue;
                }
                ring_item = &now->siblings_link;
            }
        }

        while (!(ring_item = ring_next(ring, ring_item))) {
            if (ring == top_ring) {
                return;
            }
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Ring *ring;
    RingItem *ring_item;
    Ring items;
    QRegion rgn;

    surface = &worker->surfaces[surface_id];
    ring = &surface->current;

    if (!(ring_item = ring_get_head(ring))) {
        worker->draw_context.validate_area(surface->context.canvas,
                                           &worker->dev_info.surface0_area, area);
        return;
    }

    region_init(&rgn);
    region_add(&rgn, area);
    ring_init(&items);
    __red_collect_for_update(worker, ring, ring_item, &rgn, &items);
    region_destroy(&rgn);

    while ((ring_item = ring_get_head(&items))) {
        Drawable *drawable = SPICE_CONTAINEROF(ring_item, Drawable, collect_link);
        Container *container;

        ring_remove(ring_item);
        red_draw_drawable(worker, drawable);
        container = drawable->tree_item.base.container;
        current_remove_drawable(worker, drawable);
        container_cleanup(worker, container);
    }
    validate_area(worker, area, surface_id);
}

#else

/*
	渲染指定区域的图形命令，但是只有那些last之前的命令会被渲染
    Renders drawables for updating the requested area, but only drawables that are older
    than 'last' (exclusive).
*/
static void red_update_area_till(RedWorker *worker, const SpiceRect *area, int surface_id,
                                 Drawable *last)
{
    // TODO: if we use UPDATE_AREA_BY_TREE, a corresponding red_update_area_till
    // should be implemented

    RedSurface *surface;
    Drawable *surface_last = NULL;
    Ring *ring;
    RingItem *ring_item;
    Drawable *now;
    QRegion rgn;

    spice_assert(last);
    spice_assert(ring_item_is_linked(&last->list_link));

    surface = &worker->surfaces[surface_id];

	// 找出surface_id里比last老一点的drawable，存放在surface_last里
    if (surface_id != last->surface_id) { //surface id不一致
        // find the nearest older drawable from the appropriate surface
        // 从最近的surface里找surface_id里最接近last的drawable
        ring = &worker->current_list;
        ring_item = &last->list_link;
		//新的drawable总是添加在current链表的头部，
		//从last的位置开始往后找就能找到与surface相同的
		//比last老一点的drawable
		while ((ring_item = ring_next(ring, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
            if (now->surface_id == surface_id) {
                surface_last = now;
                break;
            }
        }
    } else { //surface_id跟last同一个surface，则直接取last的surface链表的后一个节点
        ring_item = ring_next(&surface->current_list, &last->surface_list_link);
        if (ring_item) {
            surface_last = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        }
    }
	
    if (!surface_last) { //可能后面都没节点了，则直接返回，不用更新区域
        return;
    }

	//找到了
    ring = &surface->current_list; //surface的渲染命令链表
    ring_item = &surface_last->surface_list_link; //当前位置

    region_init(&rgn);
    region_add(&rgn, area);

    // find the first older drawable that intersects with the area
    // 继续往后找surface_last，这次找到其目标区域跟参数区域相交的命令
    do {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            surface_last = now;
            break;
        }
    } while ((ring_item = ring_next(ring, ring_item)));

    region_destroy(&rgn);

	// 如果没有相交的命令，也不用更新
    if (!surface_last) {
        return;
    }

	// surface_last是drawable命令之后，surface_id层里最新的一个现实绘图命令
	// surface->current_list从后开始进行渲染
    do {
        Container *container;
		//从后开始渲染，所以总是取链表的尾部节点
        ring_item = ring_get_tail(&surface->current_list);
		//恢复出尾部drawable存放在now中
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
		//先加一次now的引用计数，防止now在current_remove_drawable时被释放掉
        now->refs++;
		//获取now的container
        container = now->tree_item.base.container;
		//命令树种摘除drawable
        current_remove_drawable(worker, now);
		//尝试清理DrawItem的container，防止出现container没有孩子，或者只有一个孩子的情况。
        container_cleanup(worker, container);
        /* red_draw_drawable may call red_update_area for the surfaces 'now' depends on. Notice,
           that it is valid to call red_update_area in this case and not red_update_area_till:
           It is impossible that there was newer item then 'last' in one of the surfaces
           that red_update_area is called for, Otherwise, 'now' would have already been rendered.
           See the call for red_handle_depends_on_target_surface in red_process_drawable */
        //将now绘图命令渲染掉
        red_draw_drawable(worker, now);
		//释放Drawable
        release_drawable(worker, now);
    } while (now != surface_last);
    validate_area(worker, area, surface_id);
}

// 刷新制定surface的指定矩形区域，但是实际刷新的区域可能超出area的区域。
// 假设surface中的命令A跟area相交，那么比A命令都老的surface绘图命令都会被渲染刷新
static void red_update_area(RedWorker *worker, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Ring *ring;
    RingItem *ring_item;
    QRegion rgn;
    Drawable *last;
    Drawable *now;
#ifdef ACYCLIC_SURFACE_DEBUG
    int gn;
#endif
    spice_debug("surface %d: area ==>", surface_id);
    rect_debug(area);

	// 参数校验
    spice_return_if_fail(surface_id >= 0 && surface_id < NUM_SURFACES);
    spice_return_if_fail(area);
    spice_return_if_fail(area->left >= 0 && area->top >= 0 &&
                         area->left < area->right && area->top < area->bottom);

    surface = &worker->surfaces[surface_id];

	// 最后一个drawable先设置为空
    last = NULL;
#ifdef ACYCLIC_SURFACE_DEBUG
    gn = ++surface->current_gn;
#endif
    ring = &surface->current_list;
    ring_item = ring;

	// 找到最新的一个跟参数矩形相交的命令
    region_init(&rgn);
    region_add(&rgn, area);
    while ((ring_item = ring_next(ring, ring_item))) {
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            last = now;
            break;
        }
    }
    region_destroy(&rgn);

	// 如果surface里没有命令跟制定区域相交，不用渲染命令，直接转换一下缓冲区就好了
    if (!last) {
		//画布里的数据转到缓冲区里
        validate_area(worker, area, surface_id);
        return;
    }

	// 渲染比最新相交命令都老的绘图命令到画布
    do {
        Container *container;

		//从后往前刷，也就是按照时间顺序进行刷新
        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
		//current_remove_drawable会释放drawable的引用计数，所以先加引用防止
		//drawable被释放掉。
        now->refs++;
        container = now->tree_item.base.container;
		//从命令树，surface，redworker中移除drawable，因为从surface中移除了
		//关系，所以这个循环中不要对ring_item进行操作了
        current_remove_drawable(worker, now);
		//对父container进行清理
        container_cleanup(worker, container);
		//将当前命令渲染到画布
        red_draw_drawable(worker, now);
		//释放drawable对象
        release_drawable(worker, now);
#ifdef ACYCLIC_SURFACE_DEBUG
        if (gn != surface->current_gn) {
            spice_error("cyclic surface dependencies");
        }
#endif
    } while (now != last);
	//转换画布缓冲区，SW画布什么都没做
    validate_area(worker, area, surface_id);
}

#endif
#endif

static inline void free_cursor_item(RedWorker *worker, CursorItem *item);

static void red_release_cursor(RedWorker *worker, CursorItem *cursor)
{
    if (!--cursor->refs) {
        QXLReleaseInfoExt release_info_ext;
        RedCursorCmd *cursor_cmd;

        cursor_cmd = cursor->red_cursor;
        release_info_ext.group_id = cursor->group_id;
        release_info_ext.info = cursor_cmd->release_info;
        worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
        free_cursor_item(worker, cursor);
        red_put_cursor_cmd(cursor_cmd);
        free(cursor_cmd);
    }
}

static void red_set_cursor(RedWorker *worker, CursorItem *cursor)
{
    if (worker->cursor) {
        red_release_cursor(worker, worker->cursor);
    }
    ++cursor->refs;
    worker->cursor = cursor;
}

#ifdef DEBUG_CURSORS
static int _cursor_count = 0;
#endif

static inline CursorItem *alloc_cursor_item(RedWorker *worker)
{
    CursorItem *cursor;

    if (!worker->free_cursor_items) {
        return NULL;
    }
#ifdef DEBUG_CURSORS
    --_cursor_count;
#endif
    cursor = &worker->free_cursor_items->u.cursor_item;
    worker->free_cursor_items = worker->free_cursor_items->u.next;
    return cursor;
}

static inline void free_cursor_item(RedWorker *worker, CursorItem *item)
{
    ((_CursorItem *)item)->u.next = worker->free_cursor_items;
    worker->free_cursor_items = (_CursorItem *)item;
#ifdef DEBUG_CURSORS
    ++_cursor_count;
    spice_assert(_cursor_count <= NUM_CURSORS);
#endif
}

static void cursor_items_init(RedWorker *worker)
{
    int i;

    worker->free_cursor_items = NULL;
    for (i = 0; i < NUM_CURSORS; i++) {
        free_cursor_item(worker, &worker->cursor_items[i].u.cursor_item);
    }
}

static CursorItem *get_cursor_item(RedWorker *worker, RedCursorCmd *cmd, uint32_t group_id)
{
    CursorItem *cursor_item;

    spice_warn_if(!(cursor_item = alloc_cursor_item(worker)));

    cursor_item->refs = 1;
    cursor_item->group_id = group_id;
    cursor_item->red_cursor = cmd;

    return cursor_item;
}

static CursorPipeItem *ref_cursor_pipe_item(CursorPipeItem *item)
{
    spice_assert(item);
    item->refs++;
    return item;
}

static PipeItem *new_cursor_pipe_item(RedChannelClient *rcc, void *data, int num)
{
    CursorPipeItem *item = spice_malloc0(sizeof(CursorPipeItem));

    red_channel_pipe_item_init(rcc->channel, &item->base, PIPE_ITEM_TYPE_CURSOR);
    item->refs = 1;
    item->cursor_item = data;
    item->cursor_item->refs++;
    return &item->base;
}

static void put_cursor_pipe_item(CursorChannelClient *ccc, CursorPipeItem *pipe_item)
{
    spice_assert(pipe_item);

    if (--pipe_item->refs) {
        return;
    }

    spice_assert(!pipe_item_is_linked(&pipe_item->base));

    red_release_cursor(ccc->common.worker, pipe_item->cursor_item);
    free(pipe_item);
}

static void qxl_process_cursor(RedWorker *worker, RedCursorCmd *cursor_cmd, uint32_t group_id)
{
    CursorItem *cursor_item;
    int cursor_show = FALSE;

    cursor_item = get_cursor_item(worker, cursor_cmd, group_id);

    switch (cursor_cmd->type) {
    case QXL_CURSOR_SET:
        worker->cursor_visible = cursor_cmd->u.set.visible;
        red_set_cursor(worker, cursor_item);
        break;
    case QXL_CURSOR_MOVE:
        cursor_show = !worker->cursor_visible;
        worker->cursor_visible = TRUE;
        worker->cursor_position = cursor_cmd->u.position;
        break;
    case QXL_CURSOR_HIDE:
        worker->cursor_visible = FALSE;
        break;
    case QXL_CURSOR_TRAIL:
        worker->cursor_trail_length = cursor_cmd->u.trail.length;
        worker->cursor_trail_frequency = cursor_cmd->u.trail.frequency;
        break;
    default:
        spice_error("invalid cursor command %u", cursor_cmd->type);
    }

    if (cursor_is_connected(worker) && (worker->mouse_mode == SPICE_MOUSE_MODE_SERVER ||
                                   cursor_cmd->type != QXL_CURSOR_MOVE || cursor_show)) {
        red_channel_pipes_new_add(&worker->cursor_channel->common.base, new_cursor_pipe_item,
                                  (void*)cursor_item);
    }
    red_release_cursor(worker, cursor_item);
}

static int red_process_cursor(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }

    *ring_is_empty = FALSE;
    while (!cursor_is_connected(worker) ||
           red_channel_min_pipe_size(&worker->cursor_channel->common.base) <= max_pipe_size) {
        if (!worker->qxl->st->qif->get_cursor_command(worker->qxl, &ext_cmd)) {
            *ring_is_empty = TRUE;
            if (worker->repoll_cursor_ring < CMD_RING_POLL_RETRIES) {
                worker->repoll_cursor_ring++;
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cursor_ring > CMD_RING_POLL_RETRIES ||
                worker->qxl->st->qif->req_cursor_notification(worker->qxl)) {
                worker->repoll_cursor_ring++;
                break;
            }
            continue;
        }
        worker->repoll_cursor_ring = 0;
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_CURSOR: {
            RedCursorCmd *cursor = spice_new0(RedCursorCmd, 1);

            if (red_get_cursor_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    cursor, ext_cmd.cmd.data)) {
                free(cursor);
                break;
            }

            qxl_process_cursor(worker, cursor, ext_cmd.group_id);
            break;
        }
        default:
            spice_error("bad command type");
        }
        n++;
    }
    return n;
}

static RedDrawable *red_drawable_new(void)
{
    RedDrawable * red = spice_new0(RedDrawable, 1);

    red->refs = 1;
    return red;
}

/* 处理显示命令
   max_pipe_size 是个恒定值MAX_PIPE_SIZE=50
*/
static int red_process_commands(RedWorker *worker, uint32_t max_pipe_size, int *ring_is_empty)
{
    QXLCommandExt ext_cmd;
    int n = 0;
    uint64_t start = red_now();

    if (!worker->running) {
        *ring_is_empty = TRUE;
        return n;
    }
	//每进一次+1
    worker->process_commands_generation++;
    *ring_is_empty = FALSE;//默认不空

	// 这个条件为什么是显示通道没连接 或者管道最小命令<=50
    while (!display_is_connected(worker) || //没有客户端连接，也需要处理显示命令
           // TODO: change to average pipe size? 
           red_channel_min_pipe_size(&worker->display_channel->common.base) <= max_pipe_size) 
    {
        if (!worker->qxl->st->qif->get_command(worker->qxl, &ext_cmd)) {//取命令失败
            *ring_is_empty = TRUE;//置空
            if (worker->repoll_cmd_ring < CMD_RING_POLL_RETRIES) {//需要重试，退出循环
                worker->repoll_cmd_ring++;
                worker->event_timeout = MIN(worker->event_timeout, CMD_RING_POLL_TIMEOUT);
                break;
            }
            if (worker->repoll_cmd_ring > CMD_RING_POLL_RETRIES ||
                         worker->qxl->st->qif->req_cmd_notification(worker->qxl)) {
                worker->repoll_cmd_ring++;
                break;
            }
            continue;
        }
		//取命令成功
        stat_inc_counter(worker->command_counter, 1); //增加命令计数
        worker->repoll_cmd_ring = 0;//重置repoll_cmd_ring计数

		
        switch (ext_cmd.cmd.type) {
        case QXL_CMD_DRAW: {//DRAW命令
        	//分配RedDrawable
            RedDrawable *red_drawable = red_drawable_new(); // returns with 1 ref

            if (!red_get_drawable(&worker->mem_slots, ext_cmd.group_id,
				red_drawable, ext_cmd.cmd.data, ext_cmd.flags)) 
			{//从显卡共享内存中解析数据填充RedDrawable
                red_process_drawable(worker, red_drawable, ext_cmd.group_id);
            }
            // release the red_drawable
            put_red_drawable(worker, red_drawable, ext_cmd.group_id);
            break;
        }
        case QXL_CMD_UPDATE: {
            RedUpdateCmd update;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_update_cmd(&worker->mem_slots, ext_cmd.group_id,
                                   &update, ext_cmd.cmd.data)) {
                break;
            }
            if (!validate_surface(worker, update.surface_id)) {
                rendering_incorrect("QXL_CMD_UPDATE");
                break;
            }
            red_update_area(worker, &update.area, update.surface_id);
            worker->qxl->st->qif->notify_update(worker->qxl, update.update_id);
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = update.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_update_cmd(&update);
            break;
        }
        case QXL_CMD_MESSAGE: {
            RedMessage message;
            QXLReleaseInfoExt release_info_ext;

            if (red_get_message(&worker->mem_slots, ext_cmd.group_id,
                                &message, ext_cmd.cmd.data)) {
                break;
            }
#ifdef DEBUG
            /* alert: accessing message.data is insecure */
            spice_warning("MESSAGE: %s", message.data);
#endif
            release_info_ext.group_id = ext_cmd.group_id;
            release_info_ext.info = message.release_info;
            worker->qxl->st->qif->release_resource(worker->qxl, release_info_ext);
            red_put_message(&message);
            break;
        }
        case QXL_CMD_SURFACE: {
            RedSurfaceCmd *surface = spice_new0(RedSurfaceCmd, 1);

            if (red_get_surface_cmd(&worker->mem_slots, ext_cmd.group_id,
                                    surface, ext_cmd.cmd.data)) {
                free(surface);
                break;
            }
            red_process_surface(worker, surface, ext_cmd.group_id, FALSE);
            break;
        }
        default:
            spice_error("bad command type");
        }
        n++;
        if ((worker->display_channel &&
             red_channel_all_blocked(&worker->display_channel->common.base))
            || red_now() - start > 10 * 1000 * 1000) {
            worker->event_timeout = 0;
            return n;
        }
    }
    return n;
}

#define RED_RELEASE_BUNCH_SIZE 64

static void red_free_some(RedWorker *worker)
{
    int n = 0;
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    spice_debug("#draw=%d, #red_draw=%d, #glz_draw=%d", worker->drawable_count,
                worker->red_drawable_count, worker->glz_drawable_count);
    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            // encoding using the dictionary is prevented since the following operations might
            // change the dictionary
            pthread_rwlock_wrlock(&glz_dict->encode_lock);
            n = red_display_free_some_independent_glz_drawables(dcc);
        }
    }

    while (!ring_is_empty(&worker->current_list) && n++ < RED_RELEASE_BUNCH_SIZE) {
        free_one_drawable(worker, TRUE);
    }

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

        if (glz_dict) {
            pthread_rwlock_unlock(&glz_dict->encode_lock);
        }
    }
}

// 将指定surface的命令树刷到画布上
static void red_current_flush(RedWorker *worker, int surface_id)
{
    while (!ring_is_empty(&worker->surfaces[surface_id].current_list)) {
        free_one_drawable(worker, FALSE);
    }
	//清理current
    red_current_clear(worker, surface_id);
}

// adding the pipe item after pos. If pos == NULL, adding to head.
// dcc 加入dcc的管道
// surface_id 不传指针，传id，结合RedWorker找到RedSurface对象
// area 显示区域
// pos 命令添加位置，NULL表示添加到管道的头部
// can_lossy surface内容是否可以压缩
static ImageItem *red_add_surface_area_image(
	DisplayChannelClient *dcc, int surface_id,
	SpiceRect *area, PipeItem *pos, int can_lossy)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedChannel *channel = &display_channel->common.base;
    RedSurface *surface = &worker->surfaces[surface_id];
    SpiceCanvas *canvas = surface->context.canvas;
    ImageItem *item;
    int stride;
    int width;
    int height;
    int bpp;
    int all_set;

    spice_assert(area);

    width = area->right - area->left; //宽
    height = area->bottom - area->top; //高
    //得到单像素字节数,format的低6位表示单像素位数
    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;
	//计算行长
    stride = width * bpp;

	//分配ImageItem和图像内存
    item = (ImageItem *)spice_malloc_n_m(height, stride, sizeof(ImageItem));

	//图像类型
    red_channel_pipe_item_init(channel, &item->link, PIPE_ITEM_TYPE_IMAGE);

    item->refs = 1;
    item->surface_id = surface_id; //图像画到哪个Surface
    item->image_format =
        spice_bitmap_from_surface_type(surface->context.format);
    item->image_flags = 0;
    item->pos.x = area->left;
    item->pos.y = area->top;
    item->width = width;
    item->height = height;
    item->stride = stride;
    item->top_down = surface->context.top_down;
    item->can_lossy = can_lossy;

    canvas->ops->read_bits(canvas, item->data, stride, area);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(worker, surface_id) && //不是主surface
        item->image_format == SPICE_BITMAP_FMT_32BIT && //RGB32格式
        rgb32_data_has_alpha(item->width, item->height, 
        	item->stride, item->data, &all_set))//有alpha通道
    {
        if (all_set) {
            item->image_flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            item->image_format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    if (!pos) {
        red_pipe_add_image_item(dcc, item);
    } else {
        red_pipe_add_image_item_after(dcc, item, pos);
    }

    release_image_item(item);

    return item;
}

// 将制定surface画布里的图像推送到ddc的发送管道里
static void red_push_surface_image(DisplayChannelClient *dcc, int surface_id)
{
    SpiceRect area;
    RedSurface *surface;
    RedWorker *worker;

    if (!dcc) {
        return;
    }
    worker = DCC_TO_WORKER(dcc); //恢复RedWorker
    surface = &worker->surfaces[surface_id]; //获得surface
    if (!surface->context.canvas) { //画布没构建不能压入SURFACE图像命令
        return;
    }
	//设置矩形区域
    area.top = area.left = 0; //surface的图像从没有偏移
    area.right = surface->context.width;
    area.bottom = surface->context.height;

    /* not allowing lossy compression because probably, 
       especially if it is a primary surface,
       it combines both "picture-like" areas with areas that are more "artificial"
       POS == NULL, add to head
    */
    red_add_surface_area_image(dcc, surface_id, &area, NULL, FALSE);
    red_channel_client_push(&dcc->common.base);
}

typedef struct {
    uint32_t type;
    void *data;
    uint32_t size;
} AddBufInfo;

static void marshaller_add_compressed(SpiceMarshaller *m,
                                      RedCompressBuf *comp_buf, size_t size)
{
    size_t max = size;
    size_t now;
    do {
        spice_assert(comp_buf);
        now = MIN(sizeof(comp_buf->buf), max);
        max -= now;
        spice_marshaller_add_ref(m, (uint8_t*)comp_buf->buf, now);
        comp_buf = comp_buf->send_next;
    } while (max);
}


static void add_buf_from_info(SpiceMarshaller *m, AddBufInfo *info)
{
    if (info->data) {
        switch (info->type) {
        case BUF_TYPE_RAW:
            spice_marshaller_add_ref(m, info->data, info->size);
            break;
        }
    }
}

// 将Drawable里的bbox目标区域，目标surface，和裁剪区域
static void fill_base(SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    SpiceMsgDisplayBase base;

    base.surface_id = drawable->surface_id;
    base.box = drawable->red_drawable->bbox;
    base.clip = drawable->red_drawable->clip;

    spice_marshall_DisplayBase(base_marshaller, &base);
}

static inline void fill_palette(DisplayChannelClient *dcc,
                                SpicePalette *palette,
                                uint8_t *flags)
{
    if (palette == NULL) {
        return;
    }
    if (palette->unique) {
        if (red_palette_cache_find(dcc, palette->unique)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_FROM_CACHE;
            return;
        }
        if (red_palette_cache_add(dcc, palette->unique, 1)) {
            *flags |= SPICE_BITMAP_FLAGS_PAL_CACHE_ME;
        }
    }
}

static inline RedCompressBuf *red_display_alloc_compress_buf(DisplayChannelClient *dcc)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedCompressBuf *ret;

    if (display_channel->free_compress_bufs) {
        ret = display_channel->free_compress_bufs;
        display_channel->free_compress_bufs = ret->next;
    } else {
        ret = spice_new(RedCompressBuf, 1);
    }

    ret->next = dcc->send_data.used_compress_bufs;
    dcc->send_data.used_compress_bufs = ret;
    return ret;
}

static inline void __red_display_free_compress_buf(DisplayChannel *dc,
                                                   RedCompressBuf *buf)
{
    buf->next = dc->free_compress_bufs;
    dc->free_compress_bufs = buf;
}

static void red_display_free_compress_buf(DisplayChannelClient *dcc,
                                          RedCompressBuf *buf)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedCompressBuf **curr_used = &dcc->send_data.used_compress_bufs;

    for (;;) {
        spice_assert(*curr_used);
        if (*curr_used == buf) {
            *curr_used = buf->next;
            break;
        }
        curr_used = &(*curr_used)->next;
    }
    __red_display_free_compress_buf(display_channel, buf);
}

static void red_display_reset_compress_buf(DisplayChannelClient *dcc)
{
    while (dcc->send_data.used_compress_bufs) {
        RedCompressBuf *buf = dcc->send_data.used_compress_bufs;
        dcc->send_data.used_compress_bufs = buf->next;
        __red_display_free_compress_buf(DCC_TO_DC(dcc), buf);
    }
}

static void red_display_destroy_compress_bufs(DisplayChannel *display_channel)
{
    spice_assert(!red_channel_is_connected(&display_channel->common.base));
    while (display_channel->free_compress_bufs) {
        RedCompressBuf *buf = display_channel->free_compress_bufs;
        display_channel->free_compress_bufs = buf->next;
        free(buf);
    }
}

/******************************************************
 *      Global lz red drawables routines
*******************************************************/

/* if already exists, returns it. Otherwise allocates and adds it (1) to the ring tail
   in the channel (2) to the Drawable*/
static RedGlzDrawable *red_display_get_glz_drawable(DisplayChannelClient *dcc, Drawable *drawable)
{
    RedGlzDrawable *ret;
    RingItem *item, *next;

    // TODO - I don't really understand what's going on here, so doing the technical equivalent
    // now that we have multiple glz_dicts, so the only way to go from dcc to drawable glz is to go
    // over the glz_ring (unless adding some better data structure then a ring)
    DRAWABLE_FOREACH_GLZ_SAFE(drawable, item, next, ret) {
        if (ret->dcc == dcc) {
            return ret;
        }
    }

    ret = spice_new(RedGlzDrawable, 1);

    ret->dcc = dcc;
    ret->red_drawable = ref_red_drawable(drawable->red_drawable);
    ret->drawable = drawable;
    ret->group_id = drawable->group_id;
    ret->instances_count = 0;
    ring_init(&ret->instances);

    ring_item_init(&ret->link);
    ring_item_init(&ret->drawable_link);
    ring_add_before(&ret->link, &dcc->glz_drawables);
    ring_add(&drawable->glz_ring, &ret->drawable_link);
    dcc->common.worker->glz_drawable_count++;
    return ret;
}

/* allocates new instance and adds it to instances in the given drawable.
   NOTE - the caller should set the glz_instance returned by the encoder by itself.*/
static GlzDrawableInstanceItem *red_display_add_glz_drawable_instance(RedGlzDrawable *glz_drawable)
{
    spice_assert(glz_drawable->instances_count < MAX_GLZ_DRAWABLE_INSTANCES);
    // NOTE: We assume the additions are performed consecutively, without removals in the middle
    GlzDrawableInstanceItem *ret = glz_drawable->instances_pool + glz_drawable->instances_count;
    glz_drawable->instances_count++;

    ring_item_init(&ret->free_link);
    ring_item_init(&ret->glz_link);
    ring_add(&glz_drawable->instances, &ret->glz_link);
    ret->glz_instance = NULL;
    ret->red_glz_drawable = glz_drawable;

    return ret;
}

/* Remove from the to_free list and the instances_list.
   When no instance is left - the RedGlzDrawable is released too. (and the qxl drawable too, if
   it is not used by Drawable).
   NOTE - 1) can be called only by the display channel that created the drawable
          2) it is assumed that the instance was already removed from the dictionary*/
static void red_display_free_glz_drawable_instance(DisplayChannelClient *dcc,
                                                   GlzDrawableInstanceItem *glz_drawable_instance)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedGlzDrawable *glz_drawable;

    spice_assert(glz_drawable_instance);
    spice_assert(glz_drawable_instance->red_glz_drawable);

    glz_drawable = glz_drawable_instance->red_glz_drawable;

    spice_assert(glz_drawable->dcc == dcc);
    spice_assert(glz_drawable->instances_count);

    ring_remove(&glz_drawable_instance->glz_link);
    glz_drawable->instances_count--;
    // when the remove callback is performed from the channel that the
    // drawable belongs to, the instance is not added to the 'to_free' list
    if (ring_item_is_linked(&glz_drawable_instance->free_link)) {
        ring_remove(&glz_drawable_instance->free_link);
    }

    if (ring_is_empty(&glz_drawable->instances)) {
        spice_assert(!glz_drawable->instances_count);

        Drawable *drawable = glz_drawable->drawable;

        if (drawable) {
            ring_remove(&glz_drawable->drawable_link);
        }
        put_red_drawable(worker, glz_drawable->red_drawable,
                         glz_drawable->group_id);
        worker->glz_drawable_count--;
        if (ring_item_is_linked(&glz_drawable->link)) {
            ring_remove(&glz_drawable->link);
        }
        free(glz_drawable);
    }
}

static void red_display_handle_glz_drawables_to_free(DisplayChannelClient* dcc)
{
    RingItem *ring_link;

    if (!dcc->glz_dict) {
        return;
    }
    pthread_mutex_lock(&dcc->glz_drawables_inst_to_free_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables_inst_to_free))) {
        GlzDrawableInstanceItem *drawable_instance = SPICE_CONTAINEROF(ring_link,
                                                                 GlzDrawableInstanceItem,
                                                                 free_link);
        red_display_free_glz_drawable_instance(dcc, drawable_instance);
    }
    pthread_mutex_unlock(&dcc->glz_drawables_inst_to_free_lock);
}

/*
 * Releases all the instances of the drawable from the dictionary and the display channel client.
 * The release of the last instance will also release the drawable itself and the qxl drawable
 * if possible.
 * NOTE - the caller should prevent encoding using the dictionary during this operation
 */
static void red_display_free_glz_drawable(DisplayChannelClient *dcc, RedGlzDrawable *drawable)
{
    RingItem *head_instance = ring_get_head(&drawable->instances);
    int cont = (head_instance != NULL);

    while (cont) {
        if (drawable->instances_count == 1) {
            /* Last instance: red_display_free_glz_drawable_instance will free the drawable */
            cont = FALSE;
        }
        GlzDrawableInstanceItem *instance = SPICE_CONTAINEROF(head_instance,
                                                        GlzDrawableInstanceItem,
                                                        glz_link);
        if (!ring_item_is_linked(&instance->free_link)) {
            // the instance didn't get out from window yet
            glz_enc_dictionary_remove_image(dcc->glz_dict->dict,
                                            instance->glz_instance,
                                            &dcc->glz_data.usr);
        }
        red_display_free_glz_drawable_instance(dcc, instance);

        if (cont) {
            head_instance = ring_get_head(&drawable->instances);
        }
    }
}

/* Clear all lz drawables - enforce their removal from the global dictionary.
   NOTE - prevents encoding using the dictionary during the operation*/
static void red_display_client_clear_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

    if (!glz_dict) {
        return;
    }

    // assure no display channel is during global lz encoding
    pthread_rwlock_wrlock(&glz_dict->encode_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables))) {
        RedGlzDrawable *drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        // no need to lock the to_free list, since we assured no other thread is encoding and
        // thus not other thread access the to_free list of the channel
        red_display_free_glz_drawable(dcc, drawable);
    }
    pthread_rwlock_unlock(&glz_dict->encode_lock);
}

static void red_display_clear_glz_drawables(DisplayChannel *display_channel)
{
    RingItem *link, *next;
    DisplayChannelClient *dcc;

    if (!display_channel) {
        return;
    }
    DCC_FOREACH_SAFE(link, next, dcc, &display_channel->common.base) {
        red_display_client_clear_glz_drawables(dcc);
    }
}

/*
 * Remove from the global lz dictionary some glz_drawables that have no reference to
 * Drawable (their qxl drawables are released too).
 * NOTE - the caller should prevent encoding using the dictionary during the operation
 */
static int red_display_free_some_independent_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    int n = 0;

    if (!dcc) {
        return 0;
    }
    ring_link = ring_get_head(&dcc->glz_drawables);
    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link = ring_next(&dcc->glz_drawables, ring_link);
        if (!glz_drawable->drawable) {
            red_display_free_glz_drawable(dcc, glz_drawable);
            n++;
        }
    }
    return n;
}

/******************************************************
 *              Encoders callbacks
*******************************************************/
static SPICE_GNUC_NORETURN SPICE_GNUC_PRINTF(2, 3) void
quic_usr_error(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_NORETURN SPICE_GNUC_PRINTF(2, 3) void
lz_usr_error(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_PRINTF(2, 3) void glz_usr_error(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);

    spice_critical("%s", usr_data->message_buf); // if global lz fails in the middle
                                        // the consequences are not predictable since the window
                                        // can turn to be unsynchronized between the server and
                                        // and the client
}

static SPICE_GNUC_PRINTF(2, 3) void quic_usr_warn(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void lz_usr_warn(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void glz_usr_warn(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static void *quic_usr_malloc(QuicUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *lz_usr_malloc(LzUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *glz_usr_malloc(GlzEncoderUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void quic_usr_free(QuicUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void lz_usr_free(LzUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void glz_usr_free(GlzEncoderUsrContext *usr, void *ptr)
{
    free(ptr);
}

static inline int encoder_usr_more_space(EncoderData *enc_data, uint32_t **io_ptr)
{
    RedCompressBuf *buf;

    if (!(buf = red_display_alloc_compress_buf(enc_data->dcc))) {
        return 0;
    }
    enc_data->bufs_tail->send_next = buf;
    enc_data->bufs_tail = buf;
    buf->send_next = NULL;
    *io_ptr = buf->buf;
    return sizeof(buf->buf) >> 2;
}

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int lz_usr_more_space(LzUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int glz_usr_more_space(GlzEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int jpeg_usr_more_space(JpegEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static int zlib_usr_more_space(ZlibEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    return (encoder_usr_more_space(usr_data, (uint32_t **)io_ptr) << 2);
}

static inline int encoder_usr_more_lines(EncoderData *enc_data, uint8_t **lines)
{
    struct SpiceChunk *chunk;

    if (enc_data->u.lines_data.reverse) {
        if (!(enc_data->u.lines_data.next >= 0)) {
            return 0;
        }
    } else {
        if (!(enc_data->u.lines_data.next < enc_data->u.lines_data.chunks->num_chunks)) {
            return 0;
        }
    }

    chunk = &enc_data->u.lines_data.chunks->chunk[enc_data->u.lines_data.next];
    if (chunk->len % enc_data->u.lines_data.stride) {
        return 0;
    }

    if (enc_data->u.lines_data.reverse) {
        enc_data->u.lines_data.next--;
        *lines = chunk->data + chunk->len - enc_data->u.lines_data.stride;
    } else {
        enc_data->u.lines_data.next++;
        *lines = chunk->data;
    }

    return chunk->len / enc_data->u.lines_data.stride;
}

static int quic_usr_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int lz_usr_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int glz_usr_more_lines(GlzEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int jpeg_usr_more_lines(JpegEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int zlib_usr_more_input(ZlibEncoderUsrContext *usr, uint8_t** input)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    int buf_size;

    if (!usr_data->u.compressed_data.next) {
        spice_assert(usr_data->u.compressed_data.size_left == 0);
        return 0;
    }

    *input = (uint8_t*)usr_data->u.compressed_data.next->buf;
    buf_size = MIN(sizeof(usr_data->u.compressed_data.next->buf),
                   usr_data->u.compressed_data.size_left);

    usr_data->u.compressed_data.next = usr_data->u.compressed_data.next->send_next;
    usr_data->u.compressed_data.size_left -= buf_size;
    return buf_size;
}

static void glz_usr_free_image(GlzEncoderUsrContext *usr, GlzUsrImageContext *image)
{
    GlzData *lz_data = (GlzData *)usr;
    GlzDrawableInstanceItem *glz_drawable_instance = (GlzDrawableInstanceItem *)image;
    DisplayChannelClient *drawable_cc = glz_drawable_instance->red_glz_drawable->dcc;
    DisplayChannelClient *this_cc = SPICE_CONTAINEROF(lz_data, DisplayChannelClient, glz_data);
    if (this_cc == drawable_cc) {
        red_display_free_glz_drawable_instance(drawable_cc, glz_drawable_instance);
    } else {
        /* The glz dictionary is shared between all DisplayChannelClient
         * instances that belong to the same client, and glz_usr_free_image
         * can be called by the dictionary code
         * (glz_dictionary_window_remove_head). Thus this function can be
         * called from any DisplayChannelClient thread, hence the need for
         * this check.
         */
        pthread_mutex_lock(&drawable_cc->glz_drawables_inst_to_free_lock);
        ring_add_before(&glz_drawable_instance->free_link,
                        &drawable_cc->glz_drawables_inst_to_free);
        pthread_mutex_unlock(&drawable_cc->glz_drawables_inst_to_free_lock);
    }
}

static inline void red_init_quic(RedWorker *worker)
{
    worker->quic_data.usr.error = quic_usr_error;
    worker->quic_data.usr.warn = quic_usr_warn;
    worker->quic_data.usr.info = quic_usr_warn;
    worker->quic_data.usr.malloc = quic_usr_malloc;
    worker->quic_data.usr.free = quic_usr_free;
    worker->quic_data.usr.more_space = quic_usr_more_space;
    worker->quic_data.usr.more_lines = quic_usr_more_lines;

    worker->quic = quic_create(&worker->quic_data.usr);

    if (!worker->quic) {
        spice_critical("create quic failed");
    }
}

static inline void red_init_lz(RedWorker *worker)
{
    worker->lz_data.usr.error = lz_usr_error;
    worker->lz_data.usr.warn = lz_usr_warn;
    worker->lz_data.usr.info = lz_usr_warn;
    worker->lz_data.usr.malloc = lz_usr_malloc;
    worker->lz_data.usr.free = lz_usr_free;
    worker->lz_data.usr.more_space = lz_usr_more_space;
    worker->lz_data.usr.more_lines = lz_usr_more_lines;

    worker->lz = lz_create(&worker->lz_data.usr);

    if (!worker->lz) {
        spice_critical("create lz failed");
    }
}

/* TODO: split off to DisplayChannel? avoid just copying those cb pointers */
static inline void red_display_init_glz_data(DisplayChannelClient *dcc)
{
    dcc->glz_data.usr.error = glz_usr_error;
    dcc->glz_data.usr.warn = glz_usr_warn;
    dcc->glz_data.usr.info = glz_usr_warn;
    dcc->glz_data.usr.malloc = glz_usr_malloc;
    dcc->glz_data.usr.free = glz_usr_free;
    dcc->glz_data.usr.more_space = glz_usr_more_space;
    dcc->glz_data.usr.more_lines = glz_usr_more_lines;
    dcc->glz_data.usr.free_image = glz_usr_free_image;
}

static inline void red_init_jpeg(RedWorker *worker)
{
    worker->jpeg_data.usr.more_space = jpeg_usr_more_space;
    worker->jpeg_data.usr.more_lines = jpeg_usr_more_lines;

    worker->jpeg = jpeg_encoder_create(&worker->jpeg_data.usr);

    if (!worker->jpeg) {
        spice_critical("create jpeg encoder failed");
    }
}

static inline void red_init_zlib(RedWorker *worker)
{
    worker->zlib_data.usr.more_space = zlib_usr_more_space;
    worker->zlib_data.usr.more_input = zlib_usr_more_input;

    worker->zlib = zlib_encoder_create(&worker->zlib_data.usr, ZLIB_DEFAULT_COMPRESSION_LEVEL);

    if (!worker->zlib) {
        spice_critical("create zlib encoder failed");
    }
}

#ifdef __GNUC__
#define ATTR_PACKED __attribute__ ((__packed__))
#else
#define ATTR_PACKED
#pragma pack(push)
#pragma pack(1)
#endif


typedef struct ATTR_PACKED rgb32_pixel_t {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t pad;
} rgb32_pixel_t;

typedef struct ATTR_PACKED rgb24_pixel_t {
    uint8_t b;
    uint8_t g;
    uint8_t r;
} rgb24_pixel_t;

typedef uint16_t rgb16_pixel_t;

#ifndef __GNUC__
#pragma pack(pop)
#endif

#undef ATTR_PACKED

#define RED_BITMAP_UTILS_RGB16
#include "red_bitmap_utils.h"
#define RED_BITMAP_UTILS_RGB24
#include "red_bitmap_utils.h"
#define RED_BITMAP_UTILS_RGB32
#include "red_bitmap_utils.h"

#define GRADUAL_HIGH_RGB24_TH -0.03
#define GRADUAL_HIGH_RGB16_TH 0

// setting a more permissive threshold for stream identification in order
// not to miss streams that were artificially scaled on the guest (e.g., full screen view
// in window media player 12). see red_stream_add_frame
#define GRADUAL_MEDIUM_SCORE_TH 0.002

// assumes that stride doesn't overflow
static BitmapGradualType _get_bitmap_graduality_level(RedWorker *worker, SpiceBitmap *bitmap,
                                                      uint32_t group_id)
{
    double score = 0.0;
    int num_samples = 0;
    int num_lines;
    double chunk_score = 0.0;
    int chunk_num_samples = 0;
    uint32_t x, i;
    SpiceChunk *chunk;

    chunk = bitmap->data->chunk;
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        num_lines = chunk[i].len / bitmap->stride;
        x = bitmap->x;
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_16BIT:
            compute_lines_gradual_score_rgb16((rgb16_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_24BIT:
            compute_lines_gradual_score_rgb24((rgb24_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_32BIT:
        case SPICE_BITMAP_FMT_RGBA:
            compute_lines_gradual_score_rgb32((rgb32_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        default:
            spice_error("invalid bitmap format (not RGB) %u", bitmap->format);
        }
        score += chunk_score;
        num_samples += chunk_num_samples;
    }

    spice_assert(num_samples);
    score /= num_samples;

    if (bitmap->format == SPICE_BITMAP_FMT_16BIT) {
        if (score < GRADUAL_HIGH_RGB16_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    } else {
        if (score < GRADUAL_HIGH_RGB24_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    }

    if (score < GRADUAL_MEDIUM_SCORE_TH) {
        return BITMAP_GRADUAL_MEDIUM;
    } else {
        return BITMAP_GRADUAL_LOW;
    }
}

// 判断图像的一行数据是否有添加的对齐数据
static inline int _stride_is_extra(SpiceBitmap *bitmap)
{
    spice_assert(bitmap);
    if (bitmap_fmt_is_rgb(bitmap->format)) {//RGB格式
    	//如果一行的字节数 < 图像行长，那么图像进行了对齐，返回帧，否则返回假
        return ((bitmap->x * BITMAP_FMP_BYTES_PER_PIXEL[bitmap->format]) < bitmap->stride);
    } else { //非RGB，Spice图像中只有5种非RGB图像
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_8BIT: //8位图像，可能字节对齐
            return (bitmap->x < bitmap->stride);
        case SPICE_BITMAP_FMT_4BIT_BE:
        case SPICE_BITMAP_FMT_4BIT_LE: {//4位图像，可能2个像素对齐
            int bytes_width = SPICE_ALIGN(bitmap->x, 2) >> 1;
            return bytes_width < bitmap->stride;
        }
        case SPICE_BITMAP_FMT_1BIT_BE:
        case SPICE_BITMAP_FMT_1BIT_LE: { //二值图像，字节对齐
            int bytes_width = SPICE_ALIGN(bitmap->x, 8) >> 3;
            return bytes_width < bitmap->stride;
        }
        default:
            spice_error("invalid image type %u", bitmap->format);
            return 0;
        }
    }
    return 0;
}

typedef struct compress_send_data_t {
    void*    comp_buf;
    uint32_t comp_buf_size;
    SpicePalette *lzplt_palette;
    int is_lossy;
} compress_send_data_t;

static inline int red_glz_compress_image(DisplayChannelClient *dcc,
                                         SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                         compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif
    spice_assert(bitmap_fmt_is_rgb(src->format));
    GlzData *glz_data = &dcc->glz_data;
    ZlibData *zlib_data;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    RedGlzDrawable *glz_drawable;
    GlzDrawableInstanceItem *glz_drawable_instance;
    int glz_size;
    int zlib_size;

    glz_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    glz_data->data.bufs_head = glz_data->data.bufs_tail;

    if (!glz_data->data.bufs_head) {
        return FALSE;
    }

    glz_data->data.bufs_head->send_next = NULL;
    glz_data->data.dcc = dcc;

    glz_drawable = red_display_get_glz_drawable(dcc, drawable);
    glz_drawable_instance = red_display_add_glz_drawable_instance(glz_drawable);

    glz_data->data.u.lines_data.chunks = src->data;
    glz_data->data.u.lines_data.stride = src->stride;
    glz_data->data.u.lines_data.next = 0;
    glz_data->data.u.lines_data.reverse = 0;
    glz_data->usr.more_lines = glz_usr_more_lines;

    glz_size = glz_encode(dcc->glz, type, src->x, src->y,
                          (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL, 0,
                          src->stride, (uint8_t*)glz_data->data.bufs_head->buf,
                          sizeof(glz_data->data.bufs_head->buf),
                          glz_drawable_instance,
                          &glz_drawable_instance->glz_instance);

    stat_compress_add(&display_channel->glz_stat, start_time, src->stride * src->y, glz_size);

    if (!display_channel->enable_zlib_glz_wrap || (glz_size < MIN_GLZ_SIZE_FOR_ZLIB)) {
        goto glz;
    }
#ifdef COMPRESS_STAT
    start_time = stat_now();
#endif
    zlib_data = &worker->zlib_data;

    zlib_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    zlib_data->data.bufs_head = zlib_data->data.bufs_tail;

    if (!zlib_data->data.bufs_head) {
        spice_warning("failed to allocate zlib compress buffer");
        goto glz;
    }

    zlib_data->data.bufs_head->send_next = NULL;
    zlib_data->data.dcc = dcc;

    zlib_data->data.u.compressed_data.next = glz_data->data.bufs_head;
    zlib_data->data.u.compressed_data.size_left = glz_size;

    zlib_size = zlib_encode(worker->zlib, display_channel->zlib_level,
                            glz_size, (uint8_t*)zlib_data->data.bufs_head->buf,
                            sizeof(zlib_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (zlib_size >= glz_size) {
        while (zlib_data->data.bufs_head) {
            RedCompressBuf *buf = zlib_data->data.bufs_head;
            zlib_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        goto glz;
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_ZLIB_GLZ_RGB;
    dest->u.zlib_glz.glz_data_size = glz_size;
    dest->u.zlib_glz.data_size = zlib_size;

    o_comp_data->comp_buf = zlib_data->data.bufs_head;
    o_comp_data->comp_buf_size = zlib_size;

    stat_compress_add(&display_channel->zlib_glz_stat, start_time, glz_size, zlib_size);
    return TRUE;
glz:
    dest->descriptor.type = SPICE_IMAGE_TYPE_GLZ_RGB;
    dest->u.lz_rgb.data_size = glz_size;

    o_comp_data->comp_buf = glz_data->data.bufs_head;
    o_comp_data->comp_buf_size = glz_size;

    return TRUE;
}

static inline int red_lz_compress_image(DisplayChannelClient *dcc,
                                        SpiceImage *dest, SpiceBitmap *src,
                                        compress_send_data_t* o_comp_data, uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    LzData *lz_data = &worker->lz_data;
    LzContext *lz = worker->lz;
    LzImageType type = MAP_BITMAP_FMT_TO_LZ_IMAGE_TYPE[src->format];
    int size;            // size of the compressed data

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    lz_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    lz_data->data.bufs_head = lz_data->data.bufs_tail;

    if (!lz_data->data.bufs_head) {
        return FALSE;
    }

    lz_data->data.bufs_head->send_next = NULL;
    lz_data->data.dcc = dcc;

    if (setjmp(lz_data->data.jmp_env)) {
        while (lz_data->data.bufs_head) {
            RedCompressBuf *buf = lz_data->data.bufs_head;
            lz_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    lz_data->data.u.lines_data.chunks = src->data;
    lz_data->data.u.lines_data.stride = src->stride;
    lz_data->data.u.lines_data.next = 0;
    lz_data->data.u.lines_data.reverse = 0;
    lz_data->usr.more_lines = lz_usr_more_lines;

    size = lz_encode(lz, type, src->x, src->y,
                     !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                     NULL, 0, src->stride,
                     (uint8_t*)lz_data->data.bufs_head->buf,
                     sizeof(lz_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (size > (src->y * src->stride)) {
        longjmp(lz_data->data.jmp_env, 1);
    }

    if (bitmap_fmt_is_rgb(src->format)) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_RGB;
        dest->u.lz_rgb.data_size = size;

        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
    } else {
        /* masks are 1BIT bitmaps without palettes, but they are not compressed
         * (see fill_mask) */
        spice_assert(src->palette);
        dest->descriptor.type = SPICE_IMAGE_TYPE_LZ_PLT;
        dest->u.lz_plt.data_size = size;
        dest->u.lz_plt.flags = src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;
        dest->u.lz_plt.palette = src->palette;
        dest->u.lz_plt.palette_id = src->palette->unique;
        o_comp_data->comp_buf = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;

        fill_palette(dcc, dest->u.lz_plt.palette, &(dest->u.lz_plt.flags));
        o_comp_data->lzplt_palette = dest->u.lz_plt.palette;
    }

    stat_compress_add(&display_channel->lz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static int red_jpeg_compress_image(DisplayChannelClient *dcc, SpiceImage *dest,
                                   SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                   uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    JpegData *jpeg_data = &worker->jpeg_data;
    LzData *lz_data = &worker->lz_data;
    JpegEncoderContext *jpeg = worker->jpeg;
    LzContext *lz = worker->lz;
    volatile JpegEncoderImageType jpeg_in_type;
    int jpeg_size = 0;
    volatile int has_alpha = FALSE;
    int alpha_lz_size = 0;
    int comp_head_filled;
    int comp_head_left;
    int stride;
    uint8_t *lz_out_start_byte;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif
    switch (src->format) {
    case SPICE_BITMAP_FMT_16BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGR24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        has_alpha = TRUE;
        break;
    default:
        return FALSE;
    }

    jpeg_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    jpeg_data->data.bufs_head = jpeg_data->data.bufs_tail;

    if (!jpeg_data->data.bufs_head) {
        spice_warning("failed to allocate compress buffer");
        return FALSE;
    }

    jpeg_data->data.bufs_head->send_next = NULL;
    jpeg_data->data.dcc = dcc;

    if (setjmp(jpeg_data->data.jmp_env)) {
        while (jpeg_data->data.bufs_head) {
            RedCompressBuf *buf = jpeg_data->data.bufs_head;
            jpeg_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    jpeg_data->data.u.lines_data.chunks = src->data;
    jpeg_data->data.u.lines_data.stride = src->stride;
    jpeg_data->usr.more_lines = jpeg_usr_more_lines;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        jpeg_data->data.u.lines_data.next = 0;
        jpeg_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        jpeg_data->data.u.lines_data.next = src->data->num_chunks - 1;
        jpeg_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    jpeg_size = jpeg_encode(jpeg, display_channel->jpeg_quality, jpeg_in_type,
                            src->x, src->y, NULL,
                            0, stride, (uint8_t*)jpeg_data->data.bufs_head->buf,
                            sizeof(jpeg_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (jpeg_size > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    if (!has_alpha) {
        dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG;
        dest->u.jpeg.data_size = jpeg_size;

        o_comp_data->comp_buf = jpeg_data->data.bufs_head;
        o_comp_data->comp_buf_size = jpeg_size;
        o_comp_data->is_lossy = TRUE;

        stat_compress_add(&display_channel->jpeg_stat, start_time, src->stride * src->y,
                          o_comp_data->comp_buf_size);
        return TRUE;
    }

    lz_data->data.bufs_head = jpeg_data->data.bufs_tail;
    lz_data->data.bufs_tail = lz_data->data.bufs_head;

    comp_head_filled = jpeg_size % sizeof(lz_data->data.bufs_head->buf);
    comp_head_left = sizeof(lz_data->data.bufs_head->buf) - comp_head_filled;
    lz_out_start_byte = ((uint8_t *)lz_data->data.bufs_head->buf) + comp_head_filled;

    lz_data->data.dcc = dcc;

    lz_data->data.u.lines_data.chunks = src->data;
    lz_data->data.u.lines_data.stride = src->stride;
    lz_data->data.u.lines_data.next = 0;
    lz_data->data.u.lines_data.reverse = 0;
    lz_data->usr.more_lines = lz_usr_more_lines;

    alpha_lz_size = lz_encode(lz, LZ_IMAGE_TYPE_XXXA, src->x, src->y,
                               !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN),
                               NULL, 0, src->stride,
                               lz_out_start_byte,
                               comp_head_left);

    // the compressed buffer is bigger than the original data
    if ((jpeg_size + alpha_lz_size) > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_JPEG_ALPHA;
    dest->u.jpeg_alpha.flags = 0;
    if (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN) {
        dest->u.jpeg_alpha.flags |= SPICE_JPEG_ALPHA_FLAGS_TOP_DOWN;
    }

    dest->u.jpeg_alpha.jpeg_size = jpeg_size;
    dest->u.jpeg_alpha.data_size = jpeg_size + alpha_lz_size;

    o_comp_data->comp_buf = jpeg_data->data.bufs_head;
    o_comp_data->comp_buf_size = jpeg_size + alpha_lz_size;
    o_comp_data->is_lossy = TRUE;
    stat_compress_add(&display_channel->jpeg_alpha_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static inline int red_quic_compress_image(DisplayChannelClient *dcc, SpiceImage *dest,
                                          SpiceBitmap *src, compress_send_data_t* o_comp_data,
                                          uint32_t group_id)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    QuicData *quic_data = &worker->quic_data;
    QuicContext *quic = worker->quic;
    volatile QuicImageType type;
    int size, stride;

#ifdef COMPRESS_STAT
    stat_time_t start_time = stat_now();
#endif

    switch (src->format) {
    case SPICE_BITMAP_FMT_32BIT:
        type = QUIC_IMAGE_TYPE_RGB32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        type = QUIC_IMAGE_TYPE_RGBA;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        type = QUIC_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        type = QUIC_IMAGE_TYPE_RGB24;
        break;
    default:
        return FALSE;
    }

    quic_data->data.bufs_tail = red_display_alloc_compress_buf(dcc);
    quic_data->data.bufs_head = quic_data->data.bufs_tail;

    if (!quic_data->data.bufs_head) {
        return FALSE;
    }

    quic_data->data.bufs_head->send_next = NULL;
    quic_data->data.dcc = dcc;

    if (setjmp(quic_data->data.jmp_env)) {
        while (quic_data->data.bufs_head) {
            RedCompressBuf *buf = quic_data->data.bufs_head;
            quic_data->data.bufs_head = buf->send_next;
            red_display_free_compress_buf(dcc, buf);
        }
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    quic_data->data.u.lines_data.chunks = src->data;
    quic_data->data.u.lines_data.stride = src->stride;
    quic_data->usr.more_lines = quic_usr_more_lines;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        quic_data->data.u.lines_data.next = 0;
        quic_data->data.u.lines_data.reverse = 0;
        stride = src->stride;
    } else {
        quic_data->data.u.lines_data.next = src->data->num_chunks - 1;
        quic_data->data.u.lines_data.reverse = 1;
        stride = -src->stride;
    }
    size = quic_encode(quic, type, src->x, src->y, NULL, 0, stride,
                       quic_data->data.bufs_head->buf,
                       sizeof(quic_data->data.bufs_head->buf) >> 2);

    // the compressed buffer is bigger than the original data
    if ((size << 2) > (src->y * src->stride)) {
        longjmp(quic_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_QUIC;
    dest->u.quic.data_size = size << 2;

    o_comp_data->comp_buf = quic_data->data.bufs_head;
    o_comp_data->comp_buf_size = size << 2;

    stat_compress_add(&display_channel->quic_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

#define MIN_SIZE_TO_COMPRESS 54
#define MIN_DIMENSION_TO_QUIC 3
static inline int red_compress_image(DisplayChannelClient *dcc,
                                     SpiceImage *dest, SpiceBitmap *src, Drawable *drawable,
                                     int can_lossy,
                                     compress_send_data_t* o_comp_data)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    spice_image_compression_t image_compression =
        display_channel->common.worker->image_compression;
    int quic_compress = FALSE;

    if ((image_compression == SPICE_IMAGE_COMPRESS_OFF) ||
        ((src->y * src->stride) < MIN_SIZE_TO_COMPRESS)) { // TODO: change the size cond
        return FALSE;
    } else if (image_compression == SPICE_IMAGE_COMPRESS_QUIC) {
        if (BITMAP_FMT_IS_PLT[src->format]) {
            return FALSE;
        } else {
            quic_compress = TRUE;
        }
    } else {
        /*
            lz doesn't handle (1) bitmaps with strides that are larger than the width
            of the image in bytes (2) unstable bitmaps
        */
        if (_stride_is_extra(src) || (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
            if ((image_compression == SPICE_IMAGE_COMPRESS_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_GLZ) ||
                BITMAP_FMT_IS_PLT[src->format]) {
                return FALSE;
            } else {
                quic_compress = TRUE;
            }
        } else {
            if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) {
                if ((src->x < MIN_DIMENSION_TO_QUIC) || (src->y < MIN_DIMENSION_TO_QUIC)) {
                    quic_compress = FALSE;
                } else {
                    if (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_INVALID) {
                        quic_compress = BITMAP_FMT_HAS_GRADUALITY(src->format) &&
                            (_get_bitmap_graduality_level(display_channel->common.worker, src,
                                                          drawable->group_id) ==
                             BITMAP_GRADUAL_HIGH);
                    } else {
                        quic_compress = (drawable->copy_bitmap_graduality == BITMAP_GRADUAL_HIGH);
                    }
                }
            } else {
                quic_compress = FALSE;
            }
        }
    }

    if (quic_compress) {
#ifdef COMPRESS_DEBUG
        spice_info("QUIC compress");
#endif
        // if bitmaps is picture-like, compress it using jpeg
        if (can_lossy && display_channel->enable_jpeg &&
            ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ))) {
            // if we use lz for alpha, the stride can't be extra
            if (src->format != SPICE_BITMAP_FMT_RGBA || !_stride_is_extra(src)) {
                return red_jpeg_compress_image(dcc, dest,
                                               src, o_comp_data, drawable->group_id);
            }
        }
        return red_quic_compress_image(dcc, dest,
                                       src, o_comp_data, drawable->group_id);
    } else {
        int glz;
        int ret;
        if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_GLZ) ||
            (image_compression == SPICE_IMAGE_COMPRESS_GLZ)) {
            glz = BITMAP_FMT_HAS_GRADUALITY(src->format) && (
                    (src->x * src->y) < glz_enc_dictionary_get_size(
                        dcc->glz_dict->dict));
        } else if ((image_compression == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
                   (image_compression == SPICE_IMAGE_COMPRESS_LZ)) {
            glz = FALSE;
        } else {
            spice_error("invalid image compression type %u", image_compression);
            return FALSE;
        }

        if (glz) {
            /* using the global dictionary only if it is not frozen */
            pthread_rwlock_rdlock(&dcc->glz_dict->encode_lock);
            if (!dcc->glz_dict->migrate_freeze) {
                ret = red_glz_compress_image(dcc,
                                             dest, src,
                                             drawable, o_comp_data);
            } else {
                glz = FALSE;
            }
            pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
        }

        if (!glz) {
            ret = red_lz_compress_image(dcc, dest, src, o_comp_data,
                                        drawable->group_id);
#ifdef COMPRESS_DEBUG
            spice_info("LZ LOCAL compress");
#endif
        }
#ifdef COMPRESS_DEBUG
        else {
            spice_info("LZ global compress fmt=%d", src->format);
        }
#endif
        return ret;
    }
}

//添加图像缓存
static inline void 
red_display_add_image_to_pixmap_cache(RedChannelClient *rcc,
	SpiceImage *image, SpiceImage *io_image,
	int is_lossy)
{
    DisplayChannel *display_channel = 
		SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) 
	{ //必须带CACHE_ME标记
        spice_assert(image->descriptor.width * image->descriptor.height > 0);
		//如果没有设置CACHE_REPLACE_ME替换标记
        if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME)) 
		{	
			//把image添加到缓存，目标dcc的pixmap_cache，id，图像大小，是否压缩了，占位指针dcc
            if (pixmap_cache_add(dcc->pixmap_cache, image->descriptor.id,
					image->descriptor.width * image->descriptor.height, is_lossy, dcc))
			{
                io_image->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
                dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
                                                                               image->descriptor.id;
                stat_inc_counter(display_channel->add_to_cache_counter, 1);
            }
        }
    }

    if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        stat_inc_counter(display_channel->non_cache_counter, 1);
    }
}

typedef enum {
    FILL_BITS_TYPE_INVALID,
    FILL_BITS_TYPE_CACHE,
    FILL_BITS_TYPE_SURFACE,
    FILL_BITS_TYPE_COMPRESS_LOSSLESS,
    FILL_BITS_TYPE_COMPRESS_LOSSY,
    FILL_BITS_TYPE_BITMAP,
} FillBitsType;

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
static FillBitsType fill_bits(DisplayChannelClient *dcc, SpiceMarshaller *m,
                              SpiceImage *simage, Drawable *drawable, int can_lossy)
{
    RedChannelClient *rcc = &dcc->common.base;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    RedWorker *worker = dcc->common.worker;
    SpiceImage image;
    compress_send_data_t comp_send_data = {0};
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    if (simage == NULL) { //如果传入的spice图像为空，则使用Drawable的self_bitmap_image
        spice_assert(drawable->red_drawable->self_bitmap_image);
        simage = drawable->red_drawable->self_bitmap_image;
    }

    image.descriptor = simage->descriptor; //复制图像描述符
    image.descriptor.flags = 0; //标记清0
    if (simage->descriptor.flags & SPICE_IMAGE_FLAGS_HIGH_BITS_SET) { //只保留HIGH_BITS_SET标记
        image.descriptor.flags = SPICE_IMAGE_FLAGS_HIGH_BITS_SET; //CACHE_ME, CACHE_REPLACE_ME不要了
    }

    if ((simage->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) { //设置CACHE_ME
        int lossy_cache_item;
        if (pixmap_cache_hit(dcc->pixmap_cache, image.descriptor.id,
                             &lossy_cache_item, dcc)) //
        {
            dcc->send_data.pixmap_cache_items[dcc->send_data.num_pixmap_cache_items++] =
				image.descriptor.id;
            if (can_lossy || !lossy_cache_item) {
                if (!display_channel->enable_jpeg || lossy_cache_item) {
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
                } else {
                    // making sure, in multiple monitor scenario, that lossy items that
                    // should have been replaced with lossless data by one display channel,
                    // will be retrieved as lossless by another display channel.
                    image.descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE_LOSSLESS;
                }
                spice_marshall_Image(m, &image,
                                     &bitmap_palette_out, &lzplt_palette_out);
                spice_assert(bitmap_palette_out == NULL);
                spice_assert(lzplt_palette_out == NULL);
                stat_inc_counter(display_channel->cache_hits_counter, 1);
                return FILL_BITS_TYPE_CACHE;
            } else {
                pixmap_cache_set_lossy(dcc->pixmap_cache, simage->descriptor.id,
                                       FALSE);
                image.descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME;
            }
        }
    }

    switch (simage->descriptor.type) {
    case SPICE_IMAGE_TYPE_SURFACE: {
        int surface_id;
        RedSurface *surface;

        surface_id = simage->u.surface.surface_id;
        if (!validate_surface(worker, surface_id)) {
            rendering_incorrect("SPICE_IMAGE_TYPE_SURFACE");
            return FILL_BITS_TYPE_SURFACE;
        }

        surface = &worker->surfaces[surface_id];
        image.descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
        image.descriptor.flags = 0;
        image.descriptor.width = surface->context.width;
        image.descriptor.height = surface->context.height;

        image.u.surface.surface_id = surface_id;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == NULL);
        spice_assert(lzplt_palette_out == NULL);
        return FILL_BITS_TYPE_SURFACE;
    }
    case SPICE_IMAGE_TYPE_BITMAP: {
        SpiceBitmap *bitmap = &image.u.bitmap;
#ifdef DUMP_BITMAP
        dump_bitmap(&simage->u.bitmap);
#endif
        /* Images must be added to the cache only after they are compressed
           in order to prevent starvation in the client between pixmap_cache and
           global dictionary (in cases of multiple monitors) */
        if (!red_compress_image(dcc, &image, &simage->u.bitmap,
                                drawable, can_lossy, &comp_send_data)) {
            SpicePalette *palette;

            red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);

            *bitmap = simage->u.bitmap;
            bitmap->flags = bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;

            palette = bitmap->palette;
            fill_palette(dcc, palette, &bitmap->flags);
            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(lzplt_palette_out == NULL);

            if (bitmap_palette_out && palette) {
                spice_marshall_Palette(bitmap_palette_out, palette);
            }

            spice_marshaller_add_ref_chunks(m, bitmap->data);
            return FILL_BITS_TYPE_BITMAP;
        } else {
            red_display_add_image_to_pixmap_cache(rcc, simage, &image,
                                                  comp_send_data.is_lossy);

            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(bitmap_palette_out == NULL);

            marshaller_add_compressed(m, comp_send_data.comp_buf,
                                      comp_send_data.comp_buf_size);

            if (lzplt_palette_out && comp_send_data.lzplt_palette) {
                spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
            }

            spice_assert(!comp_send_data.is_lossy || can_lossy);
            return (comp_send_data.is_lossy ? FILL_BITS_TYPE_COMPRESS_LOSSY :
                                              FILL_BITS_TYPE_COMPRESS_LOSSLESS);
        }
        break;
    }
    case SPICE_IMAGE_TYPE_QUIC:
        red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);
        image.u.quic = simage->u.quic;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == NULL);
        spice_assert(lzplt_palette_out == NULL);
        spice_marshaller_add_ref_chunks(m, image.u.quic.data);
        return FILL_BITS_TYPE_COMPRESS_LOSSLESS;
    default:
        spice_error("invalid image type %u", image.descriptor.type);
    }

    return 0;
}

static void fill_mask(RedChannelClient *rcc, SpiceMarshaller *m,
                      SpiceImage *mask_bitmap, Drawable *drawable)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (mask_bitmap && m) {
        if (display_channel->common.worker->image_compression != SPICE_IMAGE_COMPRESS_OFF) {
            spice_image_compression_t save_img_comp =
                display_channel->common.worker->image_compression;
            display_channel->common.worker->image_compression = SPICE_IMAGE_COMPRESS_OFF;
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
            display_channel->common.worker->image_compression = save_img_comp;
        } else {
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
        }
    }
}

static void fill_attr(SpiceMarshaller *m, SpiceLineAttr *attr, uint32_t group_id)
{
    int i;

    if (m && attr->style_nseg) {
        for (i = 0 ; i < attr->style_nseg; i++) {
            spice_marshaller_add_uint32(m, attr->style[i]);
        }
    }
}

static void fill_cursor(CursorChannelClient *ccc, SpiceCursor *red_cursor,
                        CursorItem *cursor, AddBufInfo *addbuf)
{
    RedCursorCmd *cursor_cmd;
    addbuf->data = NULL;

    if (!cursor) {
        red_cursor->flags = SPICE_CURSOR_FLAGS_NONE;
        return;
    }

    cursor_cmd = cursor->red_cursor;
    *red_cursor = cursor_cmd->u.set.shape;

    if (red_cursor->header.unique) {
        if (red_cursor_cache_find(ccc, red_cursor->header.unique)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_FROM_CACHE;
            return;
        }
        if (red_cursor_cache_add(ccc, red_cursor->header.unique, 1)) {
            red_cursor->flags |= SPICE_CURSOR_FLAGS_CACHE_ME;
        }
    }

    if (red_cursor->data_size) {
        addbuf->type = BUF_TYPE_RAW;
        addbuf->data = red_cursor->data;
        addbuf->size = red_cursor->data_size;
    }
}

static inline void red_display_reset_send_data(DisplayChannelClient *dcc)
{
    red_display_reset_compress_buf(dcc);
    dcc->send_data.free_list.res->count = 0; //重置FREE_LIST
    dcc->send_data.num_pixmap_cache_items = 0; //
    memset(dcc->send_data.free_list.sync, 0, sizeof(dcc->send_data.free_list.sync));
}

/* set area=NULL for testing the whole surface */
static int is_surface_area_lossy(DisplayChannelClient *dcc, uint32_t surface_id,
                                 const SpiceRect *area, SpiceRect *out_lossy_area)
{
    RedSurface *surface;
    QRegion *surface_lossy_region;
    QRegion lossy_region;
    RedWorker *worker = dcc->common.worker;

    VALIDATE_SURFACE_RETVAL(worker, surface_id, FALSE);
    surface = &worker->surfaces[surface_id];
    surface_lossy_region = &dcc->surface_client_lossy_region[surface_id];//有损区域

    if (!area) { //测试整个surface
        if (region_is_empty(surface_lossy_region)) {
            return FALSE;
        } else {
            out_lossy_area->top = 0;
            out_lossy_area->left = 0;
            out_lossy_area->bottom = surface->context.height;
            out_lossy_area->right = surface->context.width;
            return TRUE;
        }
    }

    region_init(&lossy_region);
    region_add(&lossy_region, area);
    region_and(&lossy_region, surface_lossy_region);
    if (!region_is_empty(&lossy_region)) {
        out_lossy_area->left = lossy_region.extents.x1;
        out_lossy_area->top = lossy_region.extents.y1;
        out_lossy_area->right = lossy_region.extents.x2;
        out_lossy_area->bottom = lossy_region.extents.y2;
        region_destroy(&lossy_region);
        return TRUE;
    } else {
        return FALSE;
    }
}
/* returns if the bitmap was already sent lossy to the client. If the bitmap hasn't been sent yet
   to the client, returns false. "area" is for surfaces. If area = NULL,
   all the surface is considered. out_lossy_data will hold info about the bitmap, and its lossy
   area in case it is lossy and part of a surface. */
static int is_bitmap_lossy(RedChannelClient *rcc, SpiceImage *image, SpiceRect *area,
                           Drawable *drawable, BitmapData *out_data)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    if (image == NULL) {
        // self bitmap
        out_data->type = BITMAP_DATA_TYPE_BITMAP;
        return FALSE;
    }

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int is_hit_lossy;

        out_data->id = image->descriptor.id;
        if (pixmap_cache_hit(dcc->pixmap_cache, image->descriptor.id,
                             &is_hit_lossy, dcc)) {
            out_data->type = BITMAP_DATA_TYPE_CACHE;
            if (is_hit_lossy) {
                return TRUE;
            } else {
                return FALSE;
            }
        } else {
            out_data->type = BITMAP_DATA_TYPE_BITMAP_TO_CACHE;
        }
    } else {
         out_data->type = BITMAP_DATA_TYPE_BITMAP;
    }

    if (image->descriptor.type != SPICE_IMAGE_TYPE_SURFACE) {
        return FALSE;
    }

    out_data->type = BITMAP_DATA_TYPE_SURFACE;
    out_data->id = image->u.surface.surface_id;

    if (is_surface_area_lossy(dcc, out_data->id,
                              area, &out_data->lossy_rect))
    {
        return TRUE;
    } else {
        return FALSE;
    }
}

static int is_brush_lossy(RedChannelClient *rcc, SpiceBrush *brush,
                          Drawable *drawable, BitmapData *out_data)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        return is_bitmap_lossy(rcc, brush->u.pattern.pat, NULL,
                               drawable, out_data);
    } else {
        out_data->type = BITMAP_DATA_TYPE_INVALID;
        return FALSE;
    }
}

static void surface_lossy_region_update(RedWorker *worker, DisplayChannelClient *dcc,
                                        Drawable *item, int has_mask, int lossy)
{
    QRegion *surface_lossy_region;
    RedDrawable *drawable;

    if (has_mask && !lossy) {
        return;
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    drawable = item->red_drawable;

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS ) {
        QRegion clip_rgn;
        QRegion draw_region;
        region_init(&clip_rgn);
        region_init(&draw_region);
        region_add(&draw_region, &drawable->bbox);
        add_clip_rects(&clip_rgn, drawable->clip.rects);
        region_and(&draw_region, &clip_rgn);
        if (lossy) {
            region_or(surface_lossy_region, &draw_region);
        } else {
            region_exclude(surface_lossy_region, &draw_region);
        }

        region_destroy(&clip_rgn);
        region_destroy(&draw_region);
    } else { /* no clip */
        if (!lossy) {
            region_remove(surface_lossy_region, &drawable->bbox);
        } else {
            region_add(surface_lossy_region, &drawable->bbox);
        }
    }
}

static inline int drawable_intersects_with_areas(Drawable *drawable, int surface_ids[],
                                                 SpiceRect *surface_areas[],
                                                 int num_surfaces)
{
    int i;
    for (i = 0; i < num_surfaces; i++) {
        if (surface_ids[i] == drawable->red_drawable->surface_id) {
            if (rect_intersects(surface_areas[i], &drawable->red_drawable->bbox)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static inline int drawable_depends_on_areas(Drawable *drawable,
                                            int surface_ids[],
                                            SpiceRect surface_areas[],
                                            int num_surfaces)
{
    int i;
    RedDrawable *red_drawable;
    int drawable_has_shadow;
    SpiceRect shadow_rect = {0, 0, 0, 0};

    red_drawable = drawable->red_drawable;
    drawable_has_shadow = has_shadow(red_drawable);

    if (drawable_has_shadow) {
       int delta_x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left;
       int delta_y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top;

       shadow_rect.left = red_drawable->u.copy_bits.src_pos.x;
       shadow_rect.top = red_drawable->u.copy_bits.src_pos.y;
       shadow_rect.right = red_drawable->bbox.right + delta_x;
       shadow_rect.bottom = red_drawable->bbox.bottom + delta_y;
    }

    for (i = 0; i < num_surfaces; i++) {
        int x;
        int dep_surface_id;

         for (x = 0; x < 3; ++x) {
            dep_surface_id = drawable->surfaces_dest[x];
            if (dep_surface_id == surface_ids[i]) {
                if (rect_intersects(&surface_areas[i], &red_drawable->surfaces_rects[x])) {
                    return TRUE;
                }
            }
        }

        if (surface_ids[i] == red_drawable->surface_id) {
            if (drawable_has_shadow) {
                if (rect_intersects(&surface_areas[i], &shadow_rect)) {
                    return TRUE;
                }
            }

            // not dependent on dest
            if (red_drawable->effect == QXL_EFFECT_OPAQUE) {
                continue;
            }

            if (rect_intersects(&surface_areas[i], &red_drawable->bbox)) {
                return TRUE;
            }
        }

    }
    return FALSE;
}


static int pipe_rendered_drawables_intersect_with_areas(RedWorker *worker,
                                                        DisplayChannelClient *dcc,
                                                        int surface_ids[],
                                                        SpiceRect *surface_areas[],
                                                        int num_surfaces)
{
    PipeItem *pipe_item;
    Ring *pipe;

    spice_assert(num_surfaces);
    pipe = &dcc->common.base.pipe;

    for (pipe_item = (PipeItem *)ring_get_head(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_next(pipe, &pipe_item->link))
    {
        Drawable *drawable;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        drawable = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item)->drawable;

        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        if (drawable_intersects_with_areas(drawable, surface_ids, surface_areas, num_surfaces)) {
            return TRUE;
        }
    }

    return FALSE;
}

static void red_pipe_replace_rendered_drawables_with_images(RedWorker *worker,
                                                            DisplayChannelClient *dcc,
                                                            int first_surface_id,
                                                            SpiceRect *first_area)
{
    /* TODO: can't have those statics with multiple clients */
    static int resent_surface_ids[MAX_PIPE_SIZE];
    static SpiceRect resent_areas[MAX_PIPE_SIZE]; // not pointers since drawbales may be released
    int num_resent;
    PipeItem *pipe_item;
    Ring *pipe;

    resent_surface_ids[0] = first_surface_id;
    resent_areas[0] = *first_area;
    num_resent = 1;

    pipe = &dcc->common.base.pipe;

    // going from the oldest to the newest
    for (pipe_item = (PipeItem *)ring_get_tail(pipe);
         pipe_item;
         pipe_item = (PipeItem *)ring_prev(pipe, &pipe_item->link)) {
        Drawable *drawable;
        DrawablePipeItem *dpi;
        ImageItem *image;

        if (pipe_item->type != PIPE_ITEM_TYPE_DRAW)
            continue;
        dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        drawable = dpi->drawable;
        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        // When a drawable command, X, depends on bitmaps that were resent,
        // these bitmaps state at the client might not be synchronized with X
        // (i.e., the bitmaps can be more futuristic w.r.t X). Thus, X shouldn't
        // be rendered at the client, and we replace it with an image as well.
        if (!drawable_depends_on_areas(drawable,
                                       resent_surface_ids,
                                       resent_areas,
                                       num_resent)) {
            continue;
        }

        image = red_add_surface_area_image(dcc, drawable->red_drawable->surface_id,
                                           &drawable->red_drawable->bbox, pipe_item, TRUE);
        resent_surface_ids[num_resent] = drawable->red_drawable->surface_id;
        resent_areas[num_resent] = drawable->red_drawable->bbox;
        num_resent++;

        spice_assert(image);
        red_channel_client_pipe_remove_and_release(&dcc->common.base, &dpi->dpi_pipe_item);
        pipe_item = &image->link;
    }
}

static void red_add_lossless_drawable_dependencies(RedWorker *worker,
                                                   RedChannelClient *rcc,
                                                   Drawable *item,
                                                   int deps_surfaces_ids[],
                                                   SpiceRect *deps_areas[],
                                                   int num_deps)
{
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    int sync_rendered = FALSE;
    int i;

    if (!ring_item_is_linked(&item->list_link)) {
        /* drawable was already rendered, we may not be able to retrieve the lossless data
           for the lossy areas */
        sync_rendered = TRUE;

        // checking if the drawable itself or one of the other commands
        // that were rendered, affected the areas that need to be resent
        if (!drawable_intersects_with_areas(item, deps_surfaces_ids,
                                            deps_areas, num_deps)) {
            if (pipe_rendered_drawables_intersect_with_areas(worker, dcc,
                                                             deps_surfaces_ids,
                                                             deps_areas,
                                                             num_deps)) {
                sync_rendered = TRUE;
            }
        } else {
            sync_rendered = TRUE;
        }
    } else {
        sync_rendered = FALSE;
        for (i = 0; i < num_deps; i++) {
            red_update_area_till(worker, deps_areas[i],
                                 deps_surfaces_ids[i], item);
        }
    }

    if (!sync_rendered) {
        // pushing the pipe item back to the pipe
        red_pipe_add_drawable_to_tail(dcc, item);
        // the surfaces areas will be sent as DRAW_COPY commands, that
        // will be executed before the current drawable
        for (i = 0; i < num_deps; i++) {
            red_add_surface_area_image(dcc, deps_surfaces_ids[i], deps_areas[i],
                                       red_pipe_get_tail(dcc), FALSE);

        }
    } else {
        int drawable_surface_id[1];
        SpiceRect *drawable_bbox[1];

        drawable_surface_id[0] = drawable->surface_id;
        drawable_bbox[0] = &drawable->bbox;

        // check if the other rendered images in the pipe have updated the drawable bbox
        if (pipe_rendered_drawables_intersect_with_areas(worker, dcc,
                                                         drawable_surface_id,
                                                         drawable_bbox,
                                                         1)) {
            red_pipe_replace_rendered_drawables_with_images(worker, dcc,
                                                            drawable->surface_id,
                                                            &drawable->bbox);
        }

        red_add_surface_area_image(dcc, drawable->surface_id, &drawable->bbox,
                                   red_pipe_get_tail(dcc), TRUE);
    }
}

static void red_marshall_qxl_draw_fill(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceFill fill;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_FILL, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    fill = drawable->u.fill;
    spice_marshall_Fill(base_marshaller,
                        &fill,
                        &brush_pat_out,
                        &mask_bitmap_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, fill.brush.u.pattern.pat, item, FALSE);
    }

    fill_mask(rcc, mask_bitmap_out, fill.mask.bitmap, item);
}


static void red_lossy_marshall_qxl_draw_fill(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *m,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;

    int dest_allowed_lossy = FALSE;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    uint16_t rop;

    rop = drawable->u.fill.rop_descriptor;

    dest_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                           (rop & SPICE_ROPD_OP_AND) ||
                           (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.fill.brush, item,
                                    &brush_bitmap_data);
    if (!dest_allowed_lossy) {
        dest_is_lossy = is_surface_area_lossy(dcc, item->surface_id, &drawable->bbox,
                                              &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        !(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        int has_mask = !!drawable->u.fill.mask.bitmap;

        red_marshall_qxl_draw_fill(worker, rcc, m, dpi);
        // either the brush operation is opaque, or the dest is not lossy
        surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_opaque(RedWorker *worker,
                                             RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             DrawablePipeItem *dpi, int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceOpaque opaque;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_OPAQUE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    opaque = drawable->u.opaque;
    spice_marshall_Opaque(base_marshaller,
                          &opaque,
                          &src_bitmap_out,
                          &brush_pat_out,
                          &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, opaque.src_bitmap, item,
                              src_allowed_lossy);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, opaque.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, opaque.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_opaque(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;

    int src_allowed_lossy;
    int rop;
    int src_is_lossy = FALSE;
    int brush_is_lossy = FALSE;
    BitmapData src_bitmap_data;
    BitmapData brush_bitmap_data;

    rop = drawable->u.opaque.rop_descriptor;
    src_allowed_lossy = !((rop & SPICE_ROPD_OP_OR) ||
                          (rop & SPICE_ROPD_OP_AND) ||
                          (rop & SPICE_ROPD_OP_XOR));

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.opaque.brush, item,
                                    &brush_bitmap_data);

    if (!src_allowed_lossy) {
        src_is_lossy = is_bitmap_lossy(rcc, drawable->u.opaque.src_bitmap,
                                       &drawable->u.opaque.src_area,
                                       item,
                                       &src_bitmap_data);
    }

    if (!(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) &&
        !(src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        FillBitsType src_send_type;
        int has_mask = !!drawable->u.opaque.mask.bitmap;

        src_send_type = red_marshall_qxl_draw_opaque(worker, rcc, m, dpi, src_allowed_lossy);
        if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
            src_is_lossy = TRUE;
        } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
            src_is_lossy = FALSE;
        }

        surface_lossy_region_update(worker, dcc, item, has_mask, src_is_lossy);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_copy(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *base_marshaller,
                                           DrawablePipeItem *dpi, int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceCopy copy;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy = drawable->u.copy;
    spice_marshall_Copy(base_marshaller,
                        &copy,
                        &src_bitmap_out,
                        &mask_bitmap_out);

    src_send_type = fill_bits(dcc, src_bitmap_out, copy.src_bitmap, item, src_allowed_lossy);
    fill_mask(rcc, mask_bitmap_out, copy.mask.bitmap, item);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_copy(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.copy.mask.bitmap;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.copy.src_bitmap,
                                   &drawable->u.copy.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_copy(worker, rcc, base_marshaller, dpi, TRUE);
    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }
    surface_lossy_region_update(worker, dcc, item, has_mask,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_transparent(RedWorker *worker,
                                          RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceTransparent transparent;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TRANSPARENT,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    transparent = drawable->u.transparent;
    spice_marshall_Transparent(base_marshaller,
                               &transparent,
                               &src_bitmap_out);
    fill_bits(dcc, src_bitmap_out, transparent.src_bitmap, item, FALSE);
}

static void red_lossy_marshall_qxl_draw_transparent(RedWorker *worker,
                                                RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.transparent.src_bitmap,
                                   &drawable->u.transparent.src_area, item, &src_bitmap_data);

    if (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) {
        red_marshall_qxl_draw_transparent(worker, rcc, base_marshaller, dpi);
        // don't update surface lossy region since transperent areas might be lossy
    } else {
        int resend_surface_ids[1];
        SpiceRect *resend_areas[1];

        resend_surface_ids[0] = src_bitmap_data.id;
        resend_areas[0] = &src_bitmap_data.lossy_rect;

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, 1);
    }
}

static FillBitsType red_marshall_qxl_draw_alpha_blend(RedWorker *worker,
                                                  RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi,
                                                  int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceAlphaBlend alpha_blend;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND,
                                      &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    alpha_blend = drawable->u.alpha_blend;
    spice_marshall_AlphaBlend(base_marshaller,
                              &alpha_blend,
                              &src_bitmap_out);
    src_send_type = fill_bits(dcc, src_bitmap_out, alpha_blend.src_bitmap, item,
                              src_allowed_lossy);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_alpha_blend(RedWorker *worker,
                                                RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.alpha_blend.src_bitmap,
                                   &drawable->u.alpha_blend.src_area, item, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_alpha_blend(worker, rcc, base_marshaller, dpi, TRUE);

    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }

    if (src_is_lossy) {
        surface_lossy_region_update(worker, dcc, item, FALSE, src_is_lossy);
    } // else, the area stays lossy/lossless as the destination
}

static void red_marshall_qxl_copy_bits(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpicePoint copy_bits;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_COPY_BITS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    copy_bits = drawable->u.copy_bits.src_pos;
    spice_marshall_Point(base_marshaller,
                         &copy_bits);
}

static void red_lossy_marshall_qxl_copy_bits(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRect src_rect;
    int horz_offset;
    int vert_offset;
    int src_is_lossy;
    SpiceRect src_lossy_area;

    red_marshall_qxl_copy_bits(worker, rcc, base_marshaller, dpi);

    horz_offset = drawable->u.copy_bits.src_pos.x - drawable->bbox.left;
    vert_offset = drawable->u.copy_bits.src_pos.y - drawable->bbox.top;

    src_rect.left = drawable->u.copy_bits.src_pos.x;
    src_rect.top = drawable->u.copy_bits.src_pos.y;
    src_rect.right = drawable->bbox.right + horz_offset;
    src_rect.bottom = drawable->bbox.bottom + vert_offset;

    src_is_lossy = is_surface_area_lossy(dcc, item->surface_id,
                                         &src_rect, &src_lossy_area);

    surface_lossy_region_update(worker, dcc, item, FALSE,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_blend(RedWorker *worker,
                                    RedChannelClient *rcc,
                                    SpiceMarshaller *base_marshaller,
                                    DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlend blend;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLEND, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blend = drawable->u.blend;
    spice_marshall_Blend(base_marshaller,
                         &blend,
                         &src_bitmap_out,
                         &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, blend.src_bitmap, item, FALSE);

    fill_mask(rcc, mask_bitmap_out, blend.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blend(RedWorker *worker,
                                          RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.blend.src_bitmap,
                                   &drawable->u.blend.src_area, item, &src_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if (!dest_is_lossy &&
        (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_blend(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_blackness(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlackness blackness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLACKNESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    blackness = drawable->u.blackness;

    spice_marshall_Blackness(base_marshaller,
                             &blackness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, blackness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blackness(RedWorker *worker,
                                              RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.blackness.mask.bitmap;

    red_marshall_qxl_draw_blackness(worker, rcc, base_marshaller, dpi);

    surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_whiteness(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceWhiteness whiteness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_WHITENESS, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    whiteness = drawable->u.whiteness;

    spice_marshall_Whiteness(base_marshaller,
                             &whiteness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, whiteness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_whiteness(RedWorker *worker,
                                              RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.whiteness.mask.bitmap;

    red_marshall_qxl_draw_whiteness(worker, rcc, base_marshaller, dpi);

    surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_inverse(RedWorker *worker,
                                        RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        Drawable *item)
{
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceInvers inverse;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_INVERS, NULL);
    fill_base(base_marshaller, item);
    inverse = drawable->u.invers;

    spice_marshall_Invers(base_marshaller,
                          &inverse,
                          &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, inverse.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_inverse(RedWorker *worker,
                                            RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            Drawable *item)
{
    red_marshall_qxl_draw_inverse(worker, rcc, base_marshaller, item);
}

static void red_marshall_qxl_draw_rop3(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRop3 rop3;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ROP3, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    rop3 = drawable->u.rop3;
    spice_marshall_Rop3(base_marshaller,
                        &rop3,
                        &src_bitmap_out,
                        &brush_pat_out,
                        &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, rop3.src_bitmap, item, FALSE);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, rop3.brush.u.pattern.pat, item, FALSE);
    }
    fill_mask(rcc, mask_bitmap_out, rop3.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_rop3(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.rop3.src_bitmap,
                                   &drawable->u.rop3.src_area, item, &src_bitmap_data);
    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.rop3.brush, item,
                                    &brush_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        int has_mask = !!drawable->u.rop3.mask.bitmap;
        red_marshall_qxl_draw_rop3(worker, rcc, base_marshaller, dpi);
        surface_lossy_region_update(worker, dcc, item, has_mask, FALSE);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_composite(RedWorker *worker,
                                     RedChannelClient *rcc,
                                     SpiceMarshaller *base_marshaller,
                                     DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceComposite composite;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COMPOSITE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    composite = drawable->u.composite;
    spice_marshall_Composite(base_marshaller,
                             &composite,
                             &src_bitmap_out,
                             &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, composite.src_bitmap, item, FALSE);
    if (mask_bitmap_out) {
        fill_bits(dcc, mask_bitmap_out, composite.mask_bitmap, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_composite(RedWorker *worker,
                                                  RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int mask_is_lossy;
    BitmapData mask_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.composite.src_bitmap,
                                   NULL, item, &src_bitmap_data);
    mask_is_lossy = drawable->u.composite.mask_bitmap &&
        is_bitmap_lossy(rcc, drawable->u.composite.mask_bitmap, NULL, item, &mask_bitmap_data);

    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))   &&
        (!mask_is_lossy || (mask_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        red_marshall_qxl_draw_composite(worker, rcc, base_marshaller, dpi);
        surface_lossy_region_update(worker, dcc, item, FALSE, FALSE);
    }
    else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = src_bitmap_data.id;
            resend_areas[num_resend] = &src_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (mask_is_lossy && (mask_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = mask_bitmap_data.id;
            resend_areas[num_resend] = &mask_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = item->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_stroke(RedWorker *worker,
                                     RedChannelClient *rcc,
                                     SpiceMarshaller *base_marshaller,
                                     DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceStroke stroke;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *style_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_STROKE, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    stroke = drawable->u.stroke;
    spice_marshall_Stroke(base_marshaller,
                          &stroke,
                          &style_out,
                          &brush_pat_out);

    fill_attr(style_out, &stroke.attr, item->group_id);
    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, stroke.brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_stroke(RedWorker *worker,
                                           RedChannelClient *rcc,
                                           SpiceMarshaller *base_marshaller,
                                           DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop;

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.stroke.brush, item,
                                    &brush_bitmap_data);

    // back_mode is not used at the client. Ignoring.
    rop = drawable->u.stroke.fore_mode;

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.stroke.brush.type != SPICE_BRUSH_TYPE_SOLID &&
        ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR))) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)))
    {
        red_marshall_qxl_draw_stroke(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[2];
        SpiceRect *resend_areas[2];
        int num_resend = 0;

        if (brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = brush_bitmap_data.id;
            resend_areas[num_resend] = &brush_bitmap_data.lossy_rect;
            num_resend++;
        }

        // TODO: use the path in order to resend smaller areas
        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }

        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_text(RedWorker *worker,
                                   RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceText text;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *back_brush_pat_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TEXT, &dpi->dpi_pipe_item);
    fill_base(base_marshaller, item);
    text = drawable->u.text;
    spice_marshall_Text(base_marshaller,
                        &text,
                        &brush_pat_out,
                        &back_brush_pat_out);

    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, text.fore_brush.u.pattern.pat, item, FALSE);
    }
    if (back_brush_pat_out) {
        fill_bits(dcc, back_brush_pat_out, text.back_brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_text(RedWorker *worker,
                                         RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *drawable = item->red_drawable;
    int fg_is_lossy;
    BitmapData fg_bitmap_data;
    int bg_is_lossy;
    BitmapData bg_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop = 0;

    fg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.fore_brush, item,
                                 &fg_bitmap_data);
    bg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.back_brush, item,
                                 &bg_bitmap_data);

    // assuming that if the brush type is solid, the destination can
    // be lossy, no matter what the rop is.
    if (drawable->u.text.fore_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop = drawable->u.text.fore_mode;
    }

    if (drawable->u.text.back_brush.type != SPICE_BRUSH_TYPE_SOLID) {
        rop |= drawable->u.text.back_mode;
    }

    if ((rop & SPICE_ROPD_OP_OR) || (rop & SPICE_ROPD_OP_AND) ||
        (rop & SPICE_ROPD_OP_XOR)) {
        dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                              &drawable->bbox, &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        (!fg_is_lossy || (fg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!bg_is_lossy || (bg_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_text(worker, rcc, base_marshaller, dpi);
    } else {
        int resend_surface_ids[3];
        SpiceRect *resend_areas[3];
        int num_resend = 0;

        if (fg_is_lossy && (fg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = fg_bitmap_data.id;
            resend_areas[num_resend] = &fg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (bg_is_lossy && (bg_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) {
            resend_surface_ids[num_resend] = bg_bitmap_data.id;
            resend_areas[num_resend] = &bg_bitmap_data.lossy_rect;
            num_resend++;
        }

        if (dest_is_lossy) {
            resend_surface_ids[num_resend] = drawable->surface_id;
            resend_areas[num_resend] = &dest_lossy_area;
            num_resend++;
        }
        red_add_lossless_drawable_dependencies(worker, rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_lossy_marshall_qxl_drawable(RedWorker *worker, RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    switch (item->red_drawable->type) {
    case QXL_DRAW_FILL:
        red_lossy_marshall_qxl_draw_fill(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_lossy_marshall_qxl_draw_opaque(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COPY:
        red_lossy_marshall_qxl_draw_copy(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_lossy_marshall_qxl_draw_transparent(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_lossy_marshall_qxl_draw_alpha_blend(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_COPY_BITS:
        red_lossy_marshall_qxl_copy_bits(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_lossy_marshall_qxl_draw_blend(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_lossy_marshall_qxl_draw_blackness(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_lossy_marshall_qxl_draw_whiteness(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_lossy_marshall_qxl_draw_inverse(worker, rcc, base_marshaller, item);
        break;
    case QXL_DRAW_ROP3:
        red_lossy_marshall_qxl_draw_rop3(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_lossy_marshall_qxl_draw_composite(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_lossy_marshall_qxl_draw_stroke(worker, rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_lossy_marshall_qxl_draw_text(worker, rcc, base_marshaller, dpi);
        break;
    default:
        spice_error("invalid type");
    }
}

static inline void red_marshall_qxl_drawable(RedWorker *worker, RedChannelClient *rcc,
                                SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    switch (drawable->type) {
    case QXL_DRAW_FILL:
        red_marshall_qxl_draw_fill(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_marshall_qxl_draw_opaque(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_COPY:
        red_marshall_qxl_draw_copy(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_marshall_qxl_draw_transparent(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_marshall_qxl_draw_alpha_blend(worker, rcc, m, dpi, FALSE);
        break;
    case QXL_COPY_BITS:
        red_marshall_qxl_copy_bits(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_marshall_qxl_draw_blend(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_marshall_qxl_draw_blackness(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_marshall_qxl_draw_whiteness(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_marshall_qxl_draw_inverse(worker, rcc, m, item);
        break;
    case QXL_DRAW_ROP3:
        red_marshall_qxl_draw_rop3(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_marshall_qxl_draw_stroke(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_marshall_qxl_draw_composite(worker, rcc, m, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_marshall_qxl_draw_text(worker, rcc, m, dpi);
        break;
    default:
        spice_error("invalid type");
    }
}

/* 将type和id添加到dcc发送数据的FREE_LIST中
   sync_data是资源的同步序号
*/
static void display_channel_push_release(DisplayChannelClient *dcc, 
	uint8_t type, uint64_t id, uint64_t* sync_data)
{
    FreeList *free_list = &dcc->send_data.free_list;
    int i;

    for (i = 0; i < MAX_CACHE_CLIENTS; i++) { 
		//更新资源列表的同步序号
        free_list->sync[i] = MAX(free_list->sync[i], sync_data[i]);
    }

	//如果资源列表用光了，增加空间
    if (free_list->res->count == free_list->res_size) {
        SpiceResourceList *new_list;
		//SpiceResourceList满了的时候成指数增加
        new_list = spice_malloc(sizeof(*new_list) +
        	free_list->res_size * sizeof(SpiceResourceID) * 2);
        new_list->count = free_list->res->count;//设置有效ID数量
        memcpy(new_list->resources, free_list->res->resources,
               new_list->count * sizeof(SpiceResourceID));//copy资源ID列表
        free(free_list->res);//释放老内存
        free_list->res = new_list; //指向新内存
        free_list->res_size *= 2; //资源ID容量翻倍
    }
	//添加道资源列表
    free_list->res->resources[free_list->res->count].type = type;
    free_list->res->resources[free_list->res->count++].id = id;
}

static inline void display_marshal_sub_msg_inval_list(SpiceMarshaller *m,
                                                       FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_DISPLAY_INVAL_LIST);
    spice_marshaller_add_uint32(m, sizeof(*free_list->res) +
                                free_list->res->count * sizeof(free_list->res->resources[0]));
    spice_marshall_msg_display_inval_list(m, free_list->res);
}

static inline void display_marshal_sub_msg_inval_list_wait(SpiceMarshaller *m,
                                                            FreeList *free_list)

{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_WAIT_FOR_CHANNELS);
    spice_marshaller_add_uint32(m, sizeof(free_list->wait.header) +
                                free_list->wait.header.wait_count * sizeof(free_list->wait.buf[0]));
    spice_marshall_msg_wait_for_channels(m, &free_list->wait.header);
}

/* use legacy SpiceDataHeader (with sub_list) */
static inline void display_channel_send_free_list_legacy(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    SpiceMarshaller *marshaller;
    int sub_list_len = 1;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    SpiceMarshaller *sub_list_m;

    marshaller = red_channel_client_get_marshaller(rcc);
    inval_m = spice_marshaller_get_submarshaller(marshaller);

    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_list_m = spice_marshaller_get_submarshaller(marshaller);
    spice_marshaller_add_uint16(sub_list_m, sub_list_len);
    if (wait_m) {
        spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(wait_m));
    }
    spice_marshaller_add_uint32(sub_list_m, spice_marshaller_get_offset(inval_m));
    red_channel_client_set_header_sub_list(rcc, spice_marshaller_get_offset(sub_list_m));
}

/* use mini header and SPICE_MSG_LIST */
static inline void display_channel_send_free_list(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;
    int sub_list_len = 1;
    SpiceMarshaller *urgent_marshaller;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    uint32_t sub_arr_offset;
    uint32_t wait_offset = 0;
    uint32_t inval_offset = 0;
    int i;

    urgent_marshaller = red_channel_client_switch_to_urgent_sender(rcc);
    for (i = 0; i < dcc->send_data.num_pixmap_cache_items; i++) {
        int dummy;
        /* When using the urgent marshaller, the serial number of the message that is
         * going to be sent right after the SPICE_MSG_LIST, is increased by one.
         * But all this message pixmaps cache references used its old serial.
         * we use pixmap_cache_items to collect these pixmaps, and we update their serial
         * by calling pixmap_cache_hit. */
        pixmap_cache_hit(dcc->pixmap_cache, dcc->send_data.pixmap_cache_items[i],
                         &dummy, dcc);
    }

    if (free_list->wait.header.wait_count) {
        red_channel_client_init_send_data(rcc, SPICE_MSG_LIST, NULL);
    } else { /* only one message, no need for a list */
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_LIST, NULL);
        spice_marshall_msg_display_inval_list(urgent_marshaller, free_list->res);
        return;
    }

    inval_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    display_marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(urgent_marshaller);
        display_marshal_sub_msg_inval_list_wait(wait_m, free_list);
        sub_list_len++;
    }

    sub_arr_offset = sub_list_len * sizeof(uint32_t);

    spice_marshaller_add_uint16(urgent_marshaller, sub_list_len);
    inval_offset = spice_marshaller_get_offset(inval_m); // calc the offset before
                                                         // adding the sub list
                                                         // offsets array to the marshaller
    /* adding the array of offsets */
    if (wait_m) {
        wait_offset = spice_marshaller_get_offset(wait_m);
        spice_marshaller_add_uint32(urgent_marshaller, wait_offset + sub_arr_offset);
    }
    spice_marshaller_add_uint32(urgent_marshaller, inval_offset + sub_arr_offset);
}

static inline void display_begin_send_message(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    FreeList *free_list = &dcc->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int i;

        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != dcc->common.id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = SPICE_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        free_list->wait.header.wait_count = sync_count;

        if (rcc->is_mini_header) {
            display_channel_send_free_list(rcc);
        } else {
            display_channel_send_free_list_legacy(rcc);
        }
    }
    red_channel_client_begin_send_message(rcc);
}

static inline uint8_t *red_get_image_line(SpiceChunks *chunks, size_t *offset,
                                          int *chunk_nr, int stride)
{
    uint8_t *ret;
    SpiceChunk *chunk;

    chunk = &chunks->chunk[*chunk_nr];

    if (*offset == chunk->len) {
        if (*chunk_nr == chunks->num_chunks - 1) {
            return NULL; /* Last chunk */
        }
        *offset = 0;
        (*chunk_nr)++;
        chunk = &chunks->chunk[*chunk_nr];
    }

    if (chunk->len - *offset < stride) {
        spice_warning("bad chunk alignment");
        return NULL;
    }
    ret = chunk->data + *offset;
    *offset += stride;
    return ret;
}

static int encode_frame(DisplayChannelClient *dcc, const SpiceRect *src,
                        const SpiceBitmap *image, Stream *stream)
{
    SpiceChunks *chunks;
    uint32_t image_stride;
    size_t offset;
    int i, chunk;
    StreamAgent *agent = &dcc->stream_agents[stream - dcc->common.worker->streams_buf];

    chunks = image->data;
    offset = 0;
    chunk = 0;
    image_stride = image->stride;

    const int skip_lines = stream->top_down ? src->top : image->y - (src->bottom - 0);
    for (i = 0; i < skip_lines; i++) {
        red_get_image_line(chunks, &offset, &chunk, image_stride);
    }

    const unsigned int stream_height = src->bottom - src->top;
    const unsigned int stream_width = src->right - src->left;

    for (i = 0; i < stream_height; i++) {
        uint8_t *src_line =
            (uint8_t *)red_get_image_line(chunks, &offset, &chunk, image_stride);

        if (!src_line) {
            return FALSE;
        }

        src_line += src->left * mjpeg_encoder_get_bytes_per_pixel(agent->mjpeg_encoder);
        if (mjpeg_encoder_encode_scanline(agent->mjpeg_encoder, src_line, stream_width) == 0)
            return FALSE;
    }

    return TRUE;
}

static inline int red_marshall_stream_data(RedChannelClient *rcc,
                  SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    Stream *stream = drawable->stream;
    SpiceImage *image;
    RedWorker *worker = dcc->common.worker;
    uint32_t frame_mm_time;
    int n;
    int width, height;
    int ret;

    if (!stream) {
        spice_assert(drawable->sized_stream);
        stream = drawable->sized_stream;
    }
    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);

    worker = display_channel->common.worker;
    image = drawable->red_drawable->u.copy.src_bitmap;

    if (image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP) {
        return FALSE;
    }

    if (drawable->sized_stream) {
        if (red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_SIZED_STREAM)) {
            SpiceRect *src_rect = &drawable->red_drawable->u.copy.src_area;

            width = src_rect->right - src_rect->left;
            height = src_rect->bottom - src_rect->top;
        } else {
            return FALSE;
        }
    } else {
        width = stream->width;
        height = stream->height;
    }

    StreamAgent *agent = &dcc->stream_agents[get_stream_id(worker, stream)];
    uint64_t time_now = red_now();
    size_t outbuf_size;

    if (!dcc->use_mjpeg_encoder_rate_control) {
        if (time_now - agent->last_send_time < (1000 * 1000 * 1000) / agent->fps) {
            agent->frames--;
#ifdef STREAM_STATS
            agent->stats.num_drops_fps++;
#endif
            return TRUE;
        }
    }

    /* workaround for vga streams */
    frame_mm_time =  drawable->red_drawable->mm_time ?
                        drawable->red_drawable->mm_time :
                        reds_get_mm_time();
    outbuf_size = dcc->send_data.stream_outbuf_size;
    ret = mjpeg_encoder_start_frame(agent->mjpeg_encoder, image->u.bitmap.format,
                                    width, height,
                                    &dcc->send_data.stream_outbuf,
                                    &outbuf_size,
                                    frame_mm_time);
    switch (ret) {
    case MJPEG_ENCODER_FRAME_DROP:
        spice_assert(dcc->use_mjpeg_encoder_rate_control);
#ifdef STREAM_STATS
        agent->stats.num_drops_fps++;
#endif
        return TRUE;
    case MJPEG_ENCODER_FRAME_UNSUPPORTED:
        return FALSE;
    case MJPEG_ENCODER_FRAME_ENCODE_START:
        break;
    default:
        spice_error("bad return value (%d) from mjpeg_encoder_start_frame", ret);
        return FALSE;
    }

    if (!encode_frame(dcc, &drawable->red_drawable->u.copy.src_area,
                      &image->u.bitmap, stream)) {
        return FALSE;
    }
    n = mjpeg_encoder_end_frame(agent->mjpeg_encoder);
    dcc->send_data.stream_outbuf_size = outbuf_size;

    if (!drawable->sized_stream) {
        SpiceMsgDisplayStreamData stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA, NULL);

        stream_data.base.id = get_stream_id(worker, stream);
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = n;

        spice_marshall_msg_display_stream_data(base_marshaller, &stream_data);
    } else {
        SpiceMsgDisplayStreamDataSized stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA_SIZED, NULL);

        stream_data.base.id = get_stream_id(worker, stream);
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = n;
        stream_data.width = width;
        stream_data.height = height;
        stream_data.dest = drawable->red_drawable->bbox;

        spice_debug("stream %d: sized frame: dest ==> ", stream_data.base.id);
        rect_debug(&stream_data.dest);
        spice_marshall_msg_display_stream_data_sized(base_marshaller, &stream_data);
    }
    spice_marshaller_add_ref(base_marshaller,
                             dcc->send_data.stream_outbuf, n);
    agent->last_send_time = time_now;
#ifdef STREAM_STATS
    agent->stats.num_frames_sent++;
    agent->stats.size_sent += n;
    agent->stats.end = frame_mm_time;
#endif

    return TRUE;
}

static inline void marshall_qxl_drawable(RedChannelClient *rcc,
    SpiceMarshaller *m, DrawablePipeItem *dpi)
{
    Drawable *draw = dpi->drawable;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);

    spice_assert(display_channel && rcc);
    /* allow sized frames to be streamed, even if they where replaced by another frame, since
     * newer frames might not cover sized frames completely if they are bigger */
    if ((draw->stream || draw->sized_stream) && red_marshall_stream_data(rcc, m, draw)) {
        return;
    }
    if (!display_channel->enable_jpeg)
        red_marshall_qxl_drawable(display_channel->common.worker, rcc, m, dpi);
    else
        red_lossy_marshall_qxl_drawable(display_channel->common.worker, rcc, m, dpi);
}

static inline void red_marshall_verb(RedChannelClient *rcc, uint16_t verb)
{
    spice_assert(rcc);
    red_channel_client_init_send_data(rcc, verb, NULL);
}

static inline void red_marshall_inval(RedChannelClient *rcc,
        SpiceMarshaller *base_marshaller, CacheItem *cach_item)
{
    SpiceMsgDisplayInvalOne inval_one;

    red_channel_client_init_send_data(rcc, cach_item->inval_type, NULL);
    inval_one.id = *(uint64_t *)&cach_item->id;

    spice_marshall_msg_cursor_inval_one(base_marshaller, &inval_one);
}

static void display_channel_marshall_migrate_data_surfaces(DisplayChannelClient *dcc,
                                                           SpiceMarshaller *m,
                                                           int lossy)
{
    SpiceMarshaller *m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    uint32_t *num_surfaces_created;
    uint32_t i;

    num_surfaces_created = (uint32_t *)spice_marshaller_reserve_space(m2, sizeof(uint32_t));
    *num_surfaces_created = 0;
    for (i = 0; i < NUM_SURFACES; i++) {
        SpiceRect lossy_rect;

        if (!dcc->surface_client_created[i]) {
            continue;
        }
        spice_marshaller_add_uint32(m2, i);
        (*num_surfaces_created)++;

        if (!lossy) {
            continue;
        }
        region_extents(&dcc->surface_client_lossy_region[i], &lossy_rect);
        spice_marshaller_add_int32(m2, lossy_rect.left);
        spice_marshaller_add_int32(m2, lossy_rect.top);
        spice_marshaller_add_int32(m2, lossy_rect.right);
        spice_marshaller_add_int32(m2, lossy_rect.bottom);
    }
}

static void display_channel_marshall_migrate_data(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMigrateDataDisplay display_data = {0,};

    display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, NULL);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_MAGIC);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_VERSION);

    spice_assert(dcc->pixmap_cache);
    spice_assert(MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == 4 &&
                 MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == MAX_CACHE_CLIENTS);

    display_data.message_serial = red_channel_client_get_message_serial(rcc);
    display_data.low_bandwidth_setting = dcc->common.is_low_bandwidth;

    display_data.pixmap_cache_freezer = pixmap_cache_freeze(dcc->pixmap_cache);
    display_data.pixmap_cache_id = dcc->pixmap_cache->id;
    display_data.pixmap_cache_size = dcc->pixmap_cache->size;
    memcpy(display_data.pixmap_cache_clients, dcc->pixmap_cache->sync,
           sizeof(display_data.pixmap_cache_clients));

    spice_assert(dcc->glz_dict);
    red_freeze_glz(dcc);
    display_data.glz_dict_id = dcc->glz_dict->id;
    glz_enc_dictionary_get_restore_data(dcc->glz_dict->dict,
                                        &display_data.glz_dict_data,
                                        &dcc->glz_data.usr);

    /* all data besided the surfaces ref */
    spice_marshaller_add(base_marshaller,
                         (uint8_t *)&display_data, sizeof(display_data) - sizeof(uint32_t));
    display_channel_marshall_migrate_data_surfaces(dcc, base_marshaller,
                                                   display_channel->enable_jpeg);
}

static void display_channel_marshall_pixmap_sync(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;
    PixmapCache *pixmap_cache;

    red_channel_client_init_send_data(rcc, SPICE_MSG_WAIT_FOR_CHANNELS, NULL);
    pixmap_cache = dcc->pixmap_cache;

    pthread_mutex_lock(&pixmap_cache->lock);

    wait.wait_count = 1;
    wait.wait_list[0].channel_type = SPICE_CHANNEL_DISPLAY;
    wait.wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait.wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    dcc->pixmap_cache_generation = pixmap_cache->generation;
    dcc->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    spice_marshall_msg_wait_for_channels(base_marshaller, &wait);
}

static void display_channel_marshall_reset_cache(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgWaitForChannels wait;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS, NULL);
    pixmap_cache_reset(dcc->pixmap_cache, dcc, &wait);

    spice_marshall_msg_display_inval_all_pixmaps(base_marshaller,
                                                 &wait);
}

/* 将SURFACE图像调制成DRAW_COPY消息 */
static void red_marshall_image(RedChannelClient *rcc, SpiceMarshaller *m, ImageItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    SpiceImage red_image;
    RedWorker *worker;
    SpiceBitmap bitmap;
    SpiceChunks *chunks;
    QRegion *surface_lossy_region;
    int comp_succeeded;
    int lossy_comp = FALSE;
    int lz_comp = FALSE;
    spice_image_compression_t comp_mode;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    spice_assert(rcc && display_channel && item);
    worker = display_channel->common.worker;

	//设置SpiceImageDescriptor
    QXL_SET_IMAGE_ID(&red_image, QXL_IMAGE_GROUP_RED, ++worker->bits_unique);
    red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP; //位图
    red_image.descriptor.flags = item->image_flags; //复制图像标记
    red_image.descriptor.width = item->width; //复制宽
    red_image.descriptor.height = item->height; //复制高

	//设置SpiceBitmap
    bitmap.format = item->image_format; //格式
    bitmap.flags = 0; //图像数据默认布局
    if (item->top_down) { //添加
        bitmap.flags |= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride; //stride应该>0
    bitmap.palette = 0; //没有调色板
    bitmap.palette_id = 0; //调色板id为0

	//造一个SpiceChunks，但是没有copy图像内容
    chunks = spice_chunks_new_linear(item->data, bitmap.stride * bitmap.y);
    bitmap.data = chunks;

	//构建DRAW_COPY
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->link);

    copy.base.surface_id = item->surface_id; //目标SURFACE
    //目标区域
    copy.base.box.left = item->pos.x; 
    copy.base.box.top = item->pos.y;
    copy.base.box.right = item->pos.x + bitmap.x;
    copy.base.box.bottom = item->pos.y + bitmap.y;
	//没有裁剪，所以不用设置裁剪矩形集合了
    copy.base.clip.type = SPICE_CLIP_TYPE_NONE;
	
    copy.data.rop_descriptor = SPICE_ROPD_OP_PUT; //直接渲染，
    //设置数据原始区域
    copy.data.src_area.left = 0; 
    copy.data.src_area.top = 0;
    copy.data.src_area.right = bitmap.x;
    copy.data.src_area.bottom = bitmap.y;
	//设置copy数据的缩放模式
    copy.data.scale_mode = 0; //不缩放
    //原始数据图像
    copy.data.src_bitmap = 0; //没有源图像
    //设置copy掩码图像
    copy.data.mask.flags = 0; //无掩码标记
    copy.data.mask.pos.x = 0; //掩码图像坐标
    copy.data.mask.pos.y = 0;
    copy.data.mask.bitmap = 0; //没有掩码图像

	//将SpiceMsgDisplayDrawCopy消息调制到m中，src_bitmap_out返回原图调制器
	//mask_bitmap_out返回drawcopy中mask的调制器，此时原图像调制器和mask图像
	//调制器还没准备好数据
    spice_marshall_msg_display_draw_copy(m, &copy,
    	&src_bitmap_out, &mask_bitmap_out);

	// 进行压缩
    compress_send_data_t comp_send_data = {0};

	// 获取图像压缩方式
    comp_mode = display_channel->common.worker->image_compression;

	// 有损、无损决策，默认无损
    if (((comp_mode == SPICE_IMAGE_COMPRESS_AUTO_LZ) ||
        (comp_mode == SPICE_IMAGE_COMPRESS_AUTO_GLZ)) //自动LZ或者自动GLZ压缩
        && !_stride_is_extra(&bitmap)) //图像必须没有添加数据
    {

        if (BITMAP_FMT_HAS_GRADUALITY(item->image_format)) {
            BitmapGradualType grad_level;

            grad_level = _get_bitmap_graduality_level(display_channel->common.worker,
                                                      &bitmap,
                                                      worker->mem_slots.internal_groupslot_id);
            if (grad_level == BITMAP_GRADUAL_HIGH) {
                // if we use lz for alpha, the stride can't be extra
                lossy_comp = display_channel->enable_jpeg && item->can_lossy;
            } else {
                lz_comp = TRUE;
            }
        } else {
            lz_comp = TRUE;
        }
    }

    if (lossy_comp) {
        comp_succeeded = red_jpeg_compress_image(dcc, &red_image,
                                                 &bitmap, &comp_send_data,
                                                 worker->mem_slots.internal_groupslot_id);
    } else {
        if (!lz_comp) {
            comp_succeeded = red_quic_compress_image(dcc, &red_image, &bitmap,
                                                     &comp_send_data,
                                                     worker->mem_slots.internal_groupslot_id);
        } else {
            comp_succeeded = red_lz_compress_image(dcc, &red_image, &bitmap,
                                                   &comp_send_data,
                                                   worker->mem_slots.internal_groupslot_id);
        }
    }

    surface_lossy_region = &dcc->surface_client_lossy_region[item->surface_id];
    if (comp_succeeded) {
        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);

        marshaller_add_compressed(src_bitmap_out,
                                  comp_send_data.comp_buf, comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        if (lossy_comp) {
            region_add(surface_lossy_region, &copy.base.box);
        } else {
            region_remove(surface_lossy_region, &copy.base.box);
        }
    } else {
        red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        red_image.u.bitmap = bitmap;

        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_marshaller_add_ref(src_bitmap_out, item->data,
                                 bitmap.y * bitmap.stride);
        region_remove(surface_lossy_region, &copy.base.box);
    }
    spice_chunks_destroy(chunks);
}

// 调制surface更新管道消息
static void red_display_marshall_upgrade(RedChannelClient *rcc, SpiceMarshaller *m,
                                         UpgradeItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    RedDrawable *red_drawable;
    SpiceMsgDisplayDrawCopy copy; //实际是DrawCopy消息
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;

    spice_assert(rcc && rcc->channel && item && item->drawable);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY, &item->base);

    red_drawable = item->drawable->red_drawable; 
    spice_assert(red_drawable->type == QXL_DRAW_COPY);//DRAW_COPY才能upgrade
    spice_assert(red_drawable->u.copy.rop_descriptor == SPICE_ROPD_OP_PUT); //直接PUT才能upgrade
    spice_assert(red_drawable->u.copy.mask.bitmap == 0); //没有SpiceQMask

    copy.base.surface_id = 0; //更新主surface
    copy.base.box = red_drawable->bbox; //取渲染命令的目标区域
    copy.base.clip.type = SPICE_CLIP_TYPE_RECTS; //矩形裁剪
    copy.base.clip.rects = item->rects; //UpdradeItem的rects会变成drawcopy的目标裁剪区域
    copy.data = red_drawable->u.copy; 
	
    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

	//mask_bitmap_out必定为空，所以不用填充mask
    fill_bits(dcc, src_bitmap_out, copy.data.src_bitmap, item->drawable, FALSE);
}

static void red_display_marshall_stream_start(RedChannelClient *rcc,
                     SpiceMarshaller *base_marshaller, StreamAgent *agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    Stream *stream = agent->stream;

    agent->last_send_time = 0;
    spice_assert(stream);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CREATE, NULL);
    SpiceMsgDisplayStreamCreate stream_create;
    SpiceClipRects clip_rects;

    stream_create.surface_id = 0;
    stream_create.id = get_stream_id(dcc->common.worker, stream);
    stream_create.flags = stream->top_down ? SPICE_STREAM_FLAGS_TOP_DOWN : 0;
    stream_create.codec_type = SPICE_VIDEO_CODEC_TYPE_MJPEG;

    stream_create.src_width = stream->width;
    stream_create.src_height = stream->height;
    stream_create.stream_width = stream_create.src_width;
    stream_create.stream_height = stream_create.src_height;
    stream_create.dest = stream->dest_area;

    if (stream->current) {
        RedDrawable *red_drawable = stream->current->red_drawable;
        stream_create.clip = red_drawable->clip;
    } else {
        stream_create.clip.type = SPICE_CLIP_TYPE_RECTS;
        clip_rects.num_rects = 0;
        stream_create.clip.rects = &clip_rects;
    }

    stream_create.stamp = 0;

    spice_marshall_msg_display_stream_create(base_marshaller, &stream_create);
}

// 调制流裁剪管道项命令，让客户端对指定id的流重新进行裁剪。
// 这里要求客户端和服务端的流id一一对应
static void red_display_marshall_stream_clip(RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         StreamClipItem *item)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    StreamAgent *agent = item->stream_agent;

    spice_assert(agent->stream);

	//设置流裁剪命令
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CLIP, &item->base);
    SpiceMsgDisplayStreamClip stream_clip;

    stream_clip.id = get_stream_id(dcc->common.worker, agent->stream);
    stream_clip.clip.type = item->clip_type;
    stream_clip.clip.rects = item->rects;

    spice_marshall_msg_display_stream_clip(base_marshaller, &stream_clip);
}

static void red_display_marshall_stream_end(RedChannelClient *rcc,
                   SpiceMarshaller *base_marshaller, StreamAgent* agent)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgDisplayStreamDestroy destroy;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DESTROY, NULL);
    destroy.id = get_stream_id(dcc->common.worker, agent->stream);
    red_display_stream_agent_stop(dcc, agent);
    spice_marshall_msg_display_stream_destroy(base_marshaller, &destroy);
}

static void red_cursor_marshall_inval(RedChannelClient *rcc,
                SpiceMarshaller *m, CacheItem *cach_item)
{
    spice_assert(rcc);
    red_marshall_inval(rcc, m, cach_item);
}

static void red_marshall_cursor_init(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                     PipeItem *pipe_item)
{
    CursorChannel *cursor_channel;
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);
    RedWorker *worker;
    SpiceMsgCursorInit msg;
    AddBufInfo info;

    spice_assert(rcc);
    cursor_channel = SPICE_CONTAINEROF(rcc->channel, CursorChannel, common.base);
    worker = cursor_channel->common.worker;

    red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_INIT, NULL);
    msg.visible = worker->cursor_visible;
    msg.position = worker->cursor_position;
    msg.trail_length = worker->cursor_trail_length;
    msg.trail_frequency = worker->cursor_trail_frequency;

    fill_cursor(ccc, &msg.cursor, worker->cursor, &info);
    spice_marshall_msg_cursor_init(base_marshaller, &msg);
    add_buf_from_info(base_marshaller, &info);
}

static void red_marshall_cursor(RedChannelClient *rcc,
                   SpiceMarshaller *m, CursorPipeItem *cursor_pipe_item)
{
    CursorChannel *cursor_channel = SPICE_CONTAINEROF(rcc->channel, CursorChannel, common.base);
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);
    CursorItem *cursor = cursor_pipe_item->cursor_item;
    PipeItem *pipe_item = &cursor_pipe_item->base;
    RedCursorCmd *cmd;
    RedWorker *worker;

    spice_assert(cursor_channel);

    worker = cursor_channel->common.worker;

    cmd = cursor->red_cursor;
    switch (cmd->type) {
    case QXL_CURSOR_MOVE:
        {
            SpiceMsgCursorMove cursor_move;
            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_MOVE, pipe_item);
            cursor_move.position = cmd->u.position;
            spice_marshall_msg_cursor_move(m, &cursor_move);
            break;
        }
    case QXL_CURSOR_SET:
        {
            SpiceMsgCursorSet cursor_set;
            AddBufInfo info;

            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_SET, pipe_item);
            cursor_set.position = cmd->u.set.position;
            cursor_set.visible = worker->cursor_visible;

            fill_cursor(ccc, &cursor_set.cursor, cursor, &info);
            spice_marshall_msg_cursor_set(m, &cursor_set);
            add_buf_from_info(m, &info);
            break;
        }
    case QXL_CURSOR_HIDE:
        red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_HIDE, pipe_item);
        break;
    case QXL_CURSOR_TRAIL:
        {
            SpiceMsgCursorTrail cursor_trail;

            red_channel_client_init_send_data(rcc, SPICE_MSG_CURSOR_TRAIL, pipe_item);
            cursor_trail.length = cmd->u.trail.length;
            cursor_trail.frequency = cmd->u.trail.frequency;
            spice_marshall_msg_cursor_trail(m, &cursor_trail);
        }
        break;
    default:
        spice_error("bad cursor command %d", cmd->type);
    }
}

// 序列化DCC的surface创建消息
static void red_marshall_surface_create(RedChannelClient *rcc,
    SpiceMarshaller *base_marshaller, SpiceMsgSurfaceCreate *surface_create)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

	// 在创建surface消息发送时初始化surface客户端的有损区域
    region_init(&dcc->surface_client_lossy_region[surface_create->surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_CREATE, NULL);

    spice_marshall_msg_display_surface_create(base_marshaller, surface_create);
}

// 序列化DCC的surface销毁消息
static void red_marshall_surface_destroy(RedChannelClient *rcc,
       SpiceMarshaller *base_marshaller, uint32_t surface_id)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    SpiceMsgSurfaceDestroy surface_destroy;

	// 在销毁surface消息发送时，销毁surface的客户端有损区域
    region_destroy(&dcc->surface_client_lossy_region[surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_DESTROY, NULL);

    surface_destroy.surface_id = surface_id;

    spice_marshall_msg_display_surface_destroy(base_marshaller, &surface_destroy);
}

static void red_marshall_monitors_config(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                         MonitorsConfig *monitors_config)
{
    int heads_size = sizeof(SpiceHead) * monitors_config->count;
    int i;
    SpiceMsgDisplayMonitorsConfig *msg = spice_malloc0(sizeof(*msg) + heads_size);
    int count = 0; // ignore monitors_config->count, it may contain zero width monitors, remove them now

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_MONITORS_CONFIG, NULL);
    for (i = 0 ; i < monitors_config->count; ++i) {
        if (monitors_config->heads[i].width == 0 || monitors_config->heads[i].height == 0) {
            continue;
        }
        msg->heads[count].id = monitors_config->heads[i].id;
        msg->heads[count].surface_id = monitors_config->heads[i].surface_id;
        msg->heads[count].width = monitors_config->heads[i].width;
        msg->heads[count].height = monitors_config->heads[i].height;
        msg->heads[count].x = monitors_config->heads[i].x;
        msg->heads[count].y = monitors_config->heads[i].y;
        count++;
    }
    msg->count = count;
    msg->max_allowed = monitors_config->max_allowed;
    spice_marshall_msg_display_monitors_config(base_marshaller, msg);
    free(msg);
}

static void red_marshall_stream_activate_report(RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                uint32_t stream_id)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    StreamAgent *agent = &dcc->stream_agents[stream_id];
    SpiceMsgDisplayStreamActivateReport msg;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT, NULL);
    msg.stream_id = stream_id;
    msg.unique_id = agent->report_id;
    msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
    msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
    spice_marshall_msg_display_stream_activate_report(base_marshaller, &msg);
}

static void display_channel_send_item(RedChannelClient *rcc, PipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    red_display_reset_send_data(dcc);
    switch (pipe_item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(pipe_item, DrawablePipeItem, dpi_pipe_item);
        marshall_qxl_drawable(rcc, m, dpi);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
        red_marshall_inval(rcc, m, (CacheItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, create_item);
        red_display_marshall_stream_start(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP: {
        StreamClipItem* clip_item = (StreamClipItem *)pipe_item;
        red_display_marshall_stream_clip(rcc, m, clip_item);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(pipe_item, StreamAgent, destroy_item);
        red_display_marshall_stream_end(rcc, m, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        red_display_marshall_upgrade(rcc, m, (UpgradeItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        red_marshall_verb(rcc, ((VerbItem*)pipe_item)->verb);
        break;
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
        display_channel_marshall_migrate_data(rcc, m);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        red_marshall_image(rcc, m, (ImageItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
        display_channel_marshall_pixmap_sync(rcc, m);
        break;
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
        display_channel_marshall_reset_cache(rcc, m);
        break;
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
        red_reset_palette_cache(dcc);
        red_marshall_verb(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(pipe_item, SurfaceCreateItem,
                                                              pipe_item);
        red_marshall_surface_create(rcc, m, &surface_create->surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(pipe_item, SurfaceDestroyItem,
                                                                pipe_item);
        red_marshall_surface_destroy(rcc, m, surface_destroy->surface_destroy.surface_id);
        break;
    }
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(pipe_item,
                                                             MonitorsConfigItem, pipe_item);
        red_marshall_monitors_config(rcc, m, monconf_item->monitors_config);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        StreamActivateReportItem *report_item = SPICE_CONTAINEROF(pipe_item,
                                                                  StreamActivateReportItem,
                                                                  pipe_item);
        red_marshall_stream_activate_report(rcc, m, report_item->stream_id);
        break;
    }
    default:
        spice_error("invalid pipe item type");
    }

    display_channel_client_release_item_before_push(dcc, pipe_item);

    // a message is pending
    if (red_channel_client_send_message_pending(rcc)) {
        display_begin_send_message(rcc);
    }
}

static void cursor_channel_send_item(RedChannelClient *rcc, PipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);

    switch (pipe_item->type) {
    case PIPE_ITEM_TYPE_CURSOR:
        red_marshall_cursor(rcc, m, SPICE_CONTAINEROF(pipe_item, CursorPipeItem, base));
        break;
    case PIPE_ITEM_TYPE_INVAL_ONE:
        red_cursor_marshall_inval(rcc, m, (CacheItem *)pipe_item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        red_marshall_verb(rcc, ((VerbItem*)pipe_item)->verb);
        break;
    case PIPE_ITEM_TYPE_CURSOR_INIT:
        red_reset_cursor_cache(rcc);
        red_marshall_cursor_init(rcc, m, pipe_item);
        break;
    case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
        red_reset_cursor_cache(rcc);
        red_marshall_verb(rcc, SPICE_MSG_CURSOR_INVAL_ALL);
        break;
    default:
        spice_error("invalid pipe item type");
    }

    cursor_channel_client_release_item_before_push(ccc, pipe_item);
    red_channel_client_begin_send_message(rcc);
}

static inline void red_push(RedWorker *worker)
{
    if (worker->cursor_channel) {
        red_channel_push(&worker->cursor_channel->common.base);
    }
    if (worker->display_channel) {
        red_channel_push(&worker->display_channel->common.base);
    }
}

typedef struct ShowTreeData {
    RedWorker *worker;
    int level;
    Container *container;
} ShowTreeData;

static void __show_tree_call(TreeItem *item, void *data)
{
    ShowTreeData *tree_data = data;
    const char *item_prefix = "|--";
    int i;

    while (tree_data->container != item->container) {
        spice_assert(tree_data->container);
        tree_data->level--;
        tree_data->container = tree_data->container->base.container;
    }

    switch (item->type) {
    case TREE_ITEM_TYPE_DRAWABLE: {
        Drawable *drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
        const int max_indent = 200;
        char indent_str[max_indent + 1];
        int indent_str_len;

        for (i = 0; i < tree_data->level; i++) {
            printf("  ");
        }
        printf(item_prefix, 0);
        show_red_drawable(tree_data->worker, drawable->red_drawable, NULL);
        for (i = 0; i < tree_data->level; i++) {
            printf("  ");
        }
        printf("|  ");
        show_draw_item(tree_data->worker, &drawable->tree_item, NULL);
        indent_str_len = MIN(max_indent, strlen(item_prefix) + tree_data->level * 2);
        memset(indent_str, ' ', indent_str_len);
        indent_str[indent_str_len] = 0;
        region_dump(&item->rgn, indent_str);
        printf("\n");
        break;
    }
    case TREE_ITEM_TYPE_CONTAINER:
        tree_data->level++;
        tree_data->container = (Container *)item;
        break;
    case TREE_ITEM_TYPE_SHADOW:
        break;
    }
}

void red_show_tree(RedWorker *worker)
{
    int x;

    ShowTreeData show_tree_data;
    show_tree_data.worker = worker;
    show_tree_data.level = 0;
    show_tree_data.container = NULL;
    for (x = 0; x < NUM_SURFACES; ++x) {
        if (worker->surfaces[x].context.canvas) {
            current_tree_for_each(&worker->surfaces[x].current, __show_tree_call,
                                  &show_tree_data);
        }
    }
}

static void display_channel_client_on_disconnect(RedChannelClient *rcc)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    CommonChannel *common;
    RedWorker *worker;

    if (!rcc) {
        return;
    }
    spice_info(NULL);
    common = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base);
    worker = common->worker;
    display_channel = (DisplayChannel *)rcc->channel;
    spice_assert(display_channel == worker->display_channel);
#ifdef COMPRESS_STAT
    print_compress_stats(display_channel);
#endif
    red_release_pixmap_cache(dcc);
    red_release_glz(dcc);
    red_reset_palette_cache(dcc);
    free(dcc->send_data.stream_outbuf);
    red_display_reset_compress_buf(dcc);
    free(dcc->send_data.free_list.res);
    red_display_destroy_streams_agents(dcc);

    // this was the last channel client
    if (!red_channel_is_connected(rcc->channel)) {
        red_display_destroy_compress_bufs(display_channel);
    }
    spice_debug("#draw=%d, #red_draw=%d, #glz_draw=%d",
                worker->drawable_count, worker->red_drawable_count,
                worker->glz_drawable_count);
}

void red_disconnect_all_display_TODO_remove_me(RedChannel *channel)
{
    // TODO: we need to record the client that actually causes the timeout. So
    // we need to check the locations of the various pipe heads when counting,
    // and disconnect only those/that.
    if (!channel) {
        return;
    }
    red_channel_apply_clients(channel, red_channel_client_disconnect);
}

static void red_destroy_streams(RedWorker *worker)
{
    RingItem *stream_item;

    spice_debug(NULL);
    while ((stream_item = ring_get_head(&worker->streams))) {
        Stream *stream = SPICE_CONTAINEROF(stream_item, Stream, link);

        red_detach_stream_gracefully(worker, stream, NULL);
        red_stop_stream(worker, stream);
    }
}

static void red_migrate_display(RedWorker *worker, RedChannelClient *rcc)
{
    /* We need to stop the streams, and to send upgrade_items to the client.
     * Otherwise, (1) the client might display lossy regions that we don't track
     * (streams are not part of the migration data) (2) streams_timeout may occur
     * after the MIGRATE message has been sent. This can result in messages
     * being sent to the client after MSG_MIGRATE and before MSG_MIGRATE_DATA (e.g.,
     * STREAM_CLIP, STREAM_DESTROY, DRAW_COPY)
     * No message besides MSG_MIGRATE_DATA should be sent after MSG_MIGRATE.
     * Notice that red_destroy_streams won't lead to any dev ram changes, since
     * handle_dev_stop already took care of releasing all the dev ram resources.
     */
    red_destroy_streams(worker);
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_default_migrate(rcc);
    }
}

#ifdef USE_OPENGL
static SpiceCanvas *create_ogl_context_common(RedWorker *worker, OGLCtx *ctx, uint32_t width,
                                              uint32_t height, int32_t stride, uint8_t depth)
{
    SpiceCanvas *canvas;

    oglctx_make_current(ctx);
    if (!(canvas = gl_canvas_create(width, height, depth, &worker->image_cache.base,
                                    &worker->image_surfaces, NULL, NULL, NULL))) {
        return NULL;
    }

    spice_canvas_set_usr_data(canvas, ctx, (spice_destroy_fn_t)oglctx_destroy);

    canvas->ops->clear(canvas);

    return canvas;
}

static SpiceCanvas *create_ogl_pbuf_context(RedWorker *worker, uint32_t width, uint32_t height,
                                         int32_t stride, uint8_t depth)
{
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pbuf_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(worker, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}

static SpiceCanvas *create_ogl_pixmap_context(RedWorker *worker, uint32_t width, uint32_t height,
                                              int32_t stride, uint8_t depth) {
    OGLCtx *ctx;
    SpiceCanvas *canvas;

    if (!(ctx = pixmap_create(width, height))) {
        return NULL;
    }

    if (!(canvas = create_ogl_context_common(worker, ctx, width, height, stride, depth))) {
        oglctx_destroy(ctx);
        return NULL;
    }

    return canvas;
}
#endif

/* 为Surface创建画布，支持三种类型的渲染方式， 但VDI里只使用了SW渲染 */
static inline void *create_canvas_for_surface(RedWorker *worker, RedSurface *surface,
                                              uint32_t renderer, uint32_t width, uint32_t height,
                                              int32_t stride, uint32_t format, void *line_0)
{
    SpiceCanvas *canvas;

    switch (renderer) {
    case RED_RENDERER_SW: //VDI服务端渲染的方式
        canvas = canvas_create_for_data(width, height, format,
        	line_0, stride, &worker->image_cache.base,
        	&worker->image_surfaces, NULL, NULL, NULL);
        surface->context.top_down = TRUE;
        surface->context.canvas_draws_on_surface = TRUE;
        return canvas;
#ifdef USE_OPENGL //VDI服务端编译时没有支持
    case RED_RENDERER_OGL_PBUF:
        canvas = create_ogl_pbuf_context(worker, width, height, stride,
                                         SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
    case RED_RENDERER_OGL_PIXMAP:
        canvas = create_ogl_pixmap_context(worker, width, height, stride,
                                           SPICE_SURFACE_FMT_DEPTH(format));
        surface->context.top_down = FALSE;
        return canvas;
#endif
    default:
        spice_error("invalid renderer type");
    };

    return NULL;
}

//生成SurfaceCreateItem管道项
static SurfaceCreateItem *get_surface_create_item(
    RedChannel* channel,
    uint32_t surface_id, uint32_t width,
    uint32_t height, uint32_t format, uint32_t flags)
{
    SurfaceCreateItem *create;

    create = (SurfaceCreateItem *)malloc(sizeof(SurfaceCreateItem));
    spice_warn_if(!create);

    create->surface_create.surface_id = surface_id;
    create->surface_create.width = width;
    create->surface_create.height = height;
    create->surface_create.flags = flags;
    create->surface_create.format = format;

	//设置pipeitem消息类型
    red_channel_pipe_item_init(channel,
            &create->pipe_item, PIPE_ITEM_TYPE_CREATE_SURFACE);
    return create;
}

// 给一个DCC发送SURFACE创建消息
static inline void red_create_surface_item(DisplayChannelClient *dcc, int surface_id)
{
    RedSurface *surface;
    SurfaceCreateItem *create;
	
    RedWorker *worker = dcc ? dcc->common.worker : NULL;//是否有Work
	
    uint32_t flags = is_primary_surface(worker, surface_id) ? 
		SPICE_SURFACE_FLAGS_PRIMARY : 0; //surface_id == 0为主surface

    /* don't send redundant create surface commands to client */
    if (!dcc || //dcc为空
		worker->display_channel->common.during_target_migrate || //正在迁移
        dcc->surface_client_created[surface_id]) //已经发送过了
    {
        return;
    }
    surface = &worker->surfaces[surface_id];
	//构建SurfaceCreateItem
    create = get_surface_create_item(dcc->common.base.channel,
            surface_id, surface->context.width, surface->context.height,
            surface->context.format, flags);
	//设置标记表示已经给客户端发送了指定surface_id的创建消息
    dcc->surface_client_created[surface_id] = TRUE;
	//将命令放入管道
    red_channel_client_pipe_add(&dcc->common.base, &create->pipe_item);
}

// 给一个显示通道的所有DCC发送SURFACE创建消息
static void red_worker_create_surface_item(RedWorker *worker, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        red_create_surface_item(dcc, surface_id);
    }
}


static void red_worker_push_surface_image(RedWorker *worker, int surface_id)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        red_push_surface_image(dcc, surface_id);
    }
}

/**
 * red_create_surface - 构建一个Surface，通过worker的surfaces返回
 * worker 显示通道RedWorker
 * surface_id Surface的ID
 * width Surface的宽
 * height Surface的高
 * stride Surface的行长
 * format 数据填充格式
 * line_0 Surface的数据填充地址
 * data_is_valid data的地址是否合法
 * send_client 是否发送Surface创建命令给客户端
**/
static inline void red_create_surface(
	RedWorker *worker, uint32_t surface_id, 
	uint32_t width, uint32_t height, int32_t stride, 
	uint32_t format, void *line_0, 
	int data_is_valid, int send_client)
{
    RedSurface *surface = &worker->surfaces[surface_id];
    uint32_t i;

	// 如果有SpiceCanvas了，则为重复创建
    spice_warn_if(surface->context.canvas);

    surface->context.canvas_draws_on_surface = FALSE;

	// 设置宽、高、行长、格式、画布缓冲区地址
    surface->context.width = width;
    surface->context.height = height;
    surface->context.format = format; //默认32
    surface->context.stride = stride; //stride默认负值,windows 1920*1080情况下为-7680，
    surface->context.line_0 = line_0;
	//没有设置DrawContext的top_down
	
    if (!data_is_valid) {//这段没看懂
        memset((char *)line_0 + (int32_t)(stride * (height - 1)), 0, height*abs(stride));
    }
    surface->create.info = NULL;
    surface->destroy.info = NULL;
    ring_init(&surface->current); 
    ring_init(&surface->current_list);
    ring_init(&surface->depend_on_me);
    region_init(&surface->draw_dirty_region);
    surface->refs = 1;
    if (worker->renderer != RED_RENDERER_INVALID) {// 如果设置了统一的渲染方式
		// 为Surface绘画上下文创建SpiceCanvas
        surface->context.canvas = create_canvas_for_surface(
        	worker, surface, worker->renderer, width, height, 
        	stride, surface->context.format, line_0);
		
        if (!surface->context.canvas) { //SW画布创建失败
            spice_critical("drawing canvas creating failed - "
				"can`t create same type canvas");
			//没做其他异常处理
        }

        if (send_client) { //要发送命令给客户端
        	//发送给所有的DCC
            red_worker_create_surface_item(worker, surface_id);
            if (data_is_valid) {
                red_worker_push_surface_image(worker, surface_id);
            }
        }
        return;
    }
	
	// 如果各个surface的渲染方式都不一致
    for (i = 0; i < worker->num_renderers; i++) {
        surface->context.canvas = create_canvas_for_surface(worker, surface, worker->renderers[i],
                                                            width, height, stride,
                                                            surface->context.format, line_0);
        if (surface->context.canvas) { //no need canvas check
            worker->renderer = worker->renderers[i];
            if (send_client) {
                red_worker_create_surface_item(worker, surface_id);
                if (data_is_valid) {
                    red_worker_push_surface_image(worker, surface_id);
                }
            }
            return;
        }
    }

    spice_critical("unable to create drawing canvas");
}

static inline void flush_display_commands(RedWorker *worker)
{
    RedChannel *display_red_channel = &worker->display_channel->common.base;

    for (;;) {
        uint64_t end_time;
        int ring_is_empty;

        red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(&worker->display_channel->common.base);
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(&worker->display_channel->common.base);
            if (!display_is_connected(worker) ||
                red_channel_max_pipe_size(display_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->display_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            // TODO: MC: the whole timeout will break since it takes lowest timeout, should
            // do it client by client.
            if (red_now() >= end_time) {
                spice_warning("update timeout");
                red_disconnect_all_display_TODO_remove_me(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

static inline void flush_cursor_commands(RedWorker *worker)
{
    RedChannel *cursor_red_channel = &worker->cursor_channel->common.base;

    for (;;) {
        uint64_t end_time;
        int ring_is_empty = FALSE;

        red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
        if (ring_is_empty) {
            break;
        }

        while (red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
            red_channel_push(&worker->cursor_channel->common.base);
        }

        if (ring_is_empty) {
            break;
        }
        end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
        int sleep_count = 0;
        for (;;) {
            red_channel_push(&worker->cursor_channel->common.base);
            if (!cursor_is_connected(worker)
                || red_channel_min_pipe_size(cursor_red_channel) <= MAX_PIPE_SIZE) {
                break;
            }
            RedChannel *channel = (RedChannel *)worker->cursor_channel;
            red_channel_receive(channel);
            red_channel_send(channel);
            if (red_now() >= end_time) {
                spice_warning("flush cursor timeout");
                red_disconnect_cursor(channel);
            } else {
                sleep_count++;
                usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
            }
        }
    }
}

// TODO: on timeout, don't disconnect all channels immediatly - try to disconnect the slowest ones
// first and maybe turn timeouts to several timeouts in order to disconnect channels gradually.
// Should use disconnect or shutdown?
static inline void flush_all_qxl_commands(RedWorker *worker)
{
    flush_display_commands(worker);
    flush_cursor_commands(worker);
}

static void push_new_primary_surface(DisplayChannelClient *dcc)
{
    RedChannelClient *rcc = &dcc->common.base;

    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
    red_create_surface_item(dcc, 0);
    red_channel_client_push(rcc);
}

/* TODO: this function is evil^Wsynchronous, fix */
static int display_channel_client_wait_for_init(DisplayChannelClient *dcc)
{
    dcc->expect_init = TRUE;
    uint64_t end_time = red_now() + DISPLAY_CLIENT_TIMEOUT;
    for (;;) {
        red_channel_client_receive(&dcc->common.base);
        if (!red_channel_client_is_connected(&dcc->common.base)) {
            break;
        }
        if (dcc->pixmap_cache && dcc->glz_dict) {
            dcc->pixmap_cache_generation = dcc->pixmap_cache->generation;
            /* TODO: move common.id? if it's used for a per client structure.. */
            spice_info("creating encoder with id == %d", dcc->common.id);
            dcc->glz = glz_encoder_create(dcc->common.id, dcc->glz_dict->dict, &dcc->glz_data.usr);
            if (!dcc->glz) {
                spice_critical("create global lz failed");
            }
            return TRUE;
        }
        if (red_now() > end_time) {
            spice_warning("timeout");
            red_channel_client_disconnect(&dcc->common.base);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

// 在创建DCC完毕后最后调用
static void on_new_display_channel_client(DisplayChannelClient *dcc)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedChannelClient *rcc = &dcc->common.base;

    red_channel_client_push_set_ack(&dcc->common.base);

    if (red_channel_client_waits_for_migrate_data(rcc)) {
        return;
    }

    if (!display_channel_client_wait_for_init(dcc)) {
        return;
    }
    red_channel_client_ack_zero_messages_window(&dcc->common.base);
    if (worker->surfaces[0].context.canvas) {
        red_current_flush(worker, 0);
        push_new_primary_surface(dcc);
        red_push_surface_image(dcc, 0);
        red_push_monitors_config(dcc);
        red_pipe_add_verb(rcc, SPICE_MSG_DISPLAY_MARK);
        red_disply_start_streams(dcc);
    }
}

static GlzSharedDictionary *_red_find_glz_dictionary(RedClient *client, uint8_t dict_id)
{
    RingItem *now;
    GlzSharedDictionary *ret = NULL;

    now = &glz_dictionary_list;
    while ((now = ring_next(&glz_dictionary_list, now))) {
        GlzSharedDictionary *dict = (GlzSharedDictionary *)now;
        if ((dict->client == client) && (dict->id == dict_id)) {
            ret = dict;
            break;
        }
    }

    return ret;
}

static GlzSharedDictionary *_red_create_glz_dictionary(RedClient *client, uint8_t id,
                                                       GlzEncDictContext *opaque_dict)
{
    GlzSharedDictionary *shared_dict = spice_new0(GlzSharedDictionary, 1);
    shared_dict->dict = opaque_dict;
    shared_dict->id = id;
    shared_dict->refs = 1;
    shared_dict->migrate_freeze = FALSE;
    shared_dict->client = client;
    ring_item_init(&shared_dict->base);
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);
    return shared_dict;
}

static GlzSharedDictionary *red_create_glz_dictionary(DisplayChannelClient *dcc,
                                                      uint8_t id, int window_size)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_create(window_size,
                                                            MAX_LZ_ENCODERS,
                                                            &dcc->glz_data.usr);
#ifdef COMPRESS_DEBUG
    spice_info("Lz Window %d Size=%d", id, window_size);
#endif
    if (!glz_dict) {
        spice_critical("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(dcc->common.base.client, id, glz_dict);
}

static GlzSharedDictionary *red_create_restored_glz_dictionary(DisplayChannelClient *dcc,
                                                               uint8_t id,
                                                               GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_restore(restore_data,
                                                             &dcc->glz_data.usr);
    if (!glz_dict) {
        spice_critical("failed creating lz dictionary");
        return NULL;
    }
    return _red_create_glz_dictionary(dcc->common.base.client, id, glz_dict);
}

static GlzSharedDictionary *red_get_glz_dictionary(DisplayChannelClient *dcc,
                                                   uint8_t id, int window_size)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(dcc->common.base.client, id);

    if (!shared_dict) {
        shared_dict = red_create_glz_dictionary(dcc, id, window_size);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static GlzSharedDictionary *red_restore_glz_dictionary(DisplayChannelClient *dcc,
                                                       uint8_t id,
                                                       GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = _red_find_glz_dictionary(dcc->common.base.client, id);

    if (!shared_dict) {
        shared_dict = red_create_restored_glz_dictionary(dcc, id, restore_data);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    } else {
        shared_dict->refs++;
    }
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static void red_freeze_glz(DisplayChannelClient *dcc)
{
    pthread_rwlock_wrlock(&dcc->glz_dict->encode_lock);
    if (!dcc->glz_dict->migrate_freeze) {
        dcc->glz_dict->migrate_freeze = TRUE;
    }
    pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
}

/* destroy encoder, and dictionary if no one uses it*/
static void red_release_glz(DisplayChannelClient *dcc)
{
    GlzSharedDictionary *shared_dict;

    red_display_client_clear_glz_drawables(dcc);

    glz_encoder_destroy(dcc->glz);
    dcc->glz = NULL;

    if (!(shared_dict = dcc->glz_dict)) {
        return;
    }

    dcc->glz_dict = NULL;
    pthread_mutex_lock(&glz_dictionary_list_lock);
    if (--shared_dict->refs) {
        pthread_mutex_unlock(&glz_dictionary_list_lock);
        return;
    }
    ring_remove(&shared_dict->base);
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &dcc->glz_data.usr);
    free(shared_dict);
}

static PixmapCache *red_create_pixmap_cache(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *cache = spice_new0(PixmapCache, 1);
    ring_item_init(&cache->base);
    pthread_mutex_init(&cache->lock, NULL);
    cache->id = id;
    cache->refs = 1;
    ring_init(&cache->lru);
    cache->available = size;
    cache->size = size;
    cache->client = client;
    return cache;
}

static PixmapCache *red_get_pixmap_cache(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *ret = NULL;
    RingItem *now;
    pthread_mutex_lock(&cache_lock);

    now = &pixmap_cache_list;
    while ((now = ring_next(&pixmap_cache_list, now))) {
        PixmapCache *cache = (PixmapCache *)now;
        if ((cache->client == client) && (cache->id == id)) {
            ret = cache;
            ret->refs++;
            break;
        }
    }
    if (!ret) {
        ret = red_create_pixmap_cache(client, id, size);
        ring_add(&pixmap_cache_list, &ret->base);
    }
    pthread_mutex_unlock(&cache_lock);
    return ret;
}

static void red_release_pixmap_cache(DisplayChannelClient *dcc)
{
    PixmapCache *cache;
    if (!(cache = dcc->pixmap_cache)) {
        return;
    }
    dcc->pixmap_cache = NULL;
    pthread_mutex_lock(&cache_lock);
    if (--cache->refs) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    ring_remove(&cache->base);
    pthread_mutex_unlock(&cache_lock);
    pixmap_cache_destroy(cache);
    free(cache);
}

static int display_channel_init_cache(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    spice_assert(!dcc->pixmap_cache);
    return !!(dcc->pixmap_cache = red_get_pixmap_cache(dcc->common.base.client,
                                                       init_info->pixmap_cache_id,
                                                       init_info->pixmap_cache_size));
}

static int display_channel_init_glz_dictionary(DisplayChannelClient *dcc,
                                               SpiceMsgcDisplayInit *init_info)
{
    spice_assert(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_get_glz_dictionary(dcc,
                                                     init_info->glz_dictionary_id,
                                                     init_info->glz_dictionary_window_size));
}

static int display_channel_init(DisplayChannelClient *dcc, SpiceMsgcDisplayInit *init_info)
{
    return (display_channel_init_cache(dcc, init_info) &&
            display_channel_init_glz_dictionary(dcc, init_info));
}

static int display_channel_handle_migrate_glz_dictionary(DisplayChannelClient *dcc,
                                                         SpiceMigrateDataDisplay *migrate_info)
{
    spice_assert(!dcc->glz_dict);
    ring_init(&dcc->glz_drawables);
    ring_init(&dcc->glz_drawables_inst_to_free);
    pthread_mutex_init(&dcc->glz_drawables_inst_to_free_lock, NULL);
    return !!(dcc->glz_dict = red_restore_glz_dictionary(dcc,
                                                         migrate_info->glz_dict_id,
                                                         &migrate_info->glz_dict_data));
}

static int display_channel_handle_migrate_mark(RedChannelClient *rcc)
{
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    RedChannel *channel = &display_channel->common.base;

    red_channel_pipes_add_type(channel, PIPE_ITEM_TYPE_MIGRATE_DATA);
    return TRUE;
}

static uint64_t display_channel_handle_migrate_data_get_serial(
                RedChannelClient *rcc, uint32_t size, void *message)
{
    SpiceMigrateDataDisplay *migrate_data;

    migrate_data = (SpiceMigrateDataDisplay *)((uint8_t *)message + sizeof(SpiceMigrateDataHeader));

    return migrate_data->message_serial;
}

static int display_channel_client_restore_surface(DisplayChannelClient *dcc, uint32_t surface_id)
{
    /* we don't process commands till we receive the migration data, thus,
     * we should have not sent any surface to the client. */
    if (dcc->surface_client_created[surface_id]) {
        spice_warning("surface %u is already marked as client_created", surface_id);
        return FALSE;
    }
    dcc->surface_client_created[surface_id] = TRUE;
    return TRUE;
}

static int display_channel_client_restore_surfaces_lossless(DisplayChannelClient *dcc,
                                                            MigrateDisplaySurfacesAtClientLossless *mig_surfaces)
{
    uint32_t i;

    spice_debug(NULL);
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;

        if (!display_channel_client_restore_surface(dcc, surface_id)) {
            return FALSE;
        }
    }
    return TRUE;
}

static int display_channel_client_restore_surfaces_lossy(DisplayChannelClient *dcc,
                                                          MigrateDisplaySurfacesAtClientLossy *mig_surfaces)
{
    uint32_t i;

    spice_debug(NULL);
    for (i = 0; i < mig_surfaces->num_surfaces; i++) {
        uint32_t surface_id = mig_surfaces->surfaces[i].id;
        SpiceMigrateDataRect *mig_lossy_rect;
        SpiceRect lossy_rect;

        if (!display_channel_client_restore_surface(dcc, surface_id)) {
            return FALSE;
        }
        spice_assert(dcc->surface_client_created[surface_id]);

        mig_lossy_rect = &mig_surfaces->surfaces[i].lossy_rect;
        lossy_rect.left = mig_lossy_rect->left;
        lossy_rect.top = mig_lossy_rect->top;
        lossy_rect.right = mig_lossy_rect->right;
        lossy_rect.bottom = mig_lossy_rect->bottom;
        region_init(&dcc->surface_client_lossy_region[surface_id]);
        region_add(&dcc->surface_client_lossy_region[surface_id], &lossy_rect);
    }
    return TRUE;
}
static int display_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size,
                                               void *message)
{
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataDisplay *migrate_data;
    DisplayChannel *display_channel = SPICE_CONTAINEROF(rcc->channel, DisplayChannel, common.base);
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);
    uint8_t *surfaces;
    int surfaces_restored = FALSE;
    int i;

    spice_debug(NULL);
    if (size < sizeof(*migrate_data) + sizeof(SpiceMigrateDataHeader)) {
        spice_error("bad message size");
        return FALSE;
    }
    header = (SpiceMigrateDataHeader *)message;
    migrate_data = (SpiceMigrateDataDisplay *)(header + 1);
    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_DISPLAY_MAGIC,
                                            SPICE_MIGRATE_DATA_DISPLAY_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    /* size is set to -1 in order to keep the cache frozen until the original
     * channel client that froze the cache on the src size receives the migrate
     * data and unfreezes the cache by setting its size > 0 and by triggering
     * pixmap_cache_reset */
    dcc->pixmap_cache = red_get_pixmap_cache(dcc->common.base.client,
                                             migrate_data->pixmap_cache_id, -1);
    if (!dcc->pixmap_cache) {
        return FALSE;
    }
    pthread_mutex_lock(&dcc->pixmap_cache->lock);
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        dcc->pixmap_cache->sync[i] = MAX(dcc->pixmap_cache->sync[i],
                                         migrate_data->pixmap_cache_clients[i]);
    }
    pthread_mutex_unlock(&dcc->pixmap_cache->lock);

    if (migrate_data->pixmap_cache_freezer) {
        /* activating the cache. The cache will start to be active after
         * pixmap_cache_reset is called, when handling PIPE_ITEM_TYPE_PIXMAP_RESET */
        dcc->pixmap_cache->size = migrate_data->pixmap_cache_size;
        red_channel_client_pipe_add_type(rcc,
                                         PIPE_ITEM_TYPE_PIXMAP_RESET);
    }

    if (display_channel_handle_migrate_glz_dictionary(dcc, migrate_data)) {
        dcc->glz = glz_encoder_create(dcc->common.id,
                                      dcc->glz_dict->dict, &dcc->glz_data.usr);
        if (!dcc->glz) {
            spice_critical("create global lz failed");
        }
    } else {
        spice_critical("restoring global lz dictionary failed");
    }

    dcc->common.is_low_bandwidth = migrate_data->low_bandwidth_setting;

    if (migrate_data->low_bandwidth_setting) {
        red_channel_client_ack_set_client_window(rcc, WIDE_CLIENT_ACK_WINDOW);
        if (dcc->common.worker->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
            display_channel->enable_jpeg = TRUE;
        }
        if (dcc->common.worker->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
            display_channel->enable_zlib_glz_wrap = TRUE;
        }
    }

    surfaces = (uint8_t *)message + migrate_data->surfaces_at_client_ptr;
    if (display_channel->enable_jpeg) {
        surfaces_restored = display_channel_client_restore_surfaces_lossy(dcc,
                                (MigrateDisplaySurfacesAtClientLossy *)surfaces);
    } else {
        surfaces_restored = display_channel_client_restore_surfaces_lossless(dcc,
                                (MigrateDisplaySurfacesAtClientLossless*)surfaces);
    }

    if (!surfaces_restored) {
        return FALSE;
    }
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
    /* enable sending messages */
    red_channel_client_ack_zero_messages_window(rcc);
    return TRUE;
}

static int display_channel_handle_stream_report(DisplayChannelClient *dcc,
                                                SpiceMsgcDisplayStreamReport *stream_report)
{
    StreamAgent *stream_agent;

    if (stream_report->stream_id >= NUM_STREAMS) {
        spice_warning("stream_report: invalid stream id %u", stream_report->stream_id);
        return FALSE;
    }
    stream_agent = &dcc->stream_agents[stream_report->stream_id];
    if (!stream_agent->mjpeg_encoder) {
        spice_info("stream_report: no encoder for stream id %u."
                    "Probably the stream has been destroyed", stream_report->stream_id);
        return TRUE;
    }

    if (stream_report->unique_id != stream_agent->report_id) {
        spice_warning("local reoprt-id (%u) != msg report-id (%u)",
                      stream_agent->report_id, stream_report->unique_id);
        return TRUE;
    }
    mjpeg_encoder_client_stream_report(stream_agent->mjpeg_encoder,
                                       stream_report->num_frames,
                                       stream_report->num_drops,
                                       stream_report->start_frame_mm_time,
                                       stream_report->end_frame_mm_time,
                                       stream_report->last_frame_delay,
                                       stream_report->audio_delay);
    return TRUE;
}

static int display_channel_handle_message(RedChannelClient *rcc, uint32_t size, uint16_t type,
                                          void *message)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
        if (!dcc->expect_init) {
            spice_warning("unexpected SPICE_MSGC_DISPLAY_INIT");
            return FALSE;
        }
        dcc->expect_init = FALSE;
        return display_channel_init(dcc, (SpiceMsgcDisplayInit *)message);
    case SPICE_MSGC_DISPLAY_STREAM_REPORT:
        return display_channel_handle_stream_report(dcc,
                                                    (SpiceMsgcDisplayStreamReport *)message);
    default:
        return red_channel_client_handle_message(rcc, size, type, message);
    }
}

/* 配置通道socket的一般方法 */
static int common_channel_config_socket(RedChannelClient *rcc)
{
    RedClient *client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc = red_client_get_main(client);
    RedsStream *stream = red_channel_client_get_stream(rcc);
    CommonChannelClient *ccc = SPICE_CONTAINEROF(rcc, CommonChannelClient, base);
    int flags;
    int delay_val;

    if ((flags = fcntl(stream->socket, F_GETFL)) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

	//非阻塞IO
    if (fcntl(stream->socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return FALSE;
    }

    // TODO - this should be dynamic, not one time at channel creation
    ccc->is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    delay_val = ccc->is_low_bandwidth ? 0 : 1;
    /* FIXME: Using Nagle's Algorithm can lead to apparent delays, depending
     * on the delayed ack timeout on the other side.
     * Instead of using Nagle's, we need to implement message buffering on
     * the application level.
     * see: http://www.stuartcheshire.org/papers/NagleDelayedAck/
     */
    if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY, &delay_val,
                   sizeof(delay_val)) == -1) {
        if (errno != ENOTSUP) {
            spice_warning("setsockopt failed, %s", strerror(errno));
        }
    }
    return TRUE;
}

// SpiceWorker更新事件监听掩码
static void worker_watch_update_mask(SpiceWatch *watch, int event_mask)
{
    struct RedWorker *worker;
    int i;

    if (!watch) {
        return;
    }

    worker = watch->worker;
    i = watch - worker->watches;

    worker->poll_fds[i].events = 0;
    if (event_mask & SPICE_WATCH_EVENT_READ) {
        worker->poll_fds[i].events |= POLLIN;
    }
    if (event_mask & SPICE_WATCH_EVENT_WRITE) {
        worker->poll_fds[i].events |= POLLOUT;
    }
}

static SpiceWatch *worker_watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    /* Since we are a channel core implementation, we always get called from
       red_channel_client_create(), so opaque always is our rcc */
    RedChannelClient *rcc = opaque;
    struct RedWorker *worker;
    int i;

    /* Since we are called from red_channel_client_create()
       CommonChannelClient->worker has not been set yet! */
    worker = SPICE_CONTAINEROF(rcc->channel, CommonChannel, base)->worker;

    /* Search for a free slot in our poll_fds & watches arrays */
    for (i = 0; i < MAX_EVENT_SOURCES; i++) {
        if (worker->poll_fds[i].fd == -1) {
            break;
        }
    }
    if (i == MAX_EVENT_SOURCES) {
        spice_warning("could not add a watch for channel type %u id %u",
                      rcc->channel->type, rcc->channel->id);
        return NULL;
    }

    worker->poll_fds[i].fd = fd;
    worker->watches[i].worker = worker;
    worker->watches[i].watch_func = func;
    worker->watches[i].watch_func_opaque = opaque;
    worker_watch_update_mask(&worker->watches[i], event_mask);

    return &worker->watches[i];
}

static void worker_watch_remove(SpiceWatch *watch)
{
    if (!watch) {
        return;
    }

    /* Note we don't touch the poll_fd here, to avoid the
       poll_fds/watches table entry getting re-used in the same
       red_worker_main loop over the fds as it is removed.

       This is done because re-using it while events were pending on
       the fd previously occupying the slot would lead to incorrectly
       calling the watch_func for the new fd. */
    memset(watch, 0, sizeof(SpiceWatch));
}

SpiceCoreInterface worker_core = {
    .timer_add = spice_timer_queue_add,
    .timer_start = spice_timer_set,
    .timer_cancel = spice_timer_cancel,
    .timer_remove = spice_timer_remove,

    .watch_update_mask = worker_watch_update_mask,
    .watch_add = worker_watch_add,
    .watch_remove = worker_watch_remove,
};

static CommonChannelClient *common_channel_client_create(int size,
                                                         CommonChannel *common,
                                                         RedClient *client,
                                                         RedsStream *stream,
                                                         int mig_target,
                                                         int monitor_latency,
                                                         uint32_t *common_caps,
                                                         int num_common_caps,
                                                         uint32_t *caps,
                                                         int num_caps)
{
    RedChannelClient *rcc =
        red_channel_client_create(size, &common->base, client, stream, monitor_latency,
                                  num_common_caps, common_caps, num_caps, caps);
    if (!rcc) {
        return NULL;
    }
    CommonChannelClient *common_cc = (CommonChannelClient*)rcc;
    common_cc->worker = common->worker;
    common_cc->id = common->worker->id;
    common->during_target_migrate = mig_target;

    // TODO: move wide/narrow ack setting to red_channel.
    red_channel_client_ack_set_client_window(rcc,
        common_cc->is_low_bandwidth ?
        WIDE_CLIENT_ACK_WINDOW : NARROW_CLIENT_ACK_WINDOW);
    return common_cc;
}


DisplayChannelClient *display_channel_client_create(CommonChannel *common,
                                                    RedClient *client, RedsStream *stream,
                                                    int mig_target,
                                                    uint32_t *common_caps, int num_common_caps,
                                                    uint32_t *caps, int num_caps)
{
    DisplayChannelClient *dcc =
        (DisplayChannelClient*)common_channel_client_create(
            sizeof(DisplayChannelClient), common, client, stream,
            mig_target,
            TRUE,
            common_caps, num_common_caps,
            caps, num_caps);

    if (!dcc) {
        return NULL;
    }
    ring_init(&dcc->palette_cache_lru);
    dcc->palette_cache_available = CLIENT_PALETTE_CACHE_SIZE;
    return dcc;
}

CursorChannelClient *cursor_channel_create_rcc(CommonChannel *common,
                                               RedClient *client, RedsStream *stream,
                                               int mig_target,
                                               uint32_t *common_caps, int num_common_caps,
                                               uint32_t *caps, int num_caps)
{
    CursorChannelClient *ccc =
        (CursorChannelClient*)common_channel_client_create(
            sizeof(CursorChannelClient), common, client, stream,
            mig_target,
            FALSE,
            common_caps,
            num_common_caps,
            caps,
            num_caps);

    if (!ccc) {
        return NULL;
    }
    ring_init(&ccc->cursor_cache_lru);
    ccc->cursor_cache_available = CLIENT_CURSOR_CACHE_SIZE;
    return ccc;
}

/* __new_channel - DisplayChannel和CursorChannel公共的通道创建函数, 类似PlaybackChannel
  和RecordChannel也有一个公共的通道创建函数。此函数调用RedChannel实现代码中的接口
  red_channel_create_parser来创建通道

*/
static RedChannel *__new_channel(
	RedWorker *worker, int size, //size是实际通道的大小
	uint32_t channel_type, //channel_type是通道类型
	int migration_flags, //迁移标记
	channel_disconnect_proc on_disconnect, //RCC断开回调函数
	channel_send_pipe_item_proc send_item, //RCC发送PIPE_ITEM
	channel_hold_pipe_item_proc hold_item, //RCC持有PIPE_ITEM
	channel_release_pipe_item_proc release_item, //RCC释放PIPE_ITEM
	channel_handle_parsed_proc handle_parsed, //RCC处理已经解析的消息对象
	channel_handle_migrate_flush_mark_proc handle_migrate_flush_mark, //迁移
	channel_handle_migrate_data_proc handle_migrate_data, //迁移
	channel_handle_migrate_data_get_serial_proc migrate_get_serial) //迁移
{
    RedChannel *channel = NULL;
    CommonChannel *common;
    ChannelCbs channel_cbs = { NULL, };

	/* 填充ChannelCbs RCC回调函数集 */
    channel_cbs.config_socket = common_channel_config_socket;//显示和光标通道socket属性配置
    channel_cbs.on_disconnect = on_disconnect;
    channel_cbs.send_item = send_item;
    channel_cbs.hold_item = hold_item;
    channel_cbs.release_item = release_item;
	//接受缓冲区分配，VDI里只会使用CommonChannel.recv_buf这个静态缓冲区
    channel_cbs.alloc_recv_buf = common_alloc_recv_buf; 
	//释放缓冲区，VDI里因为使用的静态缓冲区，什么都没做，只有迁移数据要释放
    channel_cbs.release_recv_buf = common_release_recv_buf; 
    channel_cbs.handle_migrate_flush_mark = handle_migrate_flush_mark;
    channel_cbs.handle_migrate_data = handle_migrate_data;
    channel_cbs.handle_migrate_data_get_serial = migrate_get_serial;

	// 调用 red_channel_create_parser 创建通道
    channel = red_channel_create_parser(size, &worker_core,
    	channel_type, worker->id,//通道id从worker中传入
    	TRUE /* handle_acks */,
    	//获取通道的消息解析函数
    	spice_get_client_channel_parser(channel_type, NULL),
    	handle_parsed, //传入解析后消息对象分发函数
    	&channel_cbs, //传入构建的RCC回调函数集
    	migration_flags);
    common = (CommonChannel *)channel; //创建的通道必然是CommonChannel
    if (!channel) {
        goto error;
    }
    common->worker = worker;
    return channel;

error:
    free(channel);
    return NULL;
}

/* 显示通道在持有管道项时调用 */
static void display_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
    spice_assert(item);
    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW: //对DPI命令增加引用
        ref_drawable_pipe_item(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        ((StreamClipItem *)item)->refs++; //流裁剪命令引用增加
        break;
    case PIPE_ITEM_TYPE_UPGRADE: //surface更新命令
        ((UpgradeItem *)item)->refs++;
        break;
    case PIPE_ITEM_TYPE_IMAGE: // surface图像命令
        ((ImageItem *)item)->refs++;
        break;
    default:
        spice_critical("invalid item type");
    }
}

static void display_channel_client_release_item_after_push(DisplayChannelClient *dcc,
                                                           PipeItem *item)
{
    RedWorker *worker = dcc->common.worker;

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW:
        put_drawable_pipe_item(SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item));
        break;
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        red_display_release_stream_clip(worker, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_UPGRADE:
        release_upgrade_item(worker, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_VERB:
        free(item);
        break;
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(item,
                                                             MonitorsConfigItem, pipe_item);
        monitors_config_decref(monconf_item->monitors_config);
        free(item);
        break;
    }
    default:
        spice_critical("invalid item type");
    }
}

// TODO: share code between before/after_push since most of the items need the same
// release
// 如果还没有push之前DCC的管道项就要被release时调用下面函数
static void display_channel_client_release_item_before_push(DisplayChannelClient *dcc,
                                                            PipeItem *item)
{
    RedWorker *worker = dcc->common.worker;

    switch (item->type) {
    case PIPE_ITEM_TYPE_DRAW: {
        DrawablePipeItem *dpi = SPICE_CONTAINEROF(item, DrawablePipeItem, dpi_pipe_item);
        ring_remove(&dpi->base);
        put_drawable_pipe_item(dpi);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, create_item);
        red_display_release_stream(worker, agent);
        break;
    }
    case PIPE_ITEM_TYPE_STREAM_CLIP:
        red_display_release_stream_clip(worker, (StreamClipItem *)item);
        break;
    case PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamAgent *agent = SPICE_CONTAINEROF(item, StreamAgent, destroy_item);
        red_display_release_stream(worker, agent);
        break;
    }
    case PIPE_ITEM_TYPE_UPGRADE:
        release_upgrade_item(worker, (UpgradeItem *)item);
        break;
    case PIPE_ITEM_TYPE_IMAGE:
        release_image_item((ImageItem *)item);
        break;
    case PIPE_ITEM_TYPE_CREATE_SURFACE: {
        SurfaceCreateItem *surface_create = SPICE_CONTAINEROF(item, SurfaceCreateItem,
                                                              pipe_item);
        free(surface_create);
        break;
    }
    case PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        SurfaceDestroyItem *surface_destroy = SPICE_CONTAINEROF(item, SurfaceDestroyItem,
                                                                pipe_item);
        free(surface_destroy);
        break;
    }
    case PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        MonitorsConfigItem *monconf_item = SPICE_CONTAINEROF(item,
                                                             MonitorsConfigItem, pipe_item);
        monitors_config_decref(monconf_item->monitors_config);
        free(item);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
    case PIPE_ITEM_TYPE_VERB:
    case PIPE_ITEM_TYPE_MIGRATE_DATA:
    case PIPE_ITEM_TYPE_PIXMAP_SYNC:
    case PIPE_ITEM_TYPE_PIXMAP_RESET:
    case PIPE_ITEM_TYPE_INVAL_PALLET_CACHE:
    case PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT:
        free(item);
        break;
    default:
        spice_critical("invalid item type");
    }
}

// 释放RCC中的管道项item， item_pushed标志item是不是已经被发送过
static void display_channel_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    spice_assert(item);
    if (item_pushed) {
        display_channel_client_release_item_after_push(dcc, item);
    } else {
        spice_debug("not pushed (%d)", item->type);
        display_channel_client_release_item_before_push(dcc, item);
    }
}

/* 显示通道创建函数，支持迁移场景 */
static void display_channel_create(RedWorker *worker, int migrate)
{
    DisplayChannel *display_channel;

    if (worker->display_channel) { //如果显示通道已经存在了，直接返回
        return;
    }

    spice_info("create display channel");
    if (!(worker->display_channel = (DisplayChannel *)__new_channel(
            worker, sizeof(*display_channel),
            SPICE_CHANNEL_DISPLAY,
            SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER,
            display_channel_client_on_disconnect,
            display_channel_send_item,
            display_channel_hold_pipe_item,
            display_channel_release_item,
            display_channel_handle_message,
            display_channel_handle_migrate_mark,
            display_channel_handle_migrate_data,
            display_channel_handle_migrate_data_get_serial
            ))) {
        spice_warning("failed to create display channel");
        return;
    }
    display_channel = worker->display_channel;
#ifdef RED_STATISTICS
    display_channel->stat = stat_add_node(worker->stat, "display_channel", TRUE);
    display_channel->common.base.out_bytes_counter = stat_add_counter(display_channel->stat,
                                                               "out_bytes", TRUE);
    display_channel->cache_hits_counter = stat_add_counter(display_channel->stat,
                                                           "cache_hits", TRUE);
    display_channel->add_to_cache_counter = stat_add_counter(display_channel->stat,
                                                             "add_to_cache", TRUE);
    display_channel->non_cache_counter = stat_add_counter(display_channel->stat,
                                                          "non_cache", TRUE);
#endif
    stat_compress_init(&display_channel->lz_stat, lz_stat_name);
    stat_compress_init(&display_channel->glz_stat, glz_stat_name);
    stat_compress_init(&display_channel->quic_stat, quic_stat_name);
    stat_compress_init(&display_channel->jpeg_stat, jpeg_stat_name);
    stat_compress_init(&display_channel->zlib_glz_stat, zlib_stat_name);
    stat_compress_init(&display_channel->jpeg_alpha_stat, jpeg_alpha_stat_name);
}

static void guest_set_client_capabilities(RedWorker *worker)
{
    int i;
    DisplayChannelClient *dcc;
    RedChannelClient *rcc;
    RingItem *link, *next;
    uint8_t caps[58] = { 0 };
    int caps_available[] = {
        SPICE_DISPLAY_CAP_SIZED_STREAM,
        SPICE_DISPLAY_CAP_MONITORS_CONFIG,
        SPICE_DISPLAY_CAP_COMPOSITE,
        SPICE_DISPLAY_CAP_A8_SURFACE,
    };

    if (worker->qxl->st->qif->base.major_version < 3 ||
        (worker->qxl->st->qif->base.major_version == 3 &&
        worker->qxl->st->qif->base.minor_version < 2) ||
        !worker->qxl->st->qif->set_client_capabilities) {
        return;
    }
#define SET_CAP(a,c)                                                    \
        ((a)[(c) / 8] |= (1 << ((c) % 8)))

#define CLEAR_CAP(a,c)                                                  \
        ((a)[(c) / 8] &= ~(1 << ((c) % 8)))

    if (!worker->running) {
        worker->set_client_capabilities_pending = 1;
        return;
    }
    if ((worker->display_channel == NULL) ||
        (worker->display_channel->common.base.clients_num == 0)) {
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, FALSE, caps);
    } else {
        // Take least common denominator
        for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
            SET_CAP(caps, caps_available[i]);
        }
        DCC_FOREACH_SAFE(link, next, dcc, &worker->display_channel->common.base) {
            rcc = (RedChannelClient *)dcc;
            for (i = 0 ; i < sizeof(caps_available) / sizeof(caps_available[0]); ++i) {
                if (!red_channel_client_test_remote_cap(rcc, caps_available[i]))
                    CLEAR_CAP(caps, caps_available[i]);
            }
        }
        worker->qxl->st->qif->set_client_capabilities(worker->qxl, TRUE, caps);
    }
    worker->set_client_capabilities_pending = 0;
}

/*
 * 
*/
static void handle_new_display_channel(
	RedWorker *worker, 
	RedClient *client,
	RedsStream *stream,
	int migrate,
	uint32_t *common_caps, int num_common_caps,
	uint32_t *caps, int num_caps)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc;
    size_t stream_buf_size;

    if (!worker->display_channel) {
        spice_warning("Display channel was not created");
        return;
    }
    display_channel = worker->display_channel;
    spice_info("add display channel client");
    dcc = display_channel_client_create(&display_channel->common, client, stream,
                                        migrate,
                                        common_caps, num_common_caps,
                                        caps, num_caps);
    if (!dcc) {
        return;
    }
    spice_info("New display (client %p) dcc %p stream %p", client, dcc, stream);
    stream_buf_size = 32*1024;
    dcc->send_data.stream_outbuf = spice_malloc(stream_buf_size);
    dcc->send_data.stream_outbuf_size = stream_buf_size;
    red_display_init_glz_data(dcc);

    dcc->send_data.free_list.res =
        spice_malloc(sizeof(SpiceResourceList) +
                     DISPLAY_FREE_LIST_DEFAULT_SIZE * sizeof(SpiceResourceID));
    dcc->send_data.free_list.res_size = DISPLAY_FREE_LIST_DEFAULT_SIZE;

    if (worker->jpeg_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_jpeg = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_jpeg = (worker->jpeg_state == SPICE_WAN_COMPRESSION_ALWAYS);
    }

    // todo: tune quality according to bandwidth
    display_channel->jpeg_quality = 85;

    if (worker->zlib_glz_state == SPICE_WAN_COMPRESSION_AUTO) {
        display_channel->enable_zlib_glz_wrap = dcc->common.is_low_bandwidth;
    } else {
        display_channel->enable_zlib_glz_wrap = (worker->zlib_glz_state ==
                                                 SPICE_WAN_COMPRESSION_ALWAYS);
    }

    spice_info("jpeg %s", display_channel->enable_jpeg ? "enabled" : "disabled");
    spice_info("zlib-over-glz %s", display_channel->enable_zlib_glz_wrap ? "enabled" : "disabled");

    guest_set_client_capabilities(worker);

    // todo: tune level according to bandwidth
    display_channel->zlib_level = ZLIB_DEFAULT_COMPRESSION_LEVEL;
    red_display_client_init_streams(dcc);
    on_new_display_channel_client(dcc);
}

static void cursor_channel_client_on_disconnect(RedChannelClient *rcc)
{
    if (!rcc) {
        return;
    }
    red_reset_cursor_cache(rcc);
}

static void red_disconnect_cursor(RedChannel *channel)
{
    CommonChannel *common;

    if (!channel || !red_channel_is_connected(channel)) {
        return;
    }
    common = SPICE_CONTAINEROF(channel, CommonChannel, base);
    spice_assert(channel == (RedChannel *)common->worker->cursor_channel);
    common->worker->cursor_channel = NULL;
    red_channel_apply_clients(channel, red_reset_cursor_cache);
    red_channel_disconnect(channel);
}

static void red_migrate_cursor(RedWorker *worker, RedChannelClient *rcc)
{
    if (red_channel_client_is_connected(rcc)) {
        red_channel_client_pipe_add_type(rcc,
                                         PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        red_channel_client_default_migrate(rcc);
    }
}

static void on_new_cursor_channel(RedWorker *worker, RedChannelClient *rcc)
{
    CursorChannel *channel = worker->cursor_channel;

    spice_assert(channel);
    red_channel_client_ack_zero_messages_window(rcc);
    red_channel_client_push_set_ack(rcc);
    // TODO: why do we check for context.canvas? defer this to after display cc is connected
    // and test it's canvas? this is just a test to see if there is an active renderer?
    if (worker->surfaces[0].context.canvas && !channel->common.during_target_migrate) {
        red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_CURSOR_INIT);
    }
}

static void cursor_channel_hold_pipe_item(RedChannelClient *rcc, PipeItem *item)
{
    CursorPipeItem *cursor_pipe_item;
    spice_assert(item);
    cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
    ref_cursor_pipe_item(cursor_pipe_item);
}

// TODO: share code between before/after_push since most of the items need the same
// release
static void cursor_channel_client_release_item_before_push(CursorChannelClient *ccc,
                                                           PipeItem *item)
{
    switch (item->type) {
    case PIPE_ITEM_TYPE_CURSOR: {
        CursorPipeItem *cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
        put_cursor_pipe_item(ccc, cursor_pipe_item);
        break;
    }
    case PIPE_ITEM_TYPE_INVAL_ONE:
    case PIPE_ITEM_TYPE_VERB:
    case PIPE_ITEM_TYPE_CURSOR_INIT:
    case PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE:
        free(item);
        break;
    default:
        spice_error("invalid pipe item type");
    }
}

static void cursor_channel_client_release_item_after_push(CursorChannelClient *ccc,
                                                          PipeItem *item)
{
    switch (item->type) {
        case PIPE_ITEM_TYPE_CURSOR: {
            CursorPipeItem *cursor_pipe_item = SPICE_CONTAINEROF(item, CursorPipeItem, base);
            put_cursor_pipe_item(ccc, cursor_pipe_item);
            break;
        }
        default:
            spice_critical("invalid item type");
    }
}

static void cursor_channel_release_item(RedChannelClient *rcc, PipeItem *item, int item_pushed)
{
    CursorChannelClient *ccc = RCC_TO_CCC(rcc);

    spice_assert(item);

    if (item_pushed) {
        cursor_channel_client_release_item_after_push(ccc, item);
    } else {
        spice_debug("not pushed (%d)", item->type);
        cursor_channel_client_release_item_before_push(ccc, item);
    }
}

static void cursor_channel_create(RedWorker *worker, int migrate)
{
    if (worker->cursor_channel != NULL) {
        return;
    }
    spice_info("create cursor channel");
    worker->cursor_channel = (CursorChannel *)__new_channel(
        worker, sizeof(*worker->cursor_channel),
        SPICE_CHANNEL_CURSOR,
        0,
        cursor_channel_client_on_disconnect,
        cursor_channel_send_item,
        cursor_channel_hold_pipe_item,
        cursor_channel_release_item,
        red_channel_client_handle_message,
        NULL,
        NULL,
        NULL);
}

static void red_connect_cursor(RedWorker *worker, RedClient *client, RedsStream *stream,
                               int migrate,
                               uint32_t *common_caps, int num_common_caps,
                               uint32_t *caps, int num_caps)
{
    CursorChannel *channel;
    CursorChannelClient *ccc;

    if (worker->cursor_channel == NULL) {
        spice_warning("cursor channel was not created");
        return;
    }
    channel = worker->cursor_channel;
    spice_info("add cursor channel client");
    ccc = cursor_channel_create_rcc(&channel->common, client, stream,
                                    migrate,
                                    common_caps, num_common_caps,
                                    caps, num_caps);
    if (!ccc) {
        return;
    }
#ifdef RED_STATISTICS
    channel->stat = stat_add_node(worker->stat, "cursor_channel", TRUE);
    channel->common.base.out_bytes_counter = stat_add_counter(channel->stat, "out_bytes", TRUE);
#endif
    on_new_cursor_channel(worker, &ccc->common.base);
}

typedef struct __attribute__ ((__packed__)) CursorData {
    uint32_t visible;
    SpicePoint16 position;
    uint16_t trail_length;
    uint16_t trail_frequency;
    uint32_t data_size;
    SpiceCursor _cursor;
} CursorData;

static void surface_dirty_region_to_rects(RedSurface *surface,
                                          QXLRect *qxl_dirty_rects,
                                          uint32_t num_dirty_rects,
                                          int clear_dirty_region)
{
    QRegion *surface_dirty_region;
    SpiceRect *dirty_rects;
    int i;

    surface_dirty_region = &surface->draw_dirty_region;
    dirty_rects = spice_new0(SpiceRect, num_dirty_rects);
    region_ret_rects(surface_dirty_region, dirty_rects, num_dirty_rects);
    if (clear_dirty_region) {
        region_clear(surface_dirty_region);
    }
    for (i = 0; i < num_dirty_rects; i++) {
        qxl_dirty_rects[i].top    = dirty_rects[i].top;
        qxl_dirty_rects[i].left   = dirty_rects[i].left;
        qxl_dirty_rects[i].bottom = dirty_rects[i].bottom;
        qxl_dirty_rects[i].right  = dirty_rects[i].right;
    }
    free(dirty_rects);
}

void handle_dev_update_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdateAsync *msg = payload;
    SpiceRect rect;
    QXLRect *qxl_dirty_rects;
    uint32_t num_dirty_rects;
    RedSurface *surface;
    uint32_t surface_id = msg->surface_id;
    QXLRect qxl_area = msg->qxl_area;
    uint32_t clear_dirty_region = msg->clear_dirty_region;

    red_get_rect_ptr(&rect, &qxl_area);
    flush_display_commands(worker);

    spice_assert(worker->running);

    VALIDATE_SURFACE_RET(worker, surface_id);
    red_update_area(worker, &rect, surface_id);
    if (!worker->qxl->st->qif->update_area_complete) {
        return;
    }
    surface = &worker->surfaces[surface_id];
    num_dirty_rects = pixman_region32_n_rects(&surface->draw_dirty_region);
    if (num_dirty_rects == 0) {
        return;
    }
    qxl_dirty_rects = spice_new0(QXLRect, num_dirty_rects);
    surface_dirty_region_to_rects(surface, qxl_dirty_rects, num_dirty_rects,
                                  clear_dirty_region);
    worker->qxl->st->qif->update_area_complete(worker->qxl, surface_id,
                                          qxl_dirty_rects, num_dirty_rects);
    free(qxl_dirty_rects);
}

void handle_dev_update(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageUpdate *msg = payload;
    SpiceRect *rect = spice_new0(SpiceRect, 1);
    RedSurface *surface;
    uint32_t surface_id = msg->surface_id;
    const QXLRect *qxl_area = msg->qxl_area; //更新区域
    uint32_t num_dirty_rects = msg->num_dirty_rects; //脏域矩形个数
    QXLRect *qxl_dirty_rects = msg->qxl_dirty_rects; //脏域矩形数组
    uint32_t clear_dirty_region = msg->clear_dirty_region; //是否清空脏域

    surface = &worker->surfaces[surface_id];
    red_get_rect_ptr(rect, qxl_area);
    flush_display_commands(worker);

    spice_assert(worker->running);

    if (validate_surface(worker, surface_id)) {
        red_update_area(worker, rect, surface_id);
    } else {
        rendering_incorrect(__func__);
    }
    free(rect);

    surface_dirty_region_to_rects(surface, qxl_dirty_rects, num_dirty_rects,
                                  clear_dirty_region);
}

static void dev_add_memslot(RedWorker *worker, QXLDevMemSlot mem_slot)
{
    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

void handle_dev_add_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAddMemslot *msg = payload;
    QXLDevMemSlot mem_slot = msg->mem_slot;

    red_memslot_info_add_slot(&worker->mem_slots, mem_slot.slot_group_id, mem_slot.slot_id,
                              mem_slot.addr_delta, mem_slot.virt_start, mem_slot.virt_end,
                              mem_slot.generation);
}

void handle_dev_del_memslot(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageDelMemslot *msg = payload;
    uint32_t slot_id = msg->slot_id;
    uint32_t slot_group_id = msg->slot_group_id;

    red_memslot_info_del_slot(&worker->mem_slots, slot_group_id, slot_id);
}

/* TODO: destroy_surface_wait, dev_destroy_surface_wait - confusing. one asserts
 * surface_id == 0, maybe move the assert upward and merge the two functions? */
static inline void destroy_surface_wait(RedWorker *worker, int surface_id)
{
    if (!worker->surfaces[surface_id].context.canvas) {
        return;
    }

    red_handle_depends_on_target_surface(worker, surface_id);
    /* note that red_handle_depends_on_target_surface must be called before red_current_clear.
       otherwise "current" will hold items that other drawables may depend on, and then
       red_current_clear will remove them from the pipe. */
    red_current_clear(worker, surface_id);
    red_clear_surface_drawables_from_pipes(worker, surface_id, TRUE);
}

static void dev_destroy_surface_wait(RedWorker *worker, uint32_t surface_id)
{
    spice_assert(surface_id == 0);

    flush_all_qxl_commands(worker);

    if (worker->surfaces[0].context.canvas) {
        destroy_surface_wait(worker, 0);
    }
}

void handle_dev_destroy_surface_wait(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWait *msg = payload;
    RedWorker *worker = opaque;

    dev_destroy_surface_wait(worker, msg->surface_id);
}

static void rcc_disconnect_if_pending_send(RedChannelClient *rcc)
{
    if (red_channel_client_blocked(rcc) || rcc->pipe_size > 0) {
        red_channel_client_disconnect(rcc);
    } else {
        spice_assert(red_channel_client_no_item_being_sent(rcc));
    }
}

static inline void red_cursor_reset(RedWorker *worker)
{
    if (worker->cursor) {
        red_release_cursor(worker, worker->cursor);
        worker->cursor = NULL;
    }

    worker->cursor_visible = TRUE;
    worker->cursor_position.x = worker->cursor_position.y = 0;
    worker->cursor_trail_length = worker->cursor_trail_frequency = 0;

    if (cursor_is_connected(worker)) {
        red_channel_pipes_add_type(&worker->cursor_channel->common.base,
                                   PIPE_ITEM_TYPE_INVAL_CURSOR_CACHE);
        if (!worker->cursor_channel->common.during_target_migrate) {
            red_pipes_add_verb(&worker->cursor_channel->common.base, SPICE_MSG_CURSOR_RESET);
        }
        if (!red_channel_wait_all_sent(&worker->cursor_channel->common.base,
                                       DISPLAY_CLIENT_TIMEOUT)) {
            red_channel_apply_clients(&worker->cursor_channel->common.base,
                                      rcc_disconnect_if_pending_send);
        }
    }
}

/* called upon device reset */

/* TODO: split me*/
static inline void dev_destroy_surfaces(RedWorker *worker)
{
    int i;

    spice_debug(NULL);
    flush_all_qxl_commands(worker);
    //to handle better
    for (i = 0; i < NUM_SURFACES; ++i) {
        if (worker->surfaces[i].context.canvas) {
            destroy_surface_wait(worker, i);
            if (worker->surfaces[i].context.canvas) {
                red_destroy_surface(worker, i);
            }
            spice_assert(!worker->surfaces[i].context.canvas);
        }
    }
    spice_assert(ring_is_empty(&worker->streams));

    if (display_is_connected(worker)) {
        red_channel_pipes_add_type(&worker->display_channel->common.base,
                                   PIPE_ITEM_TYPE_INVAL_PALLET_CACHE);
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL);
    }

    red_display_clear_glz_drawables(worker->display_channel);

    red_cursor_reset(worker);
}

void handle_dev_destroy_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_destroy_surfaces(worker);
}

static MonitorsConfigItem *get_monitors_config_item(
    RedChannel* channel, MonitorsConfig *monitors_config)
{
    MonitorsConfigItem *mci;

    mci = (MonitorsConfigItem *)spice_malloc(sizeof(*mci));
    mci->monitors_config = monitors_config;

    red_channel_pipe_item_init(channel,
            &mci->pipe_item, PIPE_ITEM_TYPE_MONITORS_CONFIG);
    return mci;
}

static inline void red_monitors_config_item_add(DisplayChannelClient *dcc)
{
    MonitorsConfigItem *mci;
    RedWorker *worker = dcc->common.worker;

    mci = get_monitors_config_item(dcc->common.base.channel,
                                   monitors_config_getref(worker->monitors_config));
    red_channel_client_pipe_add(&dcc->common.base, &mci->pipe_item);
}

static void worker_update_monitors_config(RedWorker *worker,
                                          QXLMonitorsConfig *dev_monitors_config)
{
    int heads_size;
    MonitorsConfig *monitors_config;
    int i;

    monitors_config_decref(worker->monitors_config);

    spice_debug("monitors config %d(%d)",
                dev_monitors_config->count,
                dev_monitors_config->max_allowed);
    for (i = 0; i < dev_monitors_config->count; i++) {
        spice_debug("+%d+%d %dx%d",
                    dev_monitors_config->heads[i].x,
                    dev_monitors_config->heads[i].y,
                    dev_monitors_config->heads[i].width,
                    dev_monitors_config->heads[i].height);
    }
    heads_size = dev_monitors_config->count * sizeof(QXLHead);
    worker->monitors_config = monitors_config =
        spice_malloc(sizeof(*monitors_config) + heads_size);
    monitors_config->refs = 1;
    monitors_config->worker = worker;
    monitors_config->count = dev_monitors_config->count;
    monitors_config->max_allowed = dev_monitors_config->max_allowed;
    memcpy(monitors_config->heads, dev_monitors_config->heads, heads_size);
}

static void red_push_monitors_config(DisplayChannelClient *dcc)
{
    MonitorsConfig *monitors_config = DCC_TO_WORKER(dcc)->monitors_config;

    if (monitors_config == NULL) {
        spice_warning("monitors_config is NULL");
        return;
    }

    if (!red_channel_client_test_remote_cap(&dcc->common.base,
                                            SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
        return;
    }
    red_monitors_config_item_add(dcc);
    red_channel_client_push(&dcc->common.base);
}

static void red_worker_push_monitors_config(RedWorker *worker)
{
    DisplayChannelClient *dcc;
    RingItem *item, *next;

    WORKER_FOREACH_DCC_SAFE(worker, item, next, dcc) {
        red_push_monitors_config(dcc);
    }
}

static void set_monitors_config_to_primary(RedWorker *worker)
{
    QXLHead *head;
    DrawContext *context;

    if (!worker->surfaces[0].context.canvas) {
        spice_warning("no primary surface");
        return;
    }
    monitors_config_decref(worker->monitors_config);
    context = &worker->surfaces[0].context;
    worker->monitors_config =
        spice_malloc(sizeof(*worker->monitors_config) + sizeof(QXLHead));
    worker->monitors_config->refs = 1;
    worker->monitors_config->worker = worker;
    worker->monitors_config->count = 1;
    worker->monitors_config->max_allowed = 1;
    head = worker->monitors_config->heads;
    head->id = 0;
    head->surface_id = 0;
    head->width = context->width;
    head->height = context->height;
    head->x = 0;
    head->y = 0;
}

static void dev_create_primary_surface(RedWorker *worker, uint32_t surface_id,
                                       QXLDevSurfaceCreate surface)
{
    uint8_t *line_0;
    int error;

    spice_debug(NULL);
    spice_warn_if(surface_id != 0);
    spice_warn_if(surface.height == 0);
    spice_warn_if(((uint64_t)abs(surface.stride) * (uint64_t)surface.height) !=
             abs(surface.stride) * surface.height);

    line_0 = (uint8_t*)get_virt(&worker->mem_slots, surface.mem,
                                surface.height * abs(surface.stride),
                                surface.group_id, &error);
    if (error) {
        return;
    }
    if (surface.stride < 0) {
        line_0 -= (int32_t)(surface.stride * (surface.height -1));
    }

    red_create_surface(worker, 0, surface.width, surface.height, surface.stride, surface.format,
                       line_0, surface.flags & QXL_SURF_FLAG_KEEP_DATA, TRUE);
    set_monitors_config_to_primary(worker);

    if (display_is_connected(worker) && !worker->display_channel->common.during_target_migrate) {
        /* guest created primary, so it will (hopefully) send a monitors_config
         * now, don't send our own temporary one */
        if (!worker->driver_cap_monitors_config) {
            red_worker_push_monitors_config(worker);
        }
        red_pipes_add_verb(&worker->display_channel->common.base,
                           SPICE_MSG_DISPLAY_MARK);
        red_channel_push(&worker->display_channel->common.base);
    }

    if (cursor_is_connected(worker) && !worker->cursor_channel->common.during_target_migrate) {
        red_channel_pipes_add_type(&worker->cursor_channel->common.base,
                                   PIPE_ITEM_TYPE_CURSOR_INIT);
    }
}

void handle_dev_create_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurface *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

static void dev_destroy_primary_surface(RedWorker *worker, uint32_t surface_id)
{
    spice_warn_if(surface_id != 0);

    spice_debug(NULL);
    if (!worker->surfaces[surface_id].context.canvas) {
        spice_warning("double destroy of primary surface");
        return;
    }

    flush_all_qxl_commands(worker);
    dev_destroy_surface_wait(worker, 0);
    red_destroy_surface(worker, 0);
    spice_assert(ring_is_empty(&worker->streams));

    spice_assert(!worker->surfaces[surface_id].context.canvas);

    red_cursor_reset(worker);
}

void handle_dev_destroy_primary_surface(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurface *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    dev_destroy_primary_surface(worker, surface_id);
}

void handle_dev_destroy_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroyPrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;
    uint32_t surface_id = msg->surface_id;

    dev_destroy_primary_surface(worker, surface_id);
}

static void flush_all_surfaces(RedWorker *worker)
{
    int x;

    for (x = 0; x < NUM_SURFACES; ++x) {
        if (worker->surfaces[x].context.canvas) {
            red_current_flush(worker, x);
        }
    }
}

static void dev_flush_surfaces(RedWorker *worker)
{
    flush_all_qxl_commands(worker);
    flush_all_surfaces(worker);
}

void handle_dev_flush_surfaces(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_flush_surfaces(worker);
}

void handle_dev_flush_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_flush_surfaces(worker);
}

void handle_dev_stop(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_info("stop");
    spice_assert(worker->running);
    worker->running = FALSE;
    red_display_clear_glz_drawables(worker->display_channel);
    flush_all_surfaces(worker);
    /* todo: when the waiting is expected to take long (slow connection and
     * overloaded pipe), don't wait, and in case of migration,
     * purge the pipe, send destroy_all_surfaces
     * to the client (there is no such message right now), and start
     * from scratch on the destination side */
    if (!red_channel_wait_all_sent(&worker->display_channel->common.base,
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(&worker->display_channel->common.base,
                                  rcc_disconnect_if_pending_send);
    }
    if (!red_channel_wait_all_sent(&worker->cursor_channel->common.base,
                                   DISPLAY_CLIENT_TIMEOUT)) {
        red_channel_apply_clients(&worker->cursor_channel->common.base,
                                  rcc_disconnect_if_pending_send);
    }
}

static int display_channel_wait_for_migrate_data(DisplayChannel *display)
{
    uint64_t end_time = red_now() + DISPLAY_CLIENT_MIGRATE_DATA_TIMEOUT;
    RedChannel *channel = &display->common.base;
    RedChannelClient *rcc;

    spice_debug(NULL);
    spice_assert(channel->clients_num == 1);

    rcc = SPICE_CONTAINEROF(ring_get_head(&channel->clients), RedChannelClient, channel_link);
    spice_assert(red_channel_client_waits_for_migrate_data(rcc));

    for (;;) {
        red_channel_client_receive(rcc);
        if (!red_channel_client_is_connected(rcc)) {
            break;
        }

        if (!red_channel_client_waits_for_migrate_data(rcc)) {
            return TRUE;
        }
        if (red_now() > end_time) {
            spice_warning("timeout");
            red_channel_client_disconnect(rcc);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    return FALSE;
}

void handle_dev_start(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    spice_assert(!worker->running);
    if (worker->cursor_channel) {
        worker->cursor_channel->common.during_target_migrate = FALSE;
    }
    if (worker->display_channel) {
        worker->display_channel->common.during_target_migrate = FALSE;
        if (red_channel_waits_for_migrate_data(&worker->display_channel->common.base)) {
            display_channel_wait_for_migrate_data(worker->display_channel);
        }
    }
    worker->running = TRUE;
    guest_set_client_capabilities(worker);
}

void handle_dev_wakeup(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    clear_bit(RED_WORKER_PENDING_WAKEUP, worker->pending);
    stat_inc_counter(worker->wakeup_counter, 1);
}

void handle_dev_oom(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    RedChannel *display_red_channel = &worker->display_channel->common.base;
    int ring_is_empty;

    spice_assert(worker->running);
    // streams? but without streams also leak
    spice_debug("OOM1 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                worker->drawable_count,
                worker->red_drawable_count,
                worker->glz_drawable_count,
                worker->current_size,
                worker->display_channel ?
                red_channel_sum_pipes_size(display_red_channel) : 0);
    while (red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty)) {
        red_channel_push(&worker->display_channel->common.base);
    }
    if (worker->qxl->st->qif->flush_resources(worker->qxl) == 0) {
        red_free_some(worker);
        worker->qxl->st->qif->flush_resources(worker->qxl);
    }
    spice_debug("OOM2 #draw=%u, #red_draw=%u, #glz_draw=%u current %u pipes %u",
                worker->drawable_count,
                worker->red_drawable_count,
                worker->glz_drawable_count,
                worker->current_size,
                worker->display_channel ?
                red_channel_sum_pipes_size(display_red_channel) : 0);
    clear_bit(RED_WORKER_PENDING_OOM, worker->pending);
}

void handle_dev_reset_cursor(void *opaque, void *payload)
{
    red_cursor_reset((RedWorker *)opaque);
}

void handle_dev_reset_image_cache(void *opaque, void *payload)
{
    image_cache_reset(&((RedWorker *)opaque)->image_cache);
}

void handle_dev_destroy_surface_wait_async(void *opaque, void *payload)
{
    RedWorkerMessageDestroySurfaceWaitAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_destroy_surface_wait(worker, msg->surface_id);
}

void handle_dev_destroy_surfaces_async(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    dev_destroy_surfaces(worker);
}

void handle_dev_create_primary_surface_async(void *opaque, void *payload)
{
    RedWorkerMessageCreatePrimarySurfaceAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_create_primary_surface(worker, msg->surface_id, msg->surface);
}

/* exception for Dispatcher, data going from red_worker to main thread,
 * TODO: use a different dispatcher?
 * TODO: leave direct usage of channel(fd)? It's only used right after the
 * pthread is created, since the channel duration is the lifetime of the spice
 * server. */

void handle_dev_display_channel_create(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    RedChannel *red_channel;
    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    display_channel_create(worker, FALSE);
    red_channel = &worker->display_channel->common.base;
    send_data(worker->channel, &red_channel, sizeof(RedChannel *));
}

void handle_dev_display_connect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    spice_info("connect");
    handle_new_display_channel(worker, client, stream, migration,
                               msg->common_caps, msg->num_common_caps,
                               msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

void handle_dev_display_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageDisplayDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;
    RedWorker *worker = opaque;

    spice_info("disconnect display client");
    spice_assert(rcc);

    guest_set_client_capabilities(worker);

    red_channel_client_disconnect(rcc);
}

void handle_dev_display_migrate(void *opaque, void *payload)
{
    RedWorkerMessageDisplayMigrate *msg = payload;
    RedWorker *worker = opaque;

    RedChannelClient *rcc = msg->rcc;
    spice_info("migrate display client");
    spice_assert(rcc);
    red_migrate_display(worker, rcc);
}

static void handle_dev_monitors_config_async(void *opaque, void *payload)
{
    RedWorkerMessageMonitorsConfigAsync *msg = payload;
    RedWorker *worker = opaque;
    int min_size = sizeof(QXLMonitorsConfig) + sizeof(QXLHead);
    int error;
    QXLMonitorsConfig *dev_monitors_config =
        (QXLMonitorsConfig*)get_virt(&worker->mem_slots, msg->monitors_config,
                                     min_size, msg->group_id, &error);

    if (error) {
        /* TODO: raise guest bug (requires added QXL interface) */
        return;
    }
    worker->driver_cap_monitors_config = 1;
    if (dev_monitors_config->count == 0) {
        spice_warning("ignoring an empty monitors config message from driver");
        return;
    }
    if (dev_monitors_config->count > dev_monitors_config->max_allowed) {
        spice_warning("ignoring malformed monitors_config from driver, "
                      "count > max_allowed %d > %d",
                      dev_monitors_config->count,
                      dev_monitors_config->max_allowed);
        return;
    }
    worker_update_monitors_config(worker, dev_monitors_config);
    red_worker_push_monitors_config(worker);
}

/* TODO: special, perhaps use another dispatcher? */
void handle_dev_cursor_channel_create(void *opaque, void *payload)
{
    RedWorker *worker = opaque;
    RedChannel *red_channel;

    // TODO: handle seemless migration. Temp, setting migrate to FALSE
    cursor_channel_create(worker, FALSE);
    red_channel = &worker->cursor_channel->common.base;
    send_data(worker->channel, &red_channel, sizeof(RedChannel *));
}

void handle_dev_cursor_connect(void *opaque, void *payload)
{
    RedWorkerMessageCursorConnect *msg = payload;
    RedWorker *worker = opaque;
    RedsStream *stream = msg->stream;
    RedClient *client = msg->client;
    int migration = msg->migration;

    spice_info("cursor connect");
    red_connect_cursor(worker, client, stream, migration,
                       msg->common_caps, msg->num_common_caps,
                       msg->caps, msg->num_caps);
    free(msg->caps);
    free(msg->common_caps);
}

void handle_dev_cursor_disconnect(void *opaque, void *payload)
{
    RedWorkerMessageCursorDisconnect *msg = payload;
    RedChannelClient *rcc = msg->rcc;

    spice_info("disconnect cursor client");
    spice_assert(rcc);
    red_channel_client_disconnect(rcc);
}

void handle_dev_cursor_migrate(void *opaque, void *payload)
{
    RedWorkerMessageCursorMigrate *msg = payload;
    RedWorker *worker = opaque;
    RedChannelClient *rcc = msg->rcc;

    spice_info("migrate cursor client");
    spice_assert(rcc);
    red_migrate_cursor(worker, rcc);
}

void handle_dev_set_compression(void *opaque, void *payload)
{
    RedWorkerMessageSetCompression *msg = payload;
    RedWorker *worker = opaque;

    worker->image_compression = msg->image_compression;
    switch (worker->image_compression) {
    case SPICE_IMAGE_COMPRESS_AUTO_LZ:
        spice_info("ic auto_lz");
        break;
    case SPICE_IMAGE_COMPRESS_AUTO_GLZ:
        spice_info("ic auto_glz");
        break;
    case SPICE_IMAGE_COMPRESS_QUIC:
        spice_info("ic quic");
        break;
    case SPICE_IMAGE_COMPRESS_LZ:
        spice_info("ic lz");
        break;
    case SPICE_IMAGE_COMPRESS_GLZ:
        spice_info("ic glz");
        break;
    case SPICE_IMAGE_COMPRESS_OFF:
        spice_info("ic off");
        break;
    default:
        spice_warning("ic invalid");
    }
#ifdef COMPRESS_STAT
    print_compress_stats(worker->display_channel);
    if (worker->display_channel) {
        stat_reset(&worker->display_channel->quic_stat);
        stat_reset(&worker->display_channel->lz_stat);
        stat_reset(&worker->display_channel->glz_stat);
        stat_reset(&worker->display_channel->jpeg_stat);
        stat_reset(&worker->display_channel->zlib_glz_stat);
        stat_reset(&worker->display_channel->jpeg_alpha_stat);
    }
#endif
}

void handle_dev_set_streaming_video(void *opaque, void *payload)
{
    RedWorkerMessageSetStreamingVideo *msg = payload;
    RedWorker *worker = opaque;

    worker->streaming_video = msg->streaming_video;
    spice_assert(worker->streaming_video != STREAM_VIDEO_INVALID);
    switch(worker->streaming_video) {
        case STREAM_VIDEO_ALL:
            spice_info("sv all");
            break;
        case STREAM_VIDEO_FILTER:
            spice_info("sv filter");
            break;
        case STREAM_VIDEO_OFF:
            spice_info("sv off");
            break;
        default:
            spice_warning("sv invalid");
    }
}

void handle_dev_set_mouse_mode(void *opaque, void *payload)
{
    RedWorkerMessageSetMouseMode *msg = payload;
    RedWorker *worker = opaque;

    worker->mouse_mode = msg->mode;
    spice_info("mouse mode %u", worker->mouse_mode);
}

void handle_dev_add_memslot_async(void *opaque, void *payload)
{
    RedWorkerMessageAddMemslotAsync *msg = payload;
    RedWorker *worker = opaque;

    dev_add_memslot(worker, msg->mem_slot);
}

void handle_dev_reset_memslots(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    red_memslot_info_reset(&worker->mem_slots);
}

void handle_dev_driver_unload(void *opaque, void *payload)
{
    RedWorker *worker = opaque;

    worker->driver_cap_monitors_config = 0;
}

static int loadvm_command(RedWorker *worker, QXLCommandExt *ext)
{
    RedCursorCmd *cursor_cmd;
    RedSurfaceCmd *surface_cmd;

    switch (ext->cmd.type) {
    case QXL_CMD_CURSOR:
        cursor_cmd = spice_new0(RedCursorCmd, 1);
        if (red_get_cursor_cmd(&worker->mem_slots, ext->group_id, cursor_cmd, ext->cmd.data)) {
            free(cursor_cmd);
            return FALSE;
        }
        qxl_process_cursor(worker, cursor_cmd, ext->group_id);
        break;
    case QXL_CMD_SURFACE:
        surface_cmd = spice_new0(RedSurfaceCmd, 1);
        if (red_get_surface_cmd(&worker->mem_slots, ext->group_id, surface_cmd, ext->cmd.data)) {
            free(surface_cmd);
            return FALSE;
        }
        red_process_surface(worker, surface_cmd, ext->group_id, TRUE);
        break;
    default:
        spice_warning("unhandled loadvm command type (%d)", ext->cmd.type);
    }

    return TRUE;
}

void handle_dev_loadvm_commands(void *opaque, void *payload)
{
    RedWorkerMessageLoadvmCommands *msg = payload;
    RedWorker *worker = opaque;
    uint32_t i;
    uint32_t count = msg->count;
    QXLCommandExt *ext = msg->ext;

    spice_info("loadvm_commands");
    for (i = 0 ; i < count ; ++i) {
        if (!loadvm_command(worker, &ext[i])) {
            /* XXX allow failure in loadvm? */
            spice_warning("failed loadvm command type (%d)", ext[i].cmd.type);
        }
    }
}

static void worker_handle_dispatcher_async_done(void *opaque,
                                                uint32_t message_type,
                                                void *payload)
{
    RedWorker *worker = opaque;
    RedWorkerMessageAsync *msg_async = payload;

    spice_debug(NULL);
    red_dispatcher_async_complete(worker->red_dispatcher, msg_async->cmd);
}

/* 注册显示通道和光标通道的Dispatcher命令分发函数
   感觉可以用个列表来定义，然后循环注册
*/
static void register_callbacks(Dispatcher *dispatcher)
{
    dispatcher_register_async_done_callback(
                                    dispatcher,
                                    worker_handle_dispatcher_async_done);
	//显示通道客户端连接消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CONNECT,
                                handle_dev_display_connect,
                                sizeof(RedWorkerMessageDisplayConnect),
                                DISPATCHER_NONE);
	//显示通道客户端断开消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_DISCONNECT,
                                handle_dev_display_disconnect,
                                sizeof(RedWorkerMessageDisplayDisconnect),
                                DISPATCHER_ACK);
	//显示通道客户端迁移消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_MIGRATE,
                                handle_dev_display_migrate,
                                sizeof(RedWorkerMessageDisplayMigrate),
                                DISPATCHER_NONE);
	//光标通道客户端连接消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CONNECT,
                                handle_dev_cursor_connect,
                                sizeof(RedWorkerMessageCursorConnect),
                                DISPATCHER_NONE);
	//光标通道客户端断开消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_DISCONNECT,
                                handle_dev_cursor_disconnect,
                                sizeof(RedWorkerMessageCursorDisconnect),
                                DISPATCHER_ACK);
	//光标通道客户端迁移消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_MIGRATE,
                                handle_dev_cursor_migrate,
                                sizeof(RedWorkerMessageCursorMigrate),
                                DISPATCHER_NONE);
	//QXL发送的区域更新消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE,
                                handle_dev_update,
                                sizeof(RedWorkerMessageUpdate),
                                DISPATCHER_ACK);
	//QXL发送的异步区域更新消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_UPDATE_ASYNC,
                                handle_dev_update_async,
                                sizeof(RedWorkerMessageUpdateAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的添加内存槽消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT,
                                handle_dev_add_memslot,
                                sizeof(RedWorkerMessageAddMemslot),
                                DISPATCHER_ACK);
	//QXL发送的异步添加内存槽消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_ADD_MEMSLOT_ASYNC,
                                handle_dev_add_memslot_async,
                                sizeof(RedWorkerMessageAddMemslotAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的删除内存槽消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DEL_MEMSLOT,
                                handle_dev_del_memslot,
                                sizeof(RedWorkerMessageDelMemslot),
                                DISPATCHER_NONE);
	//QXL发送的同步销毁所有SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES,
                                handle_dev_destroy_surfaces,
                                sizeof(RedWorkerMessageDestroySurfaces),
                                DISPATCHER_ACK);
	//QXL发送的异步销毁所有SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACES_ASYNC,
                                handle_dev_destroy_surfaces_async,
                                sizeof(RedWorkerMessageDestroySurfacesAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的同步销毁主SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE,
                                handle_dev_destroy_primary_surface,
                                sizeof(RedWorkerMessageDestroyPrimarySurface),
                                DISPATCHER_ACK);
	//QXL发送的异步销毁主SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_PRIMARY_SURFACE_ASYNC,
                                handle_dev_destroy_primary_surface_async,
                                sizeof(RedWorkerMessageDestroyPrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的异步创建主SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE_ASYNC,
                                handle_dev_create_primary_surface_async,
                                sizeof(RedWorkerMessageCreatePrimarySurfaceAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的同步创建主SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CREATE_PRIMARY_SURFACE,
                                handle_dev_create_primary_surface,
                                sizeof(RedWorkerMessageCreatePrimarySurface),
                                DISPATCHER_ACK);
	//QXL发送的同步重置图像缓存消息 
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_IMAGE_CACHE,
                                handle_dev_reset_image_cache,
                                sizeof(RedWorkerMessageResetImageCache),
                                DISPATCHER_ACK);
	//QXL发送的同步重置光标消息 
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_CURSOR,
                                handle_dev_reset_cursor,
                                sizeof(RedWorkerMessageResetCursor),
                                DISPATCHER_ACK);

	//QXL发送的唤醒消息，不会重复唤醒，因为Dispatcher的pending里记录了状态
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_WAKEUP,
                                handle_dev_wakeup,
                                sizeof(RedWorkerMessageWakeup),
                                DISPATCHER_NONE);
	//QXL发送的OOM消息（OOM：Out Of Memery，内存耗尽）
	//该消息不会重复发送，因为Dispatcher的pending里记录了状态
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_OOM,
                                handle_dev_oom,
                                sizeof(RedWorkerMessageOom),
                                DISPATCHER_NONE);
	//QXL发送的显卡start消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_START,
                                handle_dev_start,
                                sizeof(RedWorkerMessageStart),
                                DISPATCHER_NONE);
	//QXL发送的异步刷新所有SURFACE消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_FLUSH_SURFACES_ASYNC,
                                handle_dev_flush_surfaces_async,
                                sizeof(RedWorkerMessageFlushSurfacesAsync),
                                DISPATCHER_ASYNC);
	//QXL发送的显卡stop消息
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_STOP,
                                handle_dev_stop,
                                sizeof(RedWorkerMessageStop),
                                DISPATCHER_ACK);
	//QXL发送的虚拟机加载命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_LOADVM_COMMANDS,
                                handle_dev_loadvm_commands,
                                sizeof(RedWorkerMessageLoadvmCommands),
                                DISPATCHER_ACK);
	//设置压缩格式命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_COMPRESSION,
                                handle_dev_set_compression,
                                sizeof(RedWorkerMessageSetCompression),
                                DISPATCHER_NONE);
	//设置流视频命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_STREAMING_VIDEO,
                                handle_dev_set_streaming_video,
                                sizeof(RedWorkerMessageSetStreamingVideo),
                                DISPATCHER_NONE);
	//鼠标模式设置命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_SET_MOUSE_MODE,
                                handle_dev_set_mouse_mode,
                                sizeof(RedWorkerMessageSetMouseMode),
                                DISPATCHER_NONE);
	//显示通道创建命令，RedDispatcher构建后就会发送
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DISPLAY_CHANNEL_CREATE,
                                handle_dev_display_channel_create,
                                sizeof(RedWorkerMessageDisplayChannelCreate),
                                DISPATCHER_NONE);
	//光标通道创建命令，RedDispatcher构建后就会发送
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_CURSOR_CHANNEL_CREATE,
                                handle_dev_cursor_channel_create,
                                sizeof(RedWorkerMessageCursorChannelCreate),
                                DISPATCHER_NONE);
	//同步SURFACE销毁命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT,
                                handle_dev_destroy_surface_wait,
                                sizeof(RedWorkerMessageDestroySurfaceWait),
                                DISPATCHER_ACK);
	//异步SURFACE销毁命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DESTROY_SURFACE_WAIT_ASYNC,
                                handle_dev_destroy_surface_wait_async,
                                sizeof(RedWorkerMessageDestroySurfaceWaitAsync),
                                DISPATCHER_ASYNC);
	//重置内存槽命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_RESET_MEMSLOTS,
                                handle_dev_reset_memslots,
                                sizeof(RedWorkerMessageResetMemslots),
                                DISPATCHER_NONE);
	//显示器配置命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_MONITORS_CONFIG_ASYNC,
                                handle_dev_monitors_config_async,
                                sizeof(RedWorkerMessageMonitorsConfigAsync),
                                DISPATCHER_ASYNC);
	//驱动卸载命令
    dispatcher_register_handler(dispatcher,
                                RED_WORKER_MESSAGE_DRIVER_UNLOAD,
                                handle_dev_driver_unload,
                                sizeof(RedWorkerMessageDriverUnload),
                                DISPATCHER_NONE);
}



static void handle_dev_input(int fd, int event, void *opaque)
{
    RedWorker *worker = opaque;

    dispatcher_handle_recv_read(red_dispatcher_get_dispatcher(worker->red_dispatcher));
}

static void red_init(RedWorker *worker, WorkerInitData *init_data)
{
    RedWorkerMessage message;
    Dispatcher *dispatcher;
    int i;

    spice_assert(sizeof(CursorItem) <= QXL_CURSUR_DEVICE_DATA_SIZE);

    memset(worker, 0, sizeof(RedWorker));
    dispatcher = red_dispatcher_get_dispatcher(init_data->red_dispatcher);
    dispatcher_set_opaque(dispatcher, worker);
    worker->red_dispatcher = init_data->red_dispatcher;
    worker->qxl = init_data->qxl;
    worker->id = init_data->id;
    worker->channel = dispatcher_get_recv_fd(dispatcher);
    register_callbacks(dispatcher);
    worker->pending = init_data->pending;
    worker->cursor_visible = TRUE;
    spice_assert(init_data->num_renderers > 0);
    worker->num_renderers = init_data->num_renderers;
    memcpy(worker->renderers, init_data->renderers, sizeof(worker->renderers));
    worker->renderer = RED_RENDERER_INVALID;
    worker->mouse_mode = SPICE_MOUSE_MODE_SERVER;
    worker->image_compression = init_data->image_compression;
    worker->jpeg_state = init_data->jpeg_state;
    worker->zlib_glz_state = init_data->zlib_glz_state;
    worker->streaming_video = init_data->streaming_video;
    worker->driver_cap_monitors_config = 0;
    ring_init(&worker->current_list);
    image_cache_init(&worker->image_cache);
    image_surface_init(worker);
    drawables_init(worker);
    cursor_items_init(worker);
    red_init_streams(worker);
    stat_init(&worker->add_stat, add_stat_name);
    stat_init(&worker->exclude_stat, exclude_stat_name);
    stat_init(&worker->__exclude_stat, __exclude_stat_name);
#ifdef RED_STATISTICS
    char worker_str[20];
    sprintf(worker_str, "display[%d]", worker->id);
    worker->stat = stat_add_node(INVALID_STAT_REF, worker_str, TRUE);
    worker->wakeup_counter = stat_add_counter(worker->stat, "wakeups", TRUE);
    worker->command_counter = stat_add_counter(worker->stat, "commands", TRUE);
#endif
    for (i = 0; i < MAX_EVENT_SOURCES; i++) {
        worker->poll_fds[i].fd = -1;
    }

    worker->poll_fds[0].fd = worker->channel;
    worker->poll_fds[0].events = POLLIN;
    worker->watches[0].worker = worker;
    worker->watches[0].watch_func = handle_dev_input;
    worker->watches[0].watch_func_opaque = worker;

    red_memslot_info_init(&worker->mem_slots,
                          init_data->num_memslots_groups,
                          init_data->num_memslots,
                          init_data->memslot_gen_bits,
                          init_data->memslot_id_bits,
                          init_data->internal_groupslot_id);

    spice_warn_if(init_data->n_surfaces > NUM_SURFACES);
    worker->n_surfaces = init_data->n_surfaces;

    if (!spice_timer_queue_create()) {
        spice_error("failed to create timer queue");
    }
    srand(time(NULL));

    message = RED_WORKER_MESSAGE_READY;
    write_message(worker->channel, &message);
}

static void red_display_cc_free_glz_drawables(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = RCC_TO_DCC(rcc);

    red_display_handle_glz_drawables_to_free(dcc);
}

SPICE_GNUC_NORETURN void *red_worker_main(void *arg)
{
    RedWorker *worker = spice_malloc(sizeof(RedWorker));

    spice_info("begin");
    spice_assert(MAX_PIPE_SIZE > WIDE_CLIENT_ACK_WINDOW &&
           MAX_PIPE_SIZE > NARROW_CLIENT_ACK_WINDOW); //ensure wakeup by ack message

#if  defined(RED_WORKER_STAT) || defined(COMPRESS_STAT)
    if (pthread_getcpuclockid(pthread_self(), &clock_id)) {
        spice_error("pthread_getcpuclockid failed");
    }
#endif

    red_init(worker, (WorkerInitData *)arg);
    red_init_quic(worker);
    red_init_lz(worker);
    red_init_jpeg(worker);
    red_init_zlib(worker);
    worker->event_timeout = INF_EVENT_WAIT;
    for (;;) {
        int i, num_events;
        unsigned int timers_queue_timeout;

        timers_queue_timeout = spice_timer_queue_get_timeout_ms();
        worker->event_timeout = MIN(red_get_streams_timout(worker), worker->event_timeout);
        worker->event_timeout = MIN(timers_queue_timeout, worker->event_timeout);
        num_events = poll(worker->poll_fds, MAX_EVENT_SOURCES, worker->event_timeout);
        red_handle_streams_timout(worker);
        spice_timer_queue_cb();

        if (worker->display_channel) {
            /* during migration, in the dest, the display channel can be initialized
               while the global lz data not since migrate data msg hasn't been
               received yet */
            red_channel_apply_clients(&worker->display_channel->common.base,
                                      red_display_cc_free_glz_drawables);
        }

        worker->event_timeout = INF_EVENT_WAIT;
        if (num_events == -1) {
            if (errno != EINTR) {
                spice_error("poll failed, %s", strerror(errno));
            }
        }

        for (i = 0; i < MAX_EVENT_SOURCES; i++) {
            /* The watch may have been removed by the watch-func from
               another fd (ie a disconnect through the dispatcher),
               in this case watch_func is NULL. */
            if (worker->poll_fds[i].revents && worker->watches[i].watch_func) {
                int events = 0;
                if (worker->poll_fds[i].revents & POLLIN) {
                    events |= SPICE_WATCH_EVENT_READ;
                }
                if (worker->poll_fds[i].revents & POLLOUT) {
                    events |= SPICE_WATCH_EVENT_WRITE;
                }
                worker->watches[i].watch_func(worker->poll_fds[i].fd, events,
                                        worker->watches[i].watch_func_opaque);
            }
        }

        /* Clear the poll_fd for any removed watches, see the comment in
           watch_remove for why we don't do this there. */
        for (i = 0; i < MAX_EVENT_SOURCES; i++) {
            if (!worker->watches[i].watch_func) {
                worker->poll_fds[i].fd = -1;
            }
        }

        if (worker->running) {
            int ring_is_empty;
            red_process_cursor(worker, MAX_PIPE_SIZE, &ring_is_empty);
            red_process_commands(worker, MAX_PIPE_SIZE, &ring_is_empty);
        }
        red_push(worker);
    }
    abort();
}
