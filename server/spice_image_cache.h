#ifndef H_SPICE_IMAGE_CACHE
#define H_SPICE_IMAGE_CACHE

#include <inttypes.h>

#include "common/pixman_utils.h"
#include "common/canvas_base.h"

#include "common/ring.h"

//图像缓存哈希表项
typedef struct ImageCacheItem {
    RingItem lru_link; //LRU链表，用于缓存替换
    uint64_t id; //id
#ifdef IMAGE_CACHE_AGE
    uint32_t age; //年龄
#endif
    struct ImageCacheItem *next; //接入哈系统链表
    pixman_image_t *image; //缓存pixman图像
} ImageCacheItem;

#define IMAGE_CACHE_HASH_SIZE 1024 //哈希桶长1024

//图像缓存
typedef struct ImageCache {
    SpiceImageCache base;
	//图像缓存项哈希表
    ImageCacheItem *hash_table[IMAGE_CACHE_HASH_SIZE];
    Ring lru; //lru链表
#ifdef IMAGE_CACHE_AGE
    uint32_t age; //年龄
#else
    uint32_t num_items; //项数
#endif
} ImageCache;

int image_cache_hit(ImageCache *cache, uint64_t id);
void image_cache_init(ImageCache *cache);
void image_cache_reset(ImageCache *cache);
void image_cache_aging(ImageCache *cache);

#endif
