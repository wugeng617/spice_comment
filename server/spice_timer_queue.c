/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2013 Red Hat, Inc.

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
#include <config.h>
#include <pthread.h>
#include "red_common.h"
#include "spice_timer_queue.h"
#include "common/ring.h"

static Ring timer_queue_list; /* 全局的定时器队列链表，链接SpiceTimerQueue */
static int queue_count = 0; /*定时器队列长度*/
static pthread_mutex_t queue_list_lock = PTHREAD_MUTEX_INITIALIZER; /*定时器队列锁*/

static void spice_timer_queue_init(void)
{
    ring_init(&timer_queue_list);
}

/* spice定时器 */
struct SpiceTimer {
    RingItem link; /* 插入定时器队列的 timers链表 */
    RingItem active_link; /* 接入到定时器队列的active_timers链表 */

    SpiceTimerFunc func; /* 定时器回调函数 */
    void *opaque; /* 定时器回调函数占位指针 */

    SpiceTimerQueue *queue; /* 定时器所属的定时器队列 */

    int is_active; /* 定时器是否已经激活 */
    uint32_t ms;
    uint64_t expiry_time; /* 超时时间 */
};

/* 定时器队列 */
struct SpiceTimerQueue {
    RingItem link; /* 连接定时器 */
    pthread_t thread; /* 创建定时器队列的线程，作为定时器队列的关键字 */
    Ring timers; /* 定时器队列 */
    Ring active_timers; /* 活动定时器队列 */
};

/* 无锁版的定时器队列查找 */
static SpiceTimerQueue *spice_timer_queue_find(void)
{
    pthread_t self = pthread_self();
    RingItem *queue_item;

    RING_FOREACH(queue_item, &timer_queue_list) {
         SpiceTimerQueue *queue = SPICE_CONTAINEROF(queue_item, SpiceTimerQueue, link);
		 /* 当前线程和定时器队列的创建线程是否一致 */
         if (pthread_equal(self, queue->thread) != 0) {//相同返回当前定时器队列
            return queue;
         }
    }

    return NULL;
}

/* 带锁版的定时器队列查找 */
static SpiceTimerQueue *spice_timer_queue_find_with_lock(void)
{
    SpiceTimerQueue *queue;

    pthread_mutex_lock(&queue_list_lock);
    queue = spice_timer_queue_find();
    pthread_mutex_unlock(&queue_list_lock);
    return queue;
}

/* 在当前线程创建一个定时器队列 */
int spice_timer_queue_create(void)
{
    SpiceTimerQueue *queue;

    pthread_mutex_lock(&queue_list_lock);
    if (queue_count == 0) {
        spice_timer_queue_init();
    }

    if (spice_timer_queue_find() != NULL) {
        spice_printerr("timer queue was already created for the thread");
        return FALSE;
    }

    queue = spice_new0(SpiceTimerQueue, 1);
    queue->thread = pthread_self();
    ring_init(&queue->timers);
    ring_init(&queue->active_timers);

	//加入定时器队列全局链表
    ring_add(&timer_queue_list, &queue->link);
    queue_count++;

    pthread_mutex_unlock(&queue_list_lock);

    return TRUE;
}

/* 销毁调用此函数的线程中的定时器队列，要求此线程创建过定时器队列                                */
void spice_timer_queue_destroy(void)
{
    RingItem *item;
    SpiceTimerQueue *queue;

    pthread_mutex_lock(&queue_list_lock);
    queue = spice_timer_queue_find();

    spice_assert(queue != NULL); //调用者线程必须创建了定时器队列

	/* 清理所有定时器 */
    while ((item = ring_get_head(&queue->timers))) {
        SpiceTimer *timer;

        timer = SPICE_CONTAINEROF(item, SpiceTimer, link);
        spice_timer_remove(timer);
    }

	/* 将定时器队列从 */
    ring_remove(&queue->link);
    free(queue);
    queue_count--;

    pthread_mutex_unlock(&queue_list_lock);
}

/* 添加定时器，要求当前线程必须创建了定时器队列 */
SpiceTimer *spice_timer_queue_add(SpiceTimerFunc func, void *opaque)
{
    SpiceTimer *timer = spice_new0(SpiceTimer, 1);
    SpiceTimerQueue *queue = spice_timer_queue_find_with_lock();

    spice_assert(queue != NULL);

    ring_item_init(&timer->link);
    ring_item_init(&timer->active_link);

    timer->opaque = opaque;
    timer->func = func;
    timer->queue = queue;

    ring_add(&queue->timers, &timer->link);

    return timer;
}

static void _spice_timer_set(SpiceTimer *timer, uint32_t ms, uint64_t now)
{
    RingItem *next_item;
    SpiceTimerQueue *queue;

    if (timer->is_active) { //已经激活的定时器先取消
        spice_timer_cancel(timer);
    }

    queue = timer->queue;
    timer->expiry_time = now + ms;
    timer->ms = ms;
	
	/* 定时器队列执行时间升序排列，所以找第一个比定时器时间后的定时器 */
    RING_FOREACH(next_item, &queue->active_timers) {
        SpiceTimer *next_timer = SPICE_CONTAINEROF(next_item, SpiceTimer, active_link);

        if (timer->expiry_time <= next_timer->expiry_time) {
            break;
        }
    }

	/* 插入定时器 */
    if (next_item) {
        ring_add_before(&timer->active_link, next_item);
    } else {
        ring_add_before(&timer->active_link, &queue->active_timers);
    }
    timer->is_active = TRUE;
}

/* 设置一个定时器的执行时间，只能由定时器队列线程来设置 */
void spice_timer_set(SpiceTimer *timer, uint32_t ms)
{
    struct timespec now;

    spice_assert(pthread_equal(timer->queue->thread, pthread_self()) != 0);

    clock_gettime(CLOCK_MONOTONIC, &now);
    _spice_timer_set(timer, ms,
                     (uint64_t)now.tv_sec * 1000 + (now.tv_nsec / 1000 / 1000));
}

/* 取消一个定时器 */
void spice_timer_cancel(SpiceTimer *timer)
{
	/* 只能由创建定时器队列的线程来取消定时器 */
    spice_assert(pthread_equal(timer->queue->thread, pthread_self()) != 0);

	/* 定时器没有激活，直接返回 */
    if (!ring_item_is_linked(&timer->active_link)) {
        spice_assert(!timer->is_active);
        return;
    }

	/* 将定时器从活动队列移除并设置活动标记 */
    spice_assert(timer->is_active);
    ring_remove(&timer->active_link);
    timer->is_active = FALSE;
}

/* 移除一个定时器 */
void spice_timer_remove(SpiceTimer *timer)
{
	/* 定时器必须已经加入定时器队列 */
    spice_assert(timer->queue);
    spice_assert(ring_item_is_linked(&timer->link));
	/* 只能由定时器队列线程可以移除定时器 */
    spice_assert(pthread_equal(timer->queue->thread, pthread_self()) != 0);

    if (timer->is_active) {
		/* 激活的定时器必定已经加入了定时器队列的活动列表 */
        spice_assert(ring_item_is_linked(&timer->active_link));
        ring_remove(&timer->active_link); /* 从活动队列中摘除 */
    }
	/* 从定时器队列中摘除 */
    ring_remove(&timer->link);
	/* 释放定时器内存 */
    free(timer);
}

/* 获取当前定时器队列最近一个活动定时器距离当前时间的时间差值，单位ms，如果没有活动定时器则返回-1 */
unsigned int spice_timer_queue_get_timeout_ms(void)
{
    struct timespec now;
    int64_t now_ms;
    RingItem *head;
    SpiceTimer *head_timer;
    SpiceTimerQueue *queue = spice_timer_queue_find_with_lock();

    spice_assert(queue != NULL);

    if (ring_is_empty(&queue->active_timers)) {
        return -1;
    }

    head = ring_get_head(&queue->active_timers);
    head_timer = SPICE_CONTAINEROF(head, SpiceTimer, active_link);

    clock_gettime(CLOCK_MONOTONIC, &now);
    now_ms = ((int64_t)now.tv_sec * 1000) - (now.tv_nsec / 1000 / 1000);

    return MAX(0, ((int64_t)head_timer->expiry_time - now_ms));
}

/* 定时器驱动函数，执行定时器里的回调函数，根据spice_timer_queue_get_timeout_ms来不断
的调用 spice_timer_queue_cb函数，就可以让定时器得到执行 */
void spice_timer_queue_cb(void)
{
    struct timespec now;
    uint64_t now_ms;
    RingItem *head;
    SpiceTimerQueue *queue = spice_timer_queue_find_with_lock();

    spice_assert(queue != NULL);

    if (ring_is_empty(&queue->active_timers)) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    now_ms = ((uint64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000 / 1000);

    while ((head = ring_get_head(&queue->active_timers))) {
        SpiceTimer *timer = SPICE_CONTAINEROF(head, SpiceTimer, active_link);

        if (timer->expiry_time > now_ms) {//如果没有到达执行时间则结束定时器的执行
            break;
        } else {
            timer->func(timer->opaque); //执行定时器函数并且取消定时器
            spice_timer_cancel(timer); //定时器只会执行一次定时器回调函数，所以重复定时器需要不断的设置定时器
        }
    }
}
