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

#ifndef DCC_PRIVATE_H_
#define DCC_PRIVATE_H_

#include "cache-item.h"
#include "dcc.h"
#include "image-encoders.h"
#include "stream.h"
#include "red-channel-client.h"

struct DisplayChannelClientPrivate
{
    uint32_t id;
    SpiceImageCompression image_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    ImageEncoders encoders;

    int expect_init;

    PixmapCache *pixmap_cache;
    uint32_t pixmap_cache_generation;
    int pending_pixmaps_sync;

    RedCacheItem *palette_cache[PALETTE_CACHE_HASH_SIZE];
    Ring palette_cache_lru;
    long palette_cache_available;
    uint32_t palette_cache_items;

    struct {
        FreeList free_list;
        uint64_t pixmap_cache_items[MAX_DRAWABLE_PIXMAP_CACHE_ITEMS];
        int num_pixmap_cache_items;
    } send_data;

    /* Host preferred video-codec order sorted with client preferred */
    GArray *preferred_video_codecs;
    /* Array with SPICE_VIDEO_CODEC_TYPE_ENUM_END elements, with the client
     * preference order (index) as value */
    GArray *client_preferred_video_codecs;

    uint8_t surface_client_created[NUM_SURFACES];
    QRegion surface_client_lossy_region[NUM_SURFACES];

    StreamAgent stream_agents[NUM_STREAMS];
    uint32_t streams_max_latency;
    uint64_t streams_max_bit_rate;
    bool gl_draw_ongoing;
};

#endif /* DCC_PRIVATE_H_ */
