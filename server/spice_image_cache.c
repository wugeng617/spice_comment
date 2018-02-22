#include "spice_image_cache.h"

//查找
static ImageCacheItem *image_cache_find(ImageCache *cache, uint64_t id)
{
    ImageCacheItem *item = cache->hash_table[id % IMAGE_CACHE_HASH_SIZE];

    while (item) {
        if (item->id == id) {
            return item;
        }
        item = item->next;
    }
    return NULL;
}

//每次命中都更新lru，返回是否命中
int image_cache_hit(ImageCache *cache, uint64_t id)
{
    ImageCacheItem *item;
    if (!(item = image_cache_find(cache, id))) {//没找到，则没命中
        return FALSE;
    }
	//找到了，更新年龄，更新lru, 返回命中
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#endif
    ring_remove(&item->lru_link);
    ring_add(&cache->lru, &item->lru_link);
    return TRUE;
}

//干掉缓存中的缓存项
static void image_cache_remove(ImageCache *cache, ImageCacheItem *item)
{
    ImageCacheItem **now;

    now = &cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    for (;;) {
        spice_assert(*now);
        if (*now == item) { //移出哈希桶
            *now = item->next;
            break;
        }
        now = &(*now)->next;
    }
    ring_remove(&item->lru_link); //干掉lru
    pixman_image_unref(item->image);//释放图像
    free(item);
#ifndef IMAGE_CACHE_AGE
    cache->num_items--;
#endif
}

#define IMAGE_CACHE_MAX_ITEMS 2

//将图像加入缓存
static void image_cache_put(SpiceImageCache *spice_cache, uint64_t id, pixman_image_t *image)
{
    ImageCache *cache = (ImageCache *)spice_cache;
    ImageCacheItem *item;

#ifndef IMAGE_CACHE_AGE
    if (cache->num_items == IMAGE_CACHE_MAX_ITEMS) { //满了则删除lru最末端缓存项
        ImageCacheItem *tail = (ImageCacheItem *)ring_get_tail(&cache->lru);
        spice_assert(tail);
        image_cache_remove(cache, tail);
    }
#endif
	//新建缓存项
    item = spice_new(ImageCacheItem, 1);
    item->id = id;//id
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;//去age
#else
    cache->num_items++;//缓存项++
#endif
    item->image = pixman_image_ref(image);//引用图像
    ring_item_init(&item->lru_link);

	//添加到哈希桶首部
    item->next = cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE] = item;
	//添加到lru首部
    ring_add(&cache->lru, &item->lru_link);
}

//根据id返回缓存项，返回的图像增加引用
static pixman_image_t *image_cache_get(SpiceImageCache *spice_cache, uint64_t id)
{
    ImageCache *cache = (ImageCache *)spice_cache;

    ImageCacheItem *item = image_cache_find(cache, id);
    if (!item) {
        spice_error("not found");
    }
    return pixman_image_ref(item->image);
}

//初始化，定义ops
void image_cache_init(ImageCache *cache)
{
    static SpiceImageCacheOps image_cache_ops = {
        image_cache_put,
        image_cache_get,
    };

    cache->base.ops = &image_cache_ops;
    memset(cache->hash_table, 0, sizeof(cache->hash_table));
    ring_init(&cache->lru);
#ifdef IMAGE_CACHE_AGE
    cache->age = 0;
#else
    cache->num_items = 0;
#endif
}

//清空图像缓存
void image_cache_reset(ImageCache *cache)
{
    ImageCacheItem *item;
	//从头开始干掉缓存项
    while ((item = (ImageCacheItem *)ring_get_head(&cache->lru))) {
        image_cache_remove(cache, item);
    }
#ifdef IMAGE_CACHE_AGE
    cache->age = 0;
#endif
}

#define IMAGE_CACHE_DEPTH 4

//更新图像缓存年龄，将年龄超过4代的缓存项干掉
//没一次执行Drawable渲染到画布，都会更新缓存的年龄
void image_cache_aging(ImageCache *cache)
{
#ifdef IMAGE_CACHE_AGE
    ImageCacheItem *item;

    cache->age++;
    while ((item = (ImageCacheItem *)ring_get_tail(&cache->lru)) &&
           cache->age - item->age > IMAGE_CACHE_DEPTH) {
        image_cache_remove(cache, item);
    }
#endif
}
