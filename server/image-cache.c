/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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
#include "image-cache.h"
#include "red-parse-qxl.h"
#include "display-channel.h"

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

static bool image_cache_hit(ImageCache *cache, uint64_t id)
{
    ImageCacheItem *item;
    if (!(item = image_cache_find(cache, id))) {
        return FALSE;
    }
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#endif
    ring_remove(&item->lru_link);
    ring_add(&cache->lru, &item->lru_link);
    return TRUE;
}

static void image_cache_remove(ImageCache *cache, ImageCacheItem *item)
{
    ImageCacheItem **now;

    now = &cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    for (;;) {
        spice_assert(*now);
        if (*now == item) {
            *now = item->next;
            break;
        }
        now = &(*now)->next;
    }
    ring_remove(&item->lru_link);
    pixman_image_unref(item->image);
    g_free(item);
#ifndef IMAGE_CACHE_AGE
    cache->num_items--;
#endif
}

#define IMAGE_CACHE_MAX_ITEMS 2

static void image_cache_put(SpiceImageCache *spice_cache, uint64_t id, pixman_image_t *image)
{
    ImageCache *cache = SPICE_UPCAST(ImageCache, spice_cache);
    ImageCacheItem *item;

#ifndef IMAGE_CACHE_AGE
    if (cache->num_items == IMAGE_CACHE_MAX_ITEMS) {
        SPICE_VERIFY(SPICE_OFFSETOF(ImageCacheItem, lru_link) == 0);
        ImageCacheItem *tail = (ImageCacheItem *)ring_get_tail(&cache->lru);
        spice_assert(tail);
        image_cache_remove(cache, tail);
    }
#endif

    item = g_new(ImageCacheItem, 1);
    item->id = id;
#ifdef IMAGE_CACHE_AGE
    item->age = cache->age;
#else
    cache->num_items++;
#endif
    item->image = pixman_image_ref(image);
    ring_item_init(&item->lru_link);

    item->next = cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE];
    cache->hash_table[item->id % IMAGE_CACHE_HASH_SIZE] = item;

    ring_add(&cache->lru, &item->lru_link);
}

static pixman_image_t *image_cache_get(SpiceImageCache *spice_cache, uint64_t id)
{
    ImageCache *cache = SPICE_UPCAST(ImageCache, spice_cache);

    ImageCacheItem *item = image_cache_find(cache, id);
    if (!item) {
        spice_error("not found");
    }
    return pixman_image_ref(item->image);
}

void image_cache_init(ImageCache *cache)
{
    static const SpiceImageCacheOps image_cache_ops = {
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

void image_cache_reset(ImageCache *cache)
{
    ImageCacheItem *item;

    SPICE_VERIFY(SPICE_OFFSETOF(ImageCacheItem, lru_link) == 0);
    while ((item = (ImageCacheItem *)ring_get_head(&cache->lru))) {
        image_cache_remove(cache, item);
    }
#ifdef IMAGE_CACHE_AGE
    cache->age = 0;
#endif
}

#define IMAGE_CACHE_DEPTH 4

void image_cache_aging(ImageCache *cache)
{
    SPICE_VERIFY(SPICE_OFFSETOF(ImageCacheItem, lru_link) == 0);
#ifdef IMAGE_CACHE_AGE
    ImageCacheItem *item;

    cache->age++;
    while ((item = (ImageCacheItem *)ring_get_tail(&cache->lru)) &&
           cache->age - item->age > IMAGE_CACHE_DEPTH) {
        image_cache_remove(cache, item);
    }
#endif
}

void image_cache_localize(ImageCache *cache, SpiceImage **image_ptr,
                          SpiceImage *image_store, Drawable *drawable)
{
    SpiceImage *image = *image_ptr;

    if (image == NULL) {
        spice_assert(drawable != NULL);
        spice_assert(drawable->red_drawable->self_bitmap_image != NULL);
        *image_ptr = drawable->red_drawable->self_bitmap_image;
        return;
    }

    if (image_cache_hit(cache, image->descriptor.id)) {
        image_store->descriptor = image->descriptor;
        image_store->descriptor.type = SPICE_IMAGE_TYPE_FROM_CACHE;
        image_store->descriptor.flags = 0;
        *image_ptr = image_store;
        return;
    }

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
    case SPICE_IMAGE_TYPE_BITMAP:
    case SPICE_IMAGE_TYPE_SURFACE:
    case SPICE_IMAGE_TYPE_DRM_PRIME:
        /* nothing */
        break;
    default:
        spice_error("invalid image type");
    }
}

void image_cache_localize_brush(ImageCache *cache, SpiceBrush *brush, SpiceImage *image_store)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        image_cache_localize(cache, &brush->u.pattern.pat, image_store, NULL);
    }
}

void image_cache_localize_mask(ImageCache *cache, SpiceQMask *mask, SpiceImage *image_store)
{
    if (mask->bitmap) {
        image_cache_localize(cache, &mask->bitmap, image_store, NULL);
    }
}
