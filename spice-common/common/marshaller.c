/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

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

#include "marshaller.h"
#include "mem.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#ifdef WORDS_BIGENDIAN
#define write_int8(ptr,v) (*((int8_t *)(ptr)) = v)
#define write_uint8(ptr,v) (*((uint8_t *)(ptr)) = v)
#define write_int16(ptr,v) (*((int16_t *)(ptr)) = SPICE_BYTESWAP16((uint16_t)(v)))
#define write_uint16(ptr,v) (*((uint16_t *)(ptr)) = SPICE_BYTESWAP16((uint16_t)(v)))
#define write_int32(ptr,v) (*((int32_t *)(ptr)) = SPICE_BYTESWAP32((uint32_t)(v)))
#define write_uint32(ptr,v) (*((uint32_t *)(ptr)) = SPICE_BYTESWAP32((uint32_t)(v)))
#define write_int64(ptr,v) (*((int64_t *)(ptr)) = SPICE_BYTESWAP64((uint64_t)(v)))
#define write_uint64(ptr,v) (*((uint64_t *)(ptr)) = SPICE_BYTESWAP64((uint64_t)(v)))
#else
#define write_int8(ptr,v) (*((int8_t *)(ptr)) = v)
#define write_uint8(ptr,v) (*((uint8_t *)(ptr)) = v)
#define write_int16(ptr,v) (*((int16_t *)(ptr)) = v)
#define write_uint16(ptr,v) (*((uint16_t *)(ptr)) = v)
#define write_int32(ptr,v) (*((int32_t *)(ptr)) = v)
#define write_uint32(ptr,v) (*((uint32_t *)(ptr)) = v)
#define write_int64(ptr,v) (*((int64_t *)(ptr)) = v)
#define write_uint64(ptr,v) (*((uint64_t *)(ptr)) = v)
#endif

/* Marshaller条目 */
typedef struct {
    uint8_t *data; //数据地址
    size_t len; //数据长度
    spice_marshaller_item_free_func free_data; //Marshaller用完之后的数据释放回调
    void *opaque; //回调函数占位指针
} MarshallerItem;

/* Try to fit in 4k page with 2*pointer-size overhead (next ptr and malloc size) */
#define MARSHALLER_BUFFER_SIZE (4096 - sizeof(void *) * 2)

/* MarshallerBuffer是个链表，每个buf都是4k */
typedef struct MarshallerBuffer MarshallerBuffer;
struct MarshallerBuffer {
    MarshallerBuffer *next;
    uint8_t data[MARSHALLER_BUFFER_SIZE];
};

#define N_STATIC_ITEMS 4

typedef struct SpiceMarshallerData SpiceMarshallerData;

typedef struct {
    SpiceMarshaller *marshaller; //一般指向父Mashaller
    int item_nr; //条目槽号，也就是指针空间分配的条目槽号
    int is_64bit; //指针是否为64位
    size_t offset; //偏移，条目内存块内的地址偏移
} MarshallerRef;

struct SpiceMarshaller {
    size_t total_size; //本mashaller的数据量
    SpiceMarshallerData *data; //数据分配器
    SpiceMarshaller *next; //链表

    MarshallerRef pointer_ref; //Mashaller引用，子Mashaller引用父Mashaller

    int n_items; /* 下一个item的分配位置 */
    int items_size; /* number of items availible in items，item缓冲区总数 */
    MarshallerItem *items; /* item缓冲区指针，开始时指向预分配的static_items */

    MarshallerItem static_items[N_STATIC_ITEMS];
};

/* 一个Mashaller链表的所有小内存块，都从MarshallerData的buffers链表分配 */
struct SpiceMarshallerData {
    size_t total_size;
    size_t base;
    SpiceMarshaller *marshallers; //指向rootMashaller
    SpiceMarshaller *last_marshaller;

    size_t current_buffer_position; //当前buffer分配的位置
    MarshallerBuffer *current_buffer; //当前buffer分配至指针
    
    // 一个MashallerBuffer可能跟多个 MashallerItem关联，
    MarshallerItem *current_buffer_item; //当前buffer关联的Marshaller条目
    MarshallerBuffer *buffers; //buffer列表，取staticbuffer的值，
    //从而减少分配头结点和NULL头结点检查，每个Marshaller分配接近4K的空间
    //buffers持有的buffer列表在RootMashaller释放时并不会释放，而会留下来等到
    //整个RootMashaller destroy时才释放。

    SpiceMarshaller static_marshaller;
    MarshallerBuffer static_buffer; //
};

static void spice_marshaller_init(SpiceMarshaller *m,
                                  SpiceMarshallerData *data)
{
    m->data = data; //关联data
    m->next = NULL; //Marshaller链表为空
    m->total_size = 0; //总大小
    m->pointer_ref.marshaller = NULL; //引用的Marshaller
    m->n_items = 0; //初始分配位置
    m->items_size = N_STATIC_ITEMS; //条目缓冲区大小
    m->items = m->static_items; //条目缓冲区分配指针
}

// 创建一个Root SpiceMarshaller， RootMashaller时MarshallerData中包含的Masheller
SpiceMarshaller *spice_marshaller_new(void)
{
    SpiceMarshallerData *d;
    SpiceMarshaller *m;

	// 先new一个SpiceMarshallerData
    d = spice_new(SpiceMarshallerData, 1);

	// 初始化SpiceMarshallerData
    d->last_marshaller = d->marshallers = &d->static_marshaller;
    d->total_size = 0;
    d->base = 0;
    d->buffers = &d->static_buffer;
    d->buffers->next = NULL;
    d->current_buffer = d->buffers;
    d->current_buffer_position = 0;
    d->current_buffer_item = NULL;

    m = &d->static_marshaller; //取MashallerData的静态Marshaller，并和MashallerData建立关联
    spice_marshaller_init(m, d);

    return m;
}

// 释放Marsheller中所有条目持有的内存
static void free_item_data(SpiceMarshaller *m)
{
    MarshallerItem *item;
    int i;

    /* Free all user data */
    for (i = 0; i < m->n_items; i++) {
        item = &m->items[i];
        if (item->free_data != NULL) {
            item->free_data(item->data, item->opaque);
        }
    }
}

// 释放Mashaller中条目内存
static void free_items(SpiceMarshaller *m)
{
    if (m->items != m->static_items) {
        free(m->items);
    }
}

// 重置一个RootMashaller
void spice_marshaller_reset(SpiceMarshaller *m)
{
    SpiceMarshaller *m2, *next;
    SpiceMarshallerData *d;

    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    for (m2 = m; m2 != NULL; m2 = next) { //从RootMashaller向后迭代
        next = m2->next;
        free_item_data(m2); //释放条目持有的内存
        //rootMashaller不会分配Marshaller条目？

        /* Free non-root marshallers */
        if (m2 != m) {//对于非rootMashaller，释放掉条目内存和Mashaller本身
            free_items(m2);
            free(m2);
        }
    }

    m->next = NULL; //没有Mashaller链表了
    m->n_items = 0; 
    m->total_size = 0;

	//重置RootMashaller的MarshallerData
    d = m->data;
    d->last_marshaller = d->marshallers; //都指向RootMashaller
    d->total_size = 0; //总大小重置
    d->base = 0; //base重置
    d->current_buffer_item = NULL; 
    d->current_buffer = d->buffers; //SpiceMarshallData已经分配的buffer不会释放
    //用于新分配时使用
    d->current_buffer_position = 0;
}

// 销毁一个RootMashaller
void spice_marshaller_destroy(SpiceMarshaller *m)
{
    MarshallerBuffer *buf, *next;
    SpiceMarshallerData *d;

    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    spice_marshaller_reset(m);

    free_items(m); //为什么重置时不释放RootMashaller的条目内存

    d = m->data;

    buf = d->buffers->next; //d->buffers指向静态buffer，所以从下一个开始迭代
    while (buf != NULL) {
        next = buf->next; //先保存下一个的地址
        free(buf); //释放MashallerBuffer
        buf = next;
    }

    free(d); //释放整个MarshallerData及其内含的Mashaller内存
}

// 从Marshaller中分配一个MarshallerItem
static MarshallerItem *spice_marshaller_add_item(SpiceMarshaller *m)
{
    MarshallerItem *item;

    if (m->n_items == m->items_size) {//缓冲区已经用完，指向缓冲区之后了
        int items_size = m->items_size * 2; //2倍指数扩展

        if (m->items == m->static_items) { //静态缓冲区用完的情况
            m->items = spice_new(MarshallerItem, items_size);
            memcpy(m->items, m->static_items, sizeof(MarshallerItem) * m->n_items);
        } else {
            m->items = spice_renew(MarshallerItem, m->items, items_size);
        }
        m->items_size = items_size;
    }
    item = &m->items[m->n_items++]; //n_items指向下一个分配的位置
    item->free_data = NULL;

    return item;
}

// 返回当前buffer剩余的空间
static size_t remaining_buffer_size(SpiceMarshallerData *d)
{
    return MARSHALLER_BUFFER_SIZE - d->current_buffer_position;
}

// 让Mashaller分配并持有一块内存空间，返回内存的起始地址
uint8_t *spice_marshaller_reserve_space(SpiceMarshaller *m, size_t size)
{
    MarshallerItem *item;
    SpiceMarshallerData *d;
    uint8_t *res;

    if (size == 0) { //异常参数，返回空
        return NULL;
    }

	//取数据分配器
    d = m->data;

    /* Check current item, 先看当前item有没有空间 */
    item = &m->items[m->n_items - 1]; 
    if (item == d->current_buffer_item && 
		//因为大内存对应的条目不从MashallerData的buffers分配，所以会有item!=d->current_buffer_item
		//的情况，item永远取的是最近分配内存的条目项，而这个条目项可能记录的是大内存的分配。而d->
		//cuurent_buffer_item永远记录的是
        remaining_buffer_size(d) >= size) {//剩余空间比分配空间大
        assert(m->n_items >= 1);
        /* We can piggy back on existing item+buffer */
        res = item->data + item->len;
        item->len += size; //条目的持有空间增加
        d->current_buffer_position += size; //data的分配空间藏家
        d->total_size += size; //总数据增加
        m->total_size += size; //mashaller的数据增加
        return res;
    }

	// 如果当前分配条目和当前的buffer关联的条目不一致
	// 或者分配条目对应的buffer空间不够了

	// 先创建一个分配条目
    item = spice_marshaller_add_item(m);

	// 如果数据缓冲区的剩余空间超过size，则条目从buffer偏移分配空间
    if (remaining_buffer_size(d) >= size) {
        /* Fits in current buffer */
        item->data = d->current_buffer->data + d->current_buffer_position;
        item->len = size;//item直接取分配大小
        d->current_buffer_position += size; //buffer占用自增
        d->current_buffer_item = item; //buffer的item指向新的item
    } else if (size > MARSHALLER_BUFFER_SIZE / 2) { //对于size超过内存块1半的请求
        /* Large item, allocate by itself */
        item->data = (uint8_t *)spice_malloc(size); //用malloc来分配内存块
        item->len = size;
        item->free_data = (spice_marshaller_item_free_func)free;
        item->opaque = NULL;
		//大内存不是从d->current_buffer_item
    } else { //当前分配的buffer区空间不足且分配的是2K以内的内存块请求
    	// 从下一个buffer分配内存
        /* Use next buffer */
        if (d->current_buffer->next == NULL) {//如果下一块buffer还没分配，先malloc分配。
            d->current_buffer->next = spice_new(MarshallerBuffer, 1);//分配并插入链表
            d->current_buffer->next->next = NULL; 
        }//当Marshaller
        d->current_buffer = d->current_buffer->next;//当前分配buffer指向新buffer
        d->current_buffer_position = size; //从从0开始分配
        d->current_buffer_item = item; //记录当前分配的item
        item->data = d->current_buffer->data; //持有内存
        item->len = size; //记录有效长度
    }

    d->total_size += size; //增加数据量
    m->total_size += size; //增加mashaller数据量
    return item->data;
}

// 从Mashaller的最近分配条目中归还size空间。size不能比条目中的空间大，
// 即不能跨条目归还
void spice_marshaller_unreserve_space(SpiceMarshaller *m, size_t size)
{
    MarshallerItem *item;

    if (size == 0) {
        return;
    }

	// 取前一个条目
    item = &m->items[m->n_items - 1];

	// 不能跨item释放空间
    assert(item->len >= size);
    item->len -= size;
}

//给Marshaller添加一个数据条目，数据条目完全引用参数内存，并记录释放函数
uint8_t *spice_marshaller_add_ref_full(SpiceMarshaller *m, uint8_t *data, size_t size,
                                       spice_marshaller_item_free_func free_data, void *opaque)
{
    MarshallerItem *item;
    SpiceMarshallerData *d;

    if (data == NULL || size == 0) {
        return NULL;
    }

    item = spice_marshaller_add_item(m);
    item->data = data;
    item->len = size;
    item->free_data = free_data;
    item->opaque = opaque;

    d = m->data;
    m->total_size += size;
    d->total_size += size;

    return data;
}

// 让Mashaller分配size大小的内存空间，然后把数据copy到分配的空间里
uint8_t *spice_marshaller_add(SpiceMarshaller *m, const uint8_t *data, size_t size)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, size);
    memcpy(ptr, data, size);
    return ptr;
}

/* 让SpiceMarshaller分配一个MarshellerItem，并持有一块内存，这块内存需要一直有效,
   Marshaller不会释放这块内存
*/
uint8_t *spice_marshaller_add_ref(SpiceMarshaller *m, uint8_t *data, size_t size)
{
    return spice_marshaller_add_ref_full(m, data, size, NULL, NULL);
}

void spice_marshaller_add_ref_chunks(SpiceMarshaller *m, SpiceChunks *chunks)
{
    unsigned int i;

    for (i = 0; i < chunks->num_chunks; i++) {
        spice_marshaller_add_ref(m, chunks->chunk[i].data,
                                 chunks->chunk[i].len);
    }
}

// 分配一个子Mashaller，连接到最后一个Mashaller之后
SpiceMarshaller *spice_marshaller_get_submarshaller(SpiceMarshaller *m)
{
    SpiceMarshallerData *d;
    SpiceMarshaller *m2;

    d = m->data;

    m2 = spice_new(SpiceMarshaller, 1);
    spice_marshaller_init(m2, d);

    d->last_marshaller->next = m2;
    d->last_marshaller = m2;

    return m2;
}

// 给Mashaller添加一个指针，但是只把分配的指针空间设置为0，并且增加一个子mashaller
SpiceMarshaller *spice_marshaller_get_ptr_submarshaller(SpiceMarshaller *m, int is_64bit)
{
    SpiceMarshaller *m2;
    uint8_t *p;
    int size;

    size = is_64bit ? 8 : 4;

    p = spice_marshaller_reserve_space(m, size);
    memset(p, 0, size);
    m2 = spice_marshaller_get_submarshaller(m);
    m2->pointer_ref.marshaller = m;
    m2->pointer_ref.item_nr = m->n_items - 1;
    m2->pointer_ref.offset = m->items[m->n_items - 1].len - size;//偏移是槽位内存内的偏移
    m2->pointer_ref.is_64bit = is_64bit;

    return m2;
}

// 取出MashallerRef在父Mashaller中的存储位置。
static uint8_t *lookup_ref(MarshallerRef *ref)
{
    MarshallerItem *item;

    item = &ref->marshaller->items[ref->item_nr];
    return item->data + ref->offset;
}

// 设置RootMashller的基本数据偏移，也就是需要跳过头部数据的跳跃量
void spice_marshaller_set_base(SpiceMarshaller *m, size_t base)
{
    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    m->data->base = base;
}

uint8_t *spice_marshaller_linearize(SpiceMarshaller *m, size_t skip_bytes,
                                    size_t *len, int *free_res)
{
    MarshallerItem *item;
    uint8_t *res, *p;
    int i;

    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    if (m->n_items == 1) {
        *free_res = FALSE;
        if (m->items[0].len <= skip_bytes) {
            *len = 0;
            return NULL;
        }
        *len = m->items[0].len - skip_bytes;
        return m->items[0].data + skip_bytes;
    }

    *free_res = TRUE;
    res = (uint8_t *)spice_malloc(m->data->total_size - skip_bytes);
    *len = m->data->total_size - skip_bytes;
    p = res;

    do {
        for (i = 0; i < m->n_items; i++) {
            item = &m->items[i];

            if (item->len <= skip_bytes) {
                skip_bytes -= item->len;
                continue;
            }
            memcpy(p, item->data + skip_bytes, item->len - skip_bytes);
            p += item->len - skip_bytes;
            skip_bytes = 0;
        }
        m = m->next;
    } while (m != NULL);

    return res;
}

uint8_t *spice_marshaller_get_ptr(SpiceMarshaller *m)
{
    return m->items[0].data;
}

// 求M这个Mashaller数据块起始位置距离base的偏移
size_t spice_marshaller_get_offset(SpiceMarshaller *m)
{
    SpiceMarshaller *m2;
    size_t offset;

    offset = 0;
	//从rootMashaller开始迭代，先求m之前所有Mashller的总大小
    m2 = m->data->marshallers;
    while (m2 != m) {
        offset += m2->total_size;
        m2 = m2->next;
    }
    return offset - m->data->base;
}

// 返回一个Mashaller的数据大小
size_t spice_marshaller_get_size(SpiceMarshaller *m)
{
    return m->total_size;
}

// 返回整个RootMashaller的数据总大小
size_t spice_marshaller_get_total_size(SpiceMarshaller *m)
{
    return m->data->total_size;
}

// RootMashaller的所有Mashaller都填充数据之后，填充之前的指针对象
// 经过Mashaller之后的偏移位置。也就是之前消息中的指针空间的值转化为子Mashaller
// 在消息序列中的偏移位置。
void spice_marshaller_flush(SpiceMarshaller *m)
{
    SpiceMarshaller *m2;
    uint8_t *ptr_pos;

    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    for (m2 = m; m2 != NULL; m2 = m2->next) {
        if (m2->pointer_ref.marshaller != NULL && m2->total_size > 0) {
            ptr_pos = lookup_ref(&m2->pointer_ref);
            if (m2->pointer_ref.is_64bit) {
                write_uint64(ptr_pos,
                             spice_marshaller_get_offset(m2));
            } else {
                write_uint32(ptr_pos,
                             spice_marshaller_get_offset(m2));
            }
        }
    }
}

#ifndef WIN32
// 
int spice_marshaller_fill_iovec(SpiceMarshaller *m, struct iovec *vec,
                                int n_vec, size_t skip_bytes)
{
    MarshallerItem *item;
    int v, i;

    /* Only supported for root marshaller */
    assert(m->data->marshallers == m);

    v = 0;
    do {
        for (i = 0; i < m->n_items; i++) {
            item = &m->items[i];

            if (item->len <= skip_bytes) {
                skip_bytes -= item->len;
                continue;
            }
            if (v == n_vec) {
                return v; /* Not enough space in vec */
            }
            vec[v].iov_base = (uint8_t *)item->data + skip_bytes;
            vec[v].iov_len = item->len - skip_bytes;
            skip_bytes = 0;
            v++;
        }
        m = m->next;
    } while (m != NULL);

    return v;
}
#endif

void *spice_marshaller_add_uint64(SpiceMarshaller *m, uint64_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(uint64_t));
    write_uint64(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_int64(SpiceMarshaller *m, int64_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(int64_t));
    write_int64(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_uint32(SpiceMarshaller *m, uint32_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(uint32_t));
    write_uint32(ptr, v);
    return (void *)ptr;
}

void spice_marshaller_set_uint32(SpiceMarshaller *m, void *ref, uint32_t v)
{
    write_uint32((uint8_t *)ref, v);
}

void *spice_marshaller_add_int32(SpiceMarshaller *m, int32_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(int32_t));
    write_int32(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_uint16(SpiceMarshaller *m, uint16_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(uint16_t));
    write_uint16(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_int16(SpiceMarshaller *m, int16_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(int16_t));
    write_int16(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_uint8(SpiceMarshaller *m, uint8_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(uint8_t));
    write_uint8(ptr, v);
    return (void *)ptr;
}

void *spice_marshaller_add_int8(SpiceMarshaller *m, int8_t v)
{
    uint8_t *ptr;

    ptr = spice_marshaller_reserve_space(m, sizeof(int8_t));
    write_int8(ptr, v);
    return (void *)ptr;
}
