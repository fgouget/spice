/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

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

#include <inttypes.h>
#include <glib.h>
#include <common/lz_common.h>
#include "spice-bitmap-utils.h"
#include "red-common.h"
#include "red-qxl.h"
#include "memslot.h"
#include "red-parse-qxl.h"

/* Max size in bytes for any data field used in a QXL command.
 * This will for example be useful to prevent the guest from saturating the
 * host memory if it tries to send overlapping chunks.
 * This value should be big enough for all requests but limited
 * to 32 bits. Even better if it fits on 31 bits to detect integer overflows.
 */
#define MAX_DATA_CHUNK 0x7ffffffflu

verify(MAX_DATA_CHUNK <= G_MAXINT32);

/* Limit number of chunks.
 * The guest can attempt to make host allocate too much memory
 * just with a large number of small chunks.
 * Prevent that the chunk list take more memory than the data itself.
 */
#define MAX_CHUNKS (MAX_DATA_CHUNK/1024u)

#define INVALID_SIZE ((size_t) -1)

typedef struct RedDataChunk RedDataChunk;
struct RedDataChunk {
    uint32_t data_size;
    RedDataChunk *prev_chunk;
    RedDataChunk *next_chunk;
    uint8_t *data;
};

#if 0
static void hexdump_qxl(RedMemSlotInfo *slots, int group_id,
                        QXLPHYSICAL addr, uint8_t bytes)
{
    uint8_t *hex;
    int i;

    hex = (uint8_t*)memslot_get_virt(slots, addr, bytes, group_id);
    for (i = 0; i < bytes; i++) {
        if (0 == i % 16) {
            fprintf(stderr, "%lx: ", addr+i);
        }
        if (0 == i % 4) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, " %02x", hex[i]);
        if (15 == i % 16) {
            fprintf(stderr, "\n");
        }
    }
}
#endif

static inline uint32_t color_16_to_32(uint32_t color)
{
    uint32_t ret;

    ret = ((color & 0x001f) << 3) | ((color & 0x001c) >> 2);
    ret |= ((color & 0x03e0) << 6) | ((color & 0x0380) << 1);
    ret |= ((color & 0x7c00) << 9) | ((color & 0x7000) << 4);

    return ret;
}

static uint8_t *red_linearize_chunk(RedDataChunk *head, size_t size, bool *free_chunk)
{
    uint8_t *data, *ptr;
    RedDataChunk *chunk;
    uint32_t copy;

    if (head->next_chunk == NULL) {
        spice_assert(size <= head->data_size);
        *free_chunk = false;
        return head->data;
    }

    ptr = data = g_malloc(size);
    *free_chunk = true;
    for (chunk = head; chunk != NULL && size > 0; chunk = chunk->next_chunk) {
        copy = MIN(chunk->data_size, size);
        memcpy(ptr, chunk->data, copy);
        ptr += copy;
        size -= copy;
    }
    spice_assert(size == 0);
    return data;
}

static size_t red_get_data_chunks_ptr(RedMemSlotInfo *slots, int group_id,
                                      int memslot_id,
                                      RedDataChunk *red, QXLDataChunk *qxl)
{
    RedDataChunk *red_prev;
    uint64_t data_size = 0;
    uint32_t chunk_data_size;
    QXLPHYSICAL next_chunk;
    unsigned num_chunks = 0;

    red->data_size = qxl->data_size;
    data_size += red->data_size;
    red->data = qxl->data;
    red->prev_chunk = red->next_chunk = NULL;
    if (!memslot_validate_virt(slots, (intptr_t)red->data, memslot_id, red->data_size, group_id)) {
        red->data = NULL;
        return INVALID_SIZE;
    }

    while ((next_chunk = qxl->next_chunk) != 0) {
        /* somebody is trying to use too much memory using a lot of chunks.
         * Or made a circular list of chunks
         */
        if (++num_chunks >= MAX_CHUNKS) {
            spice_warning("data split in too many chunks, avoiding DoS");
            goto error;
        }

        memslot_id = memslot_get_id(slots, next_chunk);
        qxl = (QXLDataChunk *)memslot_get_virt(slots, next_chunk, sizeof(*qxl), group_id);
        if (qxl == NULL) {
            goto error;
        }

        /* do not waste space for empty chunks.
         * This could be just a driver issue or an attempt
         * to allocate too much memory or a circular list.
         * All above cases are handled by the check for number
         * of chunks.
         */
        chunk_data_size = qxl->data_size;
        if (chunk_data_size == 0)
            continue;

        red_prev = red;
        red = g_new0(RedDataChunk, 1);
        red->data_size = chunk_data_size;
        red->prev_chunk = red_prev;
        red->data = qxl->data;
        red_prev->next_chunk = red;

        data_size += chunk_data_size;
        /* this can happen if client is sending nested chunks */
        if (data_size > MAX_DATA_CHUNK) {
            spice_warning("too much data inside chunks, avoiding DoS");
            goto error;
        }
        if (!memslot_validate_virt(slots, (intptr_t)red->data, memslot_id, red->data_size, group_id))
            goto error;
    }

    red->next_chunk = NULL;
    return data_size;

error:
    while (red->prev_chunk) {
        red_prev = red->prev_chunk;
        g_free(red);
        red = red_prev;
    }
    red->data_size = 0;
    red->next_chunk = NULL;
    red->data = NULL;
    return INVALID_SIZE;
}

static size_t red_get_data_chunks(RedMemSlotInfo *slots, int group_id,
                                  RedDataChunk *red, QXLPHYSICAL addr)
{
    QXLDataChunk *qxl;
    int memslot_id = memslot_get_id(slots, addr);

    qxl = (QXLDataChunk *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return INVALID_SIZE;
    }
    return red_get_data_chunks_ptr(slots, group_id, memslot_id, red, qxl);
}

static void red_put_data_chunks(RedDataChunk *red)
{
    RedDataChunk *tmp;

    red = red->next_chunk;
    while (red) {
        tmp = red;
        red = red->next_chunk;
        g_free(tmp);
    }
}

static void red_get_point_ptr(SpicePoint *red, QXLPoint *qxl)
{
    red->x = qxl->x;
    red->y = qxl->y;
}

static void red_get_point16_ptr(SpicePoint16 *red, QXLPoint16 *qxl)
{
    red->x = qxl->x;
    red->y = qxl->y;
}

void red_get_rect_ptr(SpiceRect *red, const QXLRect *qxl)
{
    red->top    = qxl->top;
    red->left   = qxl->left;
    red->bottom = qxl->bottom;
    red->right  = qxl->right;
}

static SpicePath *red_get_path(RedMemSlotInfo *slots, int group_id,
                               QXLPHYSICAL addr)
{
    RedDataChunk chunks;
    QXLPathSeg *start, *end;
    SpicePathSeg *seg;
    uint8_t *data;
    bool free_data;
    QXLPath *qxl;
    SpicePath *red;
    size_t size;
    uint64_t mem_size, mem_size2, segment_size;
    int n_segments;
    int i;
    uint32_t count;

    qxl = (QXLPath *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return NULL;
    }
    size = red_get_data_chunks_ptr(slots, group_id,
                                   memslot_get_id(slots, addr),
                                   &chunks, &qxl->chunk);
    if (size == INVALID_SIZE) {
        return NULL;
    }
    data = red_linearize_chunk(&chunks, size, &free_data);
    red_put_data_chunks(&chunks);

    n_segments = 0;
    mem_size = sizeof(*red);

    start = (QXLPathSeg*)data;
    end = (QXLPathSeg*)(data + size);
    while (start+1 < end) {
        n_segments++;
        count = start->count;
        segment_size = sizeof(SpicePathSeg) + (uint64_t) count * sizeof(SpicePointFix);
        mem_size += sizeof(SpicePathSeg *) + SPICE_ALIGN(segment_size, 4);
        /* avoid going backward with 32 bit architectures */
        spice_assert((uint64_t) count * sizeof(QXLPointFix)
                     <= (char*) end - (char*) &start->points[0]);
        start = (QXLPathSeg*)(&start->points[count]);
    }

    red = g_malloc(mem_size);
    red->num_segments = n_segments;

    start = (QXLPathSeg*)data;
    end = (QXLPathSeg*)(data + size);
    seg = (SpicePathSeg*)&red->segments[n_segments];
    n_segments = 0;
    mem_size2 = sizeof(*red);
    while (start+1 < end && n_segments < red->num_segments) {
        red->segments[n_segments++] = seg;
        count = start->count;

        /* Protect against overflow in size calculations before
           writing to memory */
        /* Verify that we didn't overflow due to guest changing data */
        mem_size2 += sizeof(SpicePathSeg) + (uint64_t) count * sizeof(SpicePointFix);
        spice_assert(mem_size2 <= mem_size);

        seg->flags = start->flags;
        seg->count = count;
        for (i = 0; i < seg->count; i++) {
            seg->points[i].x = start->points[i].x;
            seg->points[i].y = start->points[i].y;
        }
        start = (QXLPathSeg*)(&start->points[i]);
        seg = (SpicePathSeg*)(&seg->points[i]);
    }
    /* Ensure guest didn't tamper with segment count */
    spice_assert(n_segments == red->num_segments);

    if (free_data) {
        g_free(data);
    }
    return red;
}

static SpiceClipRects *red_get_clip_rects(RedMemSlotInfo *slots, int group_id,
                                          QXLPHYSICAL addr)
{
    RedDataChunk chunks;
    QXLClipRects *qxl;
    SpiceClipRects *red;
    QXLRect *start;
    uint8_t *data;
    bool free_data;
    size_t size;
    int i;
    uint32_t num_rects;

    qxl = (QXLClipRects *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return NULL;
    }
    size = red_get_data_chunks_ptr(slots, group_id,
                                   memslot_get_id(slots, addr),
                                   &chunks, &qxl->chunk);
    if (size == INVALID_SIZE) {
        return NULL;
    }
    data = red_linearize_chunk(&chunks, size, &free_data);
    red_put_data_chunks(&chunks);

    num_rects = qxl->num_rects;
    /* The cast is needed to prevent 32 bit integer overflows.
     * This check is enough as size is limited to 31 bit
     * by red_get_data_chunks_ptr checks.
     */
    spice_assert((uint64_t) num_rects * sizeof(QXLRect) == size);
    SPICE_VERIFY(sizeof(SpiceRect) == sizeof(QXLRect));
    red = g_malloc(sizeof(*red) + num_rects * sizeof(SpiceRect));
    red->num_rects = num_rects;

    start = (QXLRect*)data;
    for (i = 0; i < red->num_rects; i++) {
        red_get_rect_ptr(red->rects + i, start++);
    }

    if (free_data) {
        g_free(data);
    }
    return red;
}

static SpiceChunks *red_get_image_data_flat(RedMemSlotInfo *slots, int group_id,
                                            QXLPHYSICAL addr, size_t size)
{
    SpiceChunks *data;
    void *bitmap_virt;

    bitmap_virt = memslot_get_virt(slots, addr, size, group_id);
    if (bitmap_virt == NULL) {
        return NULL;
    }

    data = spice_chunks_new(1);
    data->data_size      = size;
    data->chunk[0].data  = bitmap_virt;
    data->chunk[0].len   = size;
    return data;
}

static SpiceChunks *red_get_image_data_chunked(RedMemSlotInfo *slots, int group_id,
                                               RedDataChunk *head)
{
    SpiceChunks *data;
    RedDataChunk *chunk;
    int i;

    for (i = 0, chunk = head; chunk != NULL; chunk = chunk->next_chunk) {
        i++;
    }

    data = spice_chunks_new(i);
    data->data_size = 0;
    for (i = 0, chunk = head;
         chunk != NULL && i < data->num_chunks;
         chunk = chunk->next_chunk, i++) {
        data->chunk[i].data  = chunk->data;
        data->chunk[i].len   = chunk->data_size;
        data->data_size     += chunk->data_size;
    }
    spice_assert(i == data->num_chunks);
    return data;
}

static const char *bitmap_format_to_string(int format)
{
    switch (format) {
    case SPICE_BITMAP_FMT_INVALID: return "SPICE_BITMAP_FMT_INVALID";
    case SPICE_BITMAP_FMT_1BIT_LE: return "SPICE_BITMAP_FMT_1BIT_LE";
    case SPICE_BITMAP_FMT_1BIT_BE: return "SPICE_BITMAP_FMT_1BIT_BE";
    case SPICE_BITMAP_FMT_4BIT_LE: return "SPICE_BITMAP_FMT_4BIT_LE";
    case SPICE_BITMAP_FMT_4BIT_BE: return "SPICE_BITMAP_FMT_4BIT_BE";
    case SPICE_BITMAP_FMT_8BIT: return "SPICE_BITMAP_FMT_8BIT";
    case SPICE_BITMAP_FMT_16BIT: return "SPICE_BITMAP_FMT_16BIT";
    case SPICE_BITMAP_FMT_24BIT: return "SPICE_BITMAP_FMT_24BIT";
    case SPICE_BITMAP_FMT_32BIT: return "SPICE_BITMAP_FMT_32BIT";
    case SPICE_BITMAP_FMT_RGBA: return "SPICE_BITMAP_FMT_RGBA";
    case SPICE_BITMAP_FMT_8BIT_A: return "SPICE_BITMAP_FMT_8BIT_A";
    }
    return "unknown";
}

static const unsigned int MAP_BITMAP_FMT_TO_BITS_PER_PIXEL[] =
    {0, 1, 1, 4, 4, 8, 16, 24, 32, 32, 8};

static bool bitmap_consistent(SpiceBitmap *bitmap)
{
    unsigned int bpp;

    if (bitmap->format >= SPICE_N_ELEMENTS(MAP_BITMAP_FMT_TO_BITS_PER_PIXEL)) {
        spice_warning("wrong format specified for image");
        return false;
    }

    bpp = MAP_BITMAP_FMT_TO_BITS_PER_PIXEL[bitmap->format];

    if (bitmap->stride < (((uint64_t) bitmap->x * bpp + 7u) / 8u)) {
        spice_warning("image stride too small for width: %d < ((%d * %d + 7) / 8) (%s=%d)",
                    bitmap->stride, bitmap->x, bpp,
                    bitmap_format_to_string(bitmap->format),
                    bitmap->format);
        return false;
    }
    return true;
}

static SpiceImage *red_get_image(RedMemSlotInfo *slots, int group_id,
                                 QXLPHYSICAL addr, uint32_t flags, bool is_mask)
{
    RedDataChunk chunks;
    QXLImage *qxl;
    SpiceImage *red = NULL;
    SpicePalette *rp = NULL;
    uint64_t bitmap_size, size;
    uint8_t qxl_flags;
    QXLPHYSICAL palette;

    if (addr == 0) {
        return NULL;
    }

    qxl = (QXLImage *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return NULL;
    }
    red = g_new0(SpiceImage, 1);
    red->descriptor.id     = qxl->descriptor.id;
    red->descriptor.type   = qxl->descriptor.type;
    red->descriptor.flags = 0;
    if (qxl->descriptor.flags & QXL_IMAGE_HIGH_BITS_SET) {
        red->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
    }
    if (qxl->descriptor.flags & QXL_IMAGE_CACHE) {
        red->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
    }
    red->descriptor.width  = qxl->descriptor.width;
    red->descriptor.height = qxl->descriptor.height;

    switch (red->descriptor.type) {
    case SPICE_IMAGE_TYPE_BITMAP:
        red->u.bitmap.format = qxl->bitmap.format;
        red->u.bitmap.x      = qxl->bitmap.x;
        red->u.bitmap.y      = qxl->bitmap.y;
        red->u.bitmap.stride = qxl->bitmap.stride;
        palette = qxl->bitmap.palette;
        if (!bitmap_fmt_is_rgb(red->u.bitmap.format) && !palette && !is_mask) {
            spice_warning("guest error: missing palette on bitmap format=%d",
                          red->u.bitmap.format);
            goto error;
        }
        if (red->u.bitmap.x == 0 || red->u.bitmap.y == 0) {
            spice_warning("guest error: zero area bitmap");
            goto error;
        }
        qxl_flags = qxl->bitmap.flags;
        if (qxl_flags & QXL_BITMAP_TOP_DOWN) {
            red->u.bitmap.flags = SPICE_BITMAP_FLAGS_TOP_DOWN;
        }
        if (!bitmap_consistent(&red->u.bitmap)) {
            goto error;
        }
        if (palette) {
            QXLPalette *qp;
            int i, num_ents;
            qp = (QXLPalette *)memslot_get_virt(slots, palette,
                                                sizeof(*qp), group_id);
            if (qp == NULL) {
                goto error;
            }
            num_ents = qp->num_ents;
            if (!memslot_validate_virt(slots, (intptr_t)qp->ents,
                                       memslot_get_id(slots, palette),
                                       num_ents * sizeof(qp->ents[0]), group_id)) {
                goto error;
            }
            rp = g_malloc(num_ents * sizeof(rp->ents[0]) + sizeof(*rp));
            rp->unique   = qp->unique;
            rp->num_ents = num_ents;
            if (flags & QXL_COMMAND_FLAG_COMPAT_16BPP) {
                for (i = 0; i < num_ents; i++) {
                    rp->ents[i] = color_16_to_32(qp->ents[i]);
                }
            } else {
                for (i = 0; i < num_ents; i++) {
                    rp->ents[i] = qp->ents[i];
                }
            }
            red->u.bitmap.palette = rp;
            red->u.bitmap.palette_id = rp->unique;
        }
        bitmap_size = (uint64_t) red->u.bitmap.y * red->u.bitmap.stride;
        if (bitmap_size > MAX_DATA_CHUNK) {
            goto error;
        }
        if (qxl_flags & QXL_BITMAP_DIRECT) {
            red->u.bitmap.data = red_get_image_data_flat(slots, group_id,
                                                         qxl->bitmap.data,
                                                         bitmap_size);
        } else {
            size = red_get_data_chunks(slots, group_id,
                                       &chunks, qxl->bitmap.data);
            if (size == INVALID_SIZE || size != bitmap_size) {
                red_put_data_chunks(&chunks);
                goto error;
            }
            red->u.bitmap.data = red_get_image_data_chunked(slots, group_id,
                                                            &chunks);
            red_put_data_chunks(&chunks);
        }
        if (qxl_flags & QXL_BITMAP_UNSTABLE) {
            red->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_UNSTABLE;
        }
        break;
    case SPICE_IMAGE_TYPE_SURFACE:
        red->u.surface.surface_id = qxl->surface_image.surface_id;
        break;
    case SPICE_IMAGE_TYPE_QUIC:
        red->u.quic.data_size = qxl->quic.data_size;
        size = red_get_data_chunks_ptr(slots, group_id,
                                       memslot_get_id(slots, addr),
                                       &chunks, (QXLDataChunk *)qxl->quic.data);
        if (size == INVALID_SIZE || size != red->u.quic.data_size) {
            red_put_data_chunks(&chunks);
            goto error;
        }
        red->u.quic.data = red_get_image_data_chunked(slots, group_id,
                                                      &chunks);
        red_put_data_chunks(&chunks);
        break;
    default:
        spice_warning("unknown type %d", red->descriptor.type);
        goto error;
    }
    return red;
error:
    g_free(red);
    g_free(rp);
    return NULL;
}

static void red_put_image(SpiceImage *red)
{
    if (red == NULL)
        return;

    switch (red->descriptor.type) {
    case SPICE_IMAGE_TYPE_BITMAP:
        g_free(red->u.bitmap.palette);
        spice_chunks_destroy(red->u.bitmap.data);
        break;
    case SPICE_IMAGE_TYPE_QUIC:
        spice_chunks_destroy(red->u.quic.data);
        break;
    case SPICE_IMAGE_TYPE_DRM_PRIME:
        close(red->u.drm_prime.drm_dma_buf_fd);
        break;
    }
    g_free(red);
}

static void red_get_brush_ptr(RedMemSlotInfo *slots, int group_id,
                              SpiceBrush *red, QXLBrush *qxl, uint32_t flags)
{
    red->type = qxl->type;
    switch (red->type) {
    case SPICE_BRUSH_TYPE_SOLID:
        if (flags & QXL_COMMAND_FLAG_COMPAT_16BPP) {
            red->u.color = color_16_to_32(qxl->u.color);
        } else {
            red->u.color = qxl->u.color;
        }
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        red->u.pattern.pat = red_get_image(slots, group_id, qxl->u.pattern.pat, flags, false);
        red_get_point_ptr(&red->u.pattern.pos, &qxl->u.pattern.pos);
        break;
    }
}

static void red_put_brush(SpiceBrush *red)
{
    switch (red->type) {
    case SPICE_BRUSH_TYPE_PATTERN:
        red_put_image(red->u.pattern.pat);
        break;
    }
}

static void red_get_qmask_ptr(RedMemSlotInfo *slots, int group_id,
                              SpiceQMask *red, QXLQMask *qxl, uint32_t flags)
{
    red->flags  = qxl->flags;
    red_get_point_ptr(&red->pos, &qxl->pos);
    red->bitmap = red_get_image(slots, group_id, qxl->bitmap, flags, true);
}

static void red_put_qmask(SpiceQMask *red)
{
    red_put_image(red->bitmap);
}

static void red_get_fill_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceFill *red, QXLFill *qxl, uint32_t flags)
{
    red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush, flags);
    red->rop_descriptor = qxl->rop_descriptor;
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask, flags);
}

static void red_put_fill(SpiceFill *red)
{
    red_put_brush(&red->brush);
    red_put_qmask(&red->mask);
}

static void red_get_opaque_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceOpaque *red, QXLOpaque *qxl, uint32_t flags)
{
   red->src_bitmap     = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush, flags);
   red->rop_descriptor = qxl->rop_descriptor;
   red->scale_mode     = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask, flags);
}

static void red_put_opaque(SpiceOpaque *red)
{
    red_put_image(red->src_bitmap);
    red_put_brush(&red->brush);
    red_put_qmask(&red->mask);
}

static bool red_get_copy_ptr(RedMemSlotInfo *slots, int group_id,
                             RedDrawable *red_drawable, QXLCopy *qxl, uint32_t flags)
{
    /* there's no sense to have this true, this will just waste CPU and reduce optimizations
     * for this command. Due to some bugs however some driver set self_bitmap field for this
     * command so reset it. */
    red_drawable->self_bitmap = false;

    SpiceCopy *red = &red_drawable->u.copy;

    red->src_bitmap      = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
    if (!red->src_bitmap) {
        return false;
    }
    red_get_rect_ptr(&red->src_area, &qxl->src_area);
    /* The source area should not extend outside the source bitmap or have
     * swapped coordinates.
     */
    if (red->src_area.left < 0 ||
        red->src_area.left > red->src_area.right ||
        red->src_area.top < 0 ||
        red->src_area.top > red->src_area.bottom) {
        return false;
    }
    if (red->src_bitmap->descriptor.type == SPICE_IMAGE_TYPE_BITMAP &&
        (red->src_area.right > red->src_bitmap->u.bitmap.x ||
         red->src_area.bottom > red->src_bitmap->u.bitmap.y)) {
        return false;
    }
    red->rop_descriptor  = qxl->rop_descriptor;
    red->scale_mode      = qxl->scale_mode;
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask, flags);
    return true;
}

static void red_put_copy(SpiceCopy *red)
{
    red_put_image(red->src_bitmap);
    red_put_qmask(&red->mask);
}

// these types are really the same thing
#define red_get_blend_ptr red_get_copy_ptr
#define red_put_blend red_put_copy

static void red_get_transparent_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceTransparent *red, QXLTransparent *qxl,
                                    uint32_t flags)
{
    red->src_bitmap      = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red->src_color       = qxl->src_color;
   red->true_color      = qxl->true_color;
}

static void red_put_transparent(SpiceTransparent *red)
{
    red_put_image(red->src_bitmap);
}

static void red_get_alpha_blend_ptr(RedMemSlotInfo *slots, int group_id,
                                    SpiceAlphaBlend *red, QXLAlphaBlend *qxl,
                                    uint32_t flags)
{
    red->alpha_flags = qxl->alpha_flags;
    red->alpha       = qxl->alpha;
    red->src_bitmap  = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
    red_get_rect_ptr(&red->src_area, &qxl->src_area);
}

static void red_get_alpha_blend_ptr_compat(RedMemSlotInfo *slots, int group_id,
                                           SpiceAlphaBlend *red, QXLCompatAlphaBlend *qxl,
                                           uint32_t flags)
{
    red->alpha       = qxl->alpha;
    red->src_bitmap  = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
    red_get_rect_ptr(&red->src_area, &qxl->src_area);
}

static void red_put_alpha_blend(SpiceAlphaBlend *red)
{
    red_put_image(red->src_bitmap);
}

static bool get_transform(RedMemSlotInfo *slots,
                          int group_id,
                          QXLPHYSICAL qxl_transform,
                          SpiceTransform *dst_transform)
{
    const uint32_t *t = NULL;

    if (qxl_transform == 0)
        return false;

    t = (uint32_t *)memslot_get_virt(slots, qxl_transform, sizeof(*dst_transform), group_id);

    if (t == NULL)
        return false;

    memcpy(dst_transform, t, sizeof(*dst_transform));
    return true;
}

static void red_get_composite_ptr(RedMemSlotInfo *slots, int group_id,
                                  SpiceComposite *red, QXLComposite *qxl, uint32_t flags)
{
    red->flags = qxl->flags;

    red->src_bitmap = red_get_image(slots, group_id, qxl->src, flags, false);
    if (get_transform(slots, group_id, qxl->src_transform, &red->src_transform))
        red->flags |= SPICE_COMPOSITE_HAS_SRC_TRANSFORM;

    if (qxl->mask) {
        red->mask_bitmap = red_get_image(slots, group_id, qxl->mask, flags, false);
        red->flags |= SPICE_COMPOSITE_HAS_MASK;
        if (get_transform(slots, group_id, qxl->mask_transform, &red->mask_transform))
            red->flags |= SPICE_COMPOSITE_HAS_MASK_TRANSFORM;
    } else {
        red->mask_bitmap = NULL;
    }
    red->src_origin.x = qxl->src_origin.x;
    red->src_origin.y = qxl->src_origin.y;
    red->mask_origin.x = qxl->mask_origin.x;
    red->mask_origin.y = qxl->mask_origin.y;
}

static void red_put_composite(SpiceComposite *red)
{
    red_put_image(red->src_bitmap);
    if (red->mask_bitmap)
        red_put_image(red->mask_bitmap);
}

static void red_get_rop3_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceRop3 *red, QXLRop3 *qxl, uint32_t flags)
{
   red->src_bitmap = red_get_image(slots, group_id, qxl->src_bitmap, flags, false);
   red_get_rect_ptr(&red->src_area, &qxl->src_area);
   red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush, flags);
   red->rop3       = qxl->rop3;
   red->scale_mode = qxl->scale_mode;
   red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask, flags);
}

static void red_put_rop3(SpiceRop3 *red)
{
    red_put_image(red->src_bitmap);
    red_put_brush(&red->brush);
    red_put_qmask(&red->mask);
}

static bool red_get_stroke_ptr(RedMemSlotInfo *slots, int group_id,
                               SpiceStroke *red, QXLStroke *qxl, uint32_t flags)
{
    red->path = red_get_path(slots, group_id, qxl->path);
    if (!red->path) {
        return false;
    }
    red->attr.flags       = qxl->attr.flags;
    if (red->attr.flags & SPICE_LINE_FLAGS_STYLED) {
        int style_nseg;
        uint8_t *buf;

        style_nseg = qxl->attr.style_nseg;
        red->attr.style = g_malloc_n(style_nseg, sizeof(SPICE_FIXED28_4));
        red->attr.style_nseg  = style_nseg;
        spice_assert(qxl->attr.style);
        buf = (uint8_t *)memslot_get_virt(slots, qxl->attr.style,
                                          style_nseg * sizeof(QXLFIXED), group_id);
        if (buf == NULL) {
            return false;
        }
        memcpy(red->attr.style, buf, style_nseg * sizeof(QXLFIXED));
    } else {
        red->attr.style_nseg  = 0;
        red->attr.style       = NULL;
    }
    red_get_brush_ptr(slots, group_id, &red->brush, &qxl->brush, flags);
    red->fore_mode        = qxl->fore_mode;
    red->back_mode        = qxl->back_mode;
    return true;
}

static void red_put_stroke(SpiceStroke *red)
{
    red_put_brush(&red->brush);
    g_free(red->path);
    if (red->attr.flags & SPICE_LINE_FLAGS_STYLED) {
        g_free(red->attr.style);
    }
}

static SpiceString *red_get_string(RedMemSlotInfo *slots, int group_id,
                                   QXLPHYSICAL addr)
{
    RedDataChunk chunks;
    QXLString *qxl;
    QXLRasterGlyph *start, *end;
    SpiceString *red;
    SpiceRasterGlyph *glyph;
    uint8_t *data;
    bool free_data;
    size_t chunk_size, qxl_size, red_size, glyph_size;
    int glyphs, i;
    /* use unsigned to prevent integer overflow in multiplication below */
    unsigned int bpp = 0;
    uint16_t qxl_flags, qxl_length;

    qxl = (QXLString *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return NULL;
    }
    chunk_size = red_get_data_chunks_ptr(slots, group_id,
                                         memslot_get_id(slots, addr),
                                         &chunks, &qxl->chunk);
    if (chunk_size == INVALID_SIZE) {
        return NULL;
    }
    data = red_linearize_chunk(&chunks, chunk_size, &free_data);
    red_put_data_chunks(&chunks);

    qxl_size = qxl->data_size;
    qxl_flags = qxl->flags;
    qxl_length = qxl->length;
    spice_assert(chunk_size == qxl_size);

    if (qxl_flags & SPICE_STRING_FLAGS_RASTER_A1) {
        bpp = 1;
    } else if (qxl_flags & SPICE_STRING_FLAGS_RASTER_A4) {
        bpp = 4;
    } else if (qxl_flags & SPICE_STRING_FLAGS_RASTER_A8) {
        bpp = 8;
    }
    spice_assert(bpp != 0);

    start = (QXLRasterGlyph*)data;
    end = (QXLRasterGlyph*)(data + chunk_size);
    red_size = sizeof(SpiceString);
    glyphs = 0;
    while (start < end) {
        spice_assert((QXLRasterGlyph*)(&start->data[0]) <= end);
        glyphs++;
        glyph_size = start->height * ((start->width * bpp + 7u) / 8u);
        red_size += sizeof(SpiceRasterGlyph *) + SPICE_ALIGN(sizeof(SpiceRasterGlyph) + glyph_size, 4);
        /* do the test correctly, we know end - start->data[0] cannot
         * overflow, don't use start->data[glyph_size] to test for
         * buffer overflow as this on 32 bit can cause overflow
         * on the pointer arithmetic */
        spice_assert(glyph_size <= (char*) end - (char*) &start->data[0]);
        start = (QXLRasterGlyph*)(&start->data[glyph_size]);
    }
    spice_assert(start <= end);
    spice_assert(glyphs == qxl_length);

    red = g_malloc(red_size);
    red->length = qxl_length;
    red->flags = qxl_flags;

    start = (QXLRasterGlyph*)data;
    end = (QXLRasterGlyph*)(data + chunk_size);
    glyph = (SpiceRasterGlyph *)&red->glyphs[red->length];
    for (i = 0; i < red->length; i++) {
        spice_assert((QXLRasterGlyph*)(&start->data[0]) <= end);
        red->glyphs[i] = glyph;
        glyph->width = start->width;
        glyph->height = start->height;
        red_get_point_ptr(&glyph->render_pos, &start->render_pos);
        red_get_point_ptr(&glyph->glyph_origin, &start->glyph_origin);
        glyph_size = glyph->height * ((glyph->width * bpp + 7u) / 8u);
        /* see above for similar test */
        spice_assert(glyph_size <= (char*) end - (char*) &start->data[0]);
        memcpy(glyph->data, start->data, glyph_size);
        start = (QXLRasterGlyph*)(&start->data[glyph_size]);
        glyph = (SpiceRasterGlyph*)
            (((uint8_t *)glyph) +
             SPICE_ALIGN(sizeof(SpiceRasterGlyph) + glyph_size, 4));
    }

    if (free_data) {
        g_free(data);
    }
    return red;
}

static void red_get_text_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceText *red, QXLText *qxl, uint32_t flags)
{
   red->str = red_get_string(slots, group_id, qxl->str);
   red_get_rect_ptr(&red->back_area, &qxl->back_area);
   red_get_brush_ptr(slots, group_id, &red->fore_brush, &qxl->fore_brush, flags);
   red_get_brush_ptr(slots, group_id, &red->back_brush, &qxl->back_brush, flags);
   red->fore_mode  = qxl->fore_mode;
   red->back_mode  = qxl->back_mode;
}

static void red_put_text_ptr(SpiceText *red)
{
    g_free(red->str);
    red_put_brush(&red->fore_brush);
    red_put_brush(&red->back_brush);
}

static void red_get_whiteness_ptr(RedMemSlotInfo *slots, int group_id,
                                  SpiceWhiteness *red, QXLWhiteness *qxl, uint32_t flags)
{
    red_get_qmask_ptr(slots, group_id, &red->mask, &qxl->mask, flags);
}

static void red_put_whiteness(SpiceWhiteness *red)
{
    red_put_qmask(&red->mask);
}

#define red_get_invers_ptr red_get_whiteness_ptr
#define red_get_blackness_ptr red_get_whiteness_ptr
#define red_put_invers red_put_whiteness
#define red_put_blackness red_put_whiteness

static void red_get_clip_ptr(RedMemSlotInfo *slots, int group_id,
                             SpiceClip *red, QXLClip *qxl)
{
    red->type = qxl->type;
    switch (red->type) {
    case SPICE_CLIP_TYPE_RECTS:
        red->rects = red_get_clip_rects(slots, group_id, qxl->data);
        break;
    }
}

static void red_put_clip(SpiceClip *red)
{
    switch (red->type) {
    case SPICE_CLIP_TYPE_RECTS:
        g_free(red->rects);
        break;
    }
}

static bool red_get_native_drawable(QXLInstance *qxl_instance, RedMemSlotInfo *slots, int group_id,
                                    RedDrawable *red, QXLPHYSICAL addr, uint32_t flags)
{
    QXLDrawable *qxl;
    int i;

    qxl = (QXLDrawable *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info     = &qxl->release_info;
    red->release_info_ext.group_id = group_id;

    red_get_rect_ptr(&red->bbox, &qxl->bbox);
    red_get_clip_ptr(slots, group_id, &red->clip, &qxl->clip);
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;
    red->self_bitmap      = qxl->self_bitmap;
    red_get_rect_ptr(&red->self_bitmap_area, &qxl->self_bitmap_area);
    red->surface_id       = qxl->surface_id;

    for (i = 0; i < 3; i++) {
        red->surface_deps[i] = qxl->surfaces_dest[i];
        red_get_rect_ptr(&red->surfaces_rects[i], &qxl->surfaces_rects[i]);
    }

    red->type = qxl->type;
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_get_alpha_blend_ptr(slots, group_id,
                                &red->u.alpha_blend, &qxl->u.alpha_blend, flags);
        break;
    case QXL_DRAW_BLACKNESS:
        red_get_blackness_ptr(slots, group_id,
                              &red->u.blackness, &qxl->u.blackness, flags);
        break;
    case QXL_DRAW_BLEND:
        return red_get_blend_ptr(slots, group_id, red, &qxl->u.blend, flags);
    case QXL_DRAW_COPY:
        return red_get_copy_ptr(slots, group_id, red, &qxl->u.copy, flags);
    case QXL_COPY_BITS:
        red_get_point_ptr(&red->u.copy_bits.src_pos, &qxl->u.copy_bits.src_pos);
        break;
    case QXL_DRAW_FILL:
        red_get_fill_ptr(slots, group_id, &red->u.fill, &qxl->u.fill, flags);
        break;
    case QXL_DRAW_OPAQUE:
        red_get_opaque_ptr(slots, group_id, &red->u.opaque, &qxl->u.opaque, flags);
        break;
    case QXL_DRAW_INVERS:
        red_get_invers_ptr(slots, group_id, &red->u.invers, &qxl->u.invers, flags);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_get_rop3_ptr(slots, group_id, &red->u.rop3, &qxl->u.rop3, flags);
        break;
    case QXL_DRAW_COMPOSITE:
        red_get_composite_ptr(slots, group_id, &red->u.composite, &qxl->u.composite, flags);
        break;
    case QXL_DRAW_STROKE:
        return red_get_stroke_ptr(slots, group_id, &red->u.stroke, &qxl->u.stroke, flags);
    case QXL_DRAW_TEXT:
        red_get_text_ptr(slots, group_id, &red->u.text, &qxl->u.text, flags);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_get_transparent_ptr(slots, group_id,
                                &red->u.transparent, &qxl->u.transparent, flags);
        break;
    case QXL_DRAW_WHITENESS:
        red_get_whiteness_ptr(slots, group_id,
                              &red->u.whiteness, &qxl->u.whiteness, flags);
        break;
    default:
        spice_warning("unknown type %d", red->type);
        return false;
    };
    return true;
}

static bool red_get_compat_drawable(QXLInstance *qxl_instance, RedMemSlotInfo *slots, int group_id,
                                    RedDrawable *red, QXLPHYSICAL addr, uint32_t flags)
{
    QXLCompatDrawable *qxl;

    qxl = (QXLCompatDrawable *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info     = &qxl->release_info;
    red->release_info_ext.group_id = group_id;

    red_get_rect_ptr(&red->bbox, &qxl->bbox);
    red_get_clip_ptr(slots, group_id, &red->clip, &qxl->clip);
    red->effect           = qxl->effect;
    red->mm_time          = qxl->mm_time;

    red->self_bitmap = (qxl->bitmap_offset != 0);
    red_get_rect_ptr(&red->self_bitmap_area, &qxl->bitmap_area);

    red->surface_deps[0] = -1;
    red->surface_deps[1] = -1;
    red->surface_deps[2] = -1;

    red->type = qxl->type;
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_get_alpha_blend_ptr_compat(slots, group_id,
                                       &red->u.alpha_blend, &qxl->u.alpha_blend, flags);
        break;
    case QXL_DRAW_BLACKNESS:
        red_get_blackness_ptr(slots, group_id,
                              &red->u.blackness, &qxl->u.blackness, flags);
        break;
    case QXL_DRAW_BLEND:
        return red_get_blend_ptr(slots, group_id, red, &qxl->u.blend, flags);
    case QXL_DRAW_COPY:
        return red_get_copy_ptr(slots, group_id, red, &qxl->u.copy, flags);
    case QXL_COPY_BITS:
        red_get_point_ptr(&red->u.copy_bits.src_pos, &qxl->u.copy_bits.src_pos);
        red->surface_deps[0] = 0;
        red->surfaces_rects[0].left   = red->u.copy_bits.src_pos.x;
        red->surfaces_rects[0].right  = red->u.copy_bits.src_pos.x +
            (red->bbox.right - red->bbox.left);
        red->surfaces_rects[0].top    = red->u.copy_bits.src_pos.y;
        red->surfaces_rects[0].bottom = red->u.copy_bits.src_pos.y +
            (red->bbox.bottom - red->bbox.top);
        break;
    case QXL_DRAW_FILL:
        red_get_fill_ptr(slots, group_id, &red->u.fill, &qxl->u.fill, flags);
        break;
    case QXL_DRAW_OPAQUE:
        red_get_opaque_ptr(slots, group_id, &red->u.opaque, &qxl->u.opaque, flags);
        break;
    case QXL_DRAW_INVERS:
        red_get_invers_ptr(slots, group_id, &red->u.invers, &qxl->u.invers, flags);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_get_rop3_ptr(slots, group_id, &red->u.rop3, &qxl->u.rop3, flags);
        break;
    case QXL_DRAW_STROKE:
        return red_get_stroke_ptr(slots, group_id, &red->u.stroke, &qxl->u.stroke, flags);
    case QXL_DRAW_TEXT:
        red_get_text_ptr(slots, group_id, &red->u.text, &qxl->u.text, flags);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_get_transparent_ptr(slots, group_id,
                                &red->u.transparent, &qxl->u.transparent, flags);
        break;
    case QXL_DRAW_WHITENESS:
        red_get_whiteness_ptr(slots, group_id,
                              &red->u.whiteness, &qxl->u.whiteness, flags);
        break;
    default:
        spice_warning("unknown type %d", red->type);
        return false;
    };
    return true;
}

static bool red_get_drawable(QXLInstance *qxl, RedMemSlotInfo *slots, int group_id,
                             RedDrawable *red, QXLPHYSICAL addr, uint32_t flags)
{
    bool ret;

    if (flags & QXL_COMMAND_FLAG_COMPAT) {
        ret = red_get_compat_drawable(qxl, slots, group_id, red, addr, flags);
    } else {
        ret = red_get_native_drawable(qxl, slots, group_id, red, addr, flags);
    }
    return ret;
}

static void red_put_drawable(RedDrawable *red)
{
    red_put_clip(&red->clip);
    if (red->self_bitmap_image) {
        red_put_image(red->self_bitmap_image);
    }
    switch (red->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_put_alpha_blend(&red->u.alpha_blend);
        break;
    case QXL_DRAW_BLACKNESS:
        red_put_blackness(&red->u.blackness);
        break;
    case QXL_DRAW_BLEND:
        red_put_blend(&red->u.blend);
        break;
    case QXL_DRAW_COPY:
        red_put_copy(&red->u.copy);
        break;
    case QXL_DRAW_FILL:
        red_put_fill(&red->u.fill);
        break;
    case QXL_DRAW_OPAQUE:
        red_put_opaque(&red->u.opaque);
        break;
    case QXL_DRAW_INVERS:
        red_put_invers(&red->u.invers);
        break;
    case QXL_DRAW_ROP3:
        red_put_rop3(&red->u.rop3);
        break;
    case QXL_DRAW_COMPOSITE:
        red_put_composite(&red->u.composite);
        break;
    case QXL_DRAW_STROKE:
        red_put_stroke(&red->u.stroke);
        break;
    case QXL_DRAW_TEXT:
        red_put_text_ptr(&red->u.text);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_put_transparent(&red->u.transparent);
        break;
    case QXL_DRAW_WHITENESS:
        red_put_whiteness(&red->u.whiteness);
        break;
    }
    if (red->qxl != NULL) {
        red_qxl_release_resource(red->qxl, red->release_info_ext);
    }
}

RedDrawable *red_drawable_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                              int group_id, QXLPHYSICAL addr,
                              uint32_t flags)
{
    RedDrawable *red = g_new0(RedDrawable, 1);

    red->refs = 1;

    if (!red_get_drawable(qxl, slots, group_id, red, addr, flags)) {
       red_drawable_unref(red);
       return NULL;
    }

    return red;
}

RedDrawable *red_drawable_ref(RedDrawable *drawable)
{
    drawable->refs++;
    return drawable;
}

void red_drawable_unref(RedDrawable *red_drawable)
{
    if (--red_drawable->refs) {
        return;
    }
    red_put_drawable(red_drawable);
    g_free(red_drawable);
}

static bool red_get_update_cmd(QXLInstance *qxl_instance, RedMemSlotInfo *slots, int group_id,
                               RedUpdateCmd *red, QXLPHYSICAL addr)
{
    QXLUpdateCmd *qxl;

    qxl = (QXLUpdateCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info     = &qxl->release_info;
    red->release_info_ext.group_id = group_id;

    red_get_rect_ptr(&red->area, &qxl->area);
    red->update_id  = qxl->update_id;
    red->surface_id = qxl->surface_id;
    return true;
}

static void red_put_update_cmd(RedUpdateCmd *red)
{
    if (red->qxl != NULL) {
        red_qxl_release_resource(red->qxl, red->release_info_ext);
    }
}

RedUpdateCmd *red_update_cmd_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                                 int group_id, QXLPHYSICAL addr)
{
    RedUpdateCmd *red;

    red = g_new0(RedUpdateCmd, 1);

    red->refs = 1;

    if (!red_get_update_cmd(qxl, slots, group_id, red, addr)) {
        red_update_cmd_unref(red);
        return NULL;
    }

    return red;
}

RedUpdateCmd *red_update_cmd_ref(RedUpdateCmd *red)
{
    red->refs++;
    return red;
}

void red_update_cmd_unref(RedUpdateCmd *red)
{
    if (--red->refs) {
        return;
    }
    red_put_update_cmd(red);
    g_free(red);
}

static bool red_get_message(QXLInstance *qxl_instance, RedMemSlotInfo *slots, int group_id,
                            RedMessage *red, QXLPHYSICAL addr)
{
    QXLMessage *qxl;
    int memslot_id;
    unsigned long len;
    uint8_t *end;

    /*
     * security alert:
     *   qxl->data[0] size isn't specified anywhere -> can't verify
     *   luckily this is for debug logging only,
     *   so we can just ignore it by default.
     */
    qxl = (QXLMessage *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info      = &qxl->release_info;
    red->release_info_ext.group_id  = group_id;
    red->data                       = qxl->data;
    memslot_id = memslot_get_id(slots, addr+sizeof(*qxl));
    len = memslot_max_size_virt(slots, ((intptr_t) qxl)+sizeof(*qxl), memslot_id, group_id);
    len = MIN(len, 100000);
    end = (uint8_t *)memchr(qxl->data, 0, len);
    if (end == NULL) {
        return false;
    }
    red->len = end - qxl->data;
    return true;
}

static void red_put_message(RedMessage *red)
{
    if (red->qxl != NULL) {
        red_qxl_release_resource(red->qxl, red->release_info_ext);
    }
}

RedMessage *red_message_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                            int group_id, QXLPHYSICAL addr)
{
    RedMessage *red;

    red = g_new0(RedMessage, 1);

    red->refs = 1;

    if (!red_get_message(qxl, slots, group_id, red, addr)) {
        red_message_unref(red);
        return NULL;
    }

    return red;
}

RedMessage *red_message_ref(RedMessage *red)
{
    red->refs++;
    return red;
}

void red_message_unref(RedMessage *red)
{
    if (--red->refs) {
        return;
    }
    red_put_message(red);
    g_free(red);
}

static unsigned int surface_format_to_bpp(uint32_t format)
{
    switch (format) {
    case SPICE_SURFACE_FMT_1_A:
        return 1;
    case SPICE_SURFACE_FMT_8_A:
        return 8;
    case SPICE_SURFACE_FMT_16_555:
    case SPICE_SURFACE_FMT_16_565:
        return 16;
    case SPICE_SURFACE_FMT_32_xRGB:
    case SPICE_SURFACE_FMT_32_ARGB:
        return 32;
    }
    return 0;
}

bool red_validate_surface(uint32_t width, uint32_t height,
                          int32_t stride, uint32_t format)
{
    unsigned int bpp;
    uint64_t size;

    bpp = surface_format_to_bpp(format);

    /* check if format is valid */
    if (!bpp) {
        return false;
    }

    /* check stride is larger than required bytes */
    size = ((uint64_t) width * bpp + 7u) / 8u;
    /* the uint32_t conversion is here to avoid problems with -2^31 value */
    if (stride == G_MININT32 || size > (uint32_t) abs(stride)) {
        return false;
    }

    /* the multiplication can overflow, also abs(-2^31) may return a negative value */
    size = (uint64_t) height * abs(stride);
    if (size > MAX_DATA_CHUNK) {
        return false;
    }

    return true;
}

static bool red_get_surface_cmd(QXLInstance *qxl_instance, RedMemSlotInfo *slots, int group_id,
                                RedSurfaceCmd *red, QXLPHYSICAL addr)

{
    QXLSurfaceCmd *qxl;
    uint64_t size;

    qxl = (QXLSurfaceCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info      = &qxl->release_info;
    red->release_info_ext.group_id  = group_id;

    red->surface_id = qxl->surface_id;
    red->type       = qxl->type;
    red->flags      = qxl->flags;

    switch (red->type) {
    case QXL_SURFACE_CMD_CREATE:
        red->u.surface_create.format = qxl->u.surface_create.format;
        red->u.surface_create.width  = qxl->u.surface_create.width;
        red->u.surface_create.height = qxl->u.surface_create.height;
        red->u.surface_create.stride = qxl->u.surface_create.stride;

        if (!red_validate_surface(red->u.surface_create.width, red->u.surface_create.height,
                                  red->u.surface_create.stride, red->u.surface_create.format)) {
            return false;
        }

        size = red->u.surface_create.height * abs(red->u.surface_create.stride);
        red->u.surface_create.data =
            (uint8_t*)memslot_get_virt(slots, qxl->u.surface_create.data, size, group_id);
        if (red->u.surface_create.data == NULL) {
            return false;
        }
        break;
    }
    return true;
}

static void red_put_surface_cmd(RedSurfaceCmd *red)
{
    if (red->qxl) {
        red_qxl_release_resource(red->qxl, red->release_info_ext);
    }
}

RedSurfaceCmd *red_surface_cmd_new(QXLInstance *qxl_instance, RedMemSlotInfo *slots,
                                   int group_id, QXLPHYSICAL addr)
{
    RedSurfaceCmd *cmd;

    cmd = g_new0(RedSurfaceCmd, 1);

    cmd->refs = 1;

    if (!red_get_surface_cmd(qxl_instance, slots, group_id, cmd, addr)) {
        red_surface_cmd_unref(cmd);
        return NULL;
    }

    return cmd;
}

RedSurfaceCmd *red_surface_cmd_ref(RedSurfaceCmd *cmd)
{
    cmd->refs++;
    return cmd;
}

void red_surface_cmd_unref(RedSurfaceCmd *cmd)
{
    if (--cmd->refs) {
        return;
    }
    red_put_surface_cmd(cmd);
    g_free(cmd);
}

static bool red_get_cursor(RedMemSlotInfo *slots, int group_id,
                           SpiceCursor *red, QXLPHYSICAL addr)
{
    QXLCursor *qxl;
    RedDataChunk chunks;
    size_t size;
    uint8_t *data;
    bool free_data;

    qxl = (QXLCursor *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }

    red->header.unique     = qxl->header.unique;
    red->header.type       = qxl->header.type;
    red->header.width      = qxl->header.width;
    red->header.height     = qxl->header.height;
    red->header.hot_spot_x = qxl->header.hot_spot_x;
    red->header.hot_spot_y = qxl->header.hot_spot_y;

    red->flags = 0;
    red->data_size = qxl->data_size;
    size = red_get_data_chunks_ptr(slots, group_id,
                                   memslot_get_id(slots, addr),
                                   &chunks, &qxl->chunk);
    if (size == INVALID_SIZE) {
        return false;
    }
    red->data_size = MIN(red->data_size, size);
    data = red_linearize_chunk(&chunks, size, &free_data);
    red_put_data_chunks(&chunks);
    if (free_data) {
        red->data = data;
    } else {
        red->data = g_memdup(data, size);
    }
    return true;
}

static void red_put_cursor(SpiceCursor *red)
{
    g_free(red->data);
}

static bool red_get_cursor_cmd(QXLInstance *qxl_instance, RedMemSlotInfo *slots,
                               int group_id, RedCursorCmd *red,
                               QXLPHYSICAL addr)
{
    QXLCursorCmd *qxl;

    qxl = (QXLCursorCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id);
    if (qxl == NULL) {
        return false;
    }
    red->qxl = qxl_instance;
    red->release_info_ext.info      = &qxl->release_info;
    red->release_info_ext.group_id  = group_id;

    red->type = qxl->type;
    switch (red->type) {
    case QXL_CURSOR_SET:
        red_get_point16_ptr(&red->u.set.position, &qxl->u.set.position);
        red->u.set.visible  = qxl->u.set.visible;
        return red_get_cursor(slots, group_id,  &red->u.set.shape, qxl->u.set.shape);
    case QXL_CURSOR_MOVE:
        red_get_point16_ptr(&red->u.position, &qxl->u.position);
        break;
    case QXL_CURSOR_TRAIL:
        red->u.trail.length    = qxl->u.trail.length;
        red->u.trail.frequency = qxl->u.trail.frequency;
        break;
    }
    return true;
}

RedCursorCmd *red_cursor_cmd_new(QXLInstance *qxl, RedMemSlotInfo *slots,
                                 int group_id, QXLPHYSICAL addr)
{
    RedCursorCmd *cmd;

    cmd = g_new0(RedCursorCmd, 1);

    cmd->refs = 1;

    if (!red_get_cursor_cmd(qxl, slots, group_id, cmd, addr)) {
        red_cursor_cmd_unref(cmd);
        return NULL;
    }

    return cmd;
}

static void red_put_cursor_cmd(RedCursorCmd *red)
{
    switch (red->type) {
    case QXL_CURSOR_SET:
        red_put_cursor(&red->u.set.shape);
        break;
    }
    if (red->qxl) {
        red_qxl_release_resource(red->qxl, red->release_info_ext);
    }
}

RedCursorCmd *red_cursor_cmd_ref(RedCursorCmd *red)
{
    red->refs++;
    return red;
}

void red_cursor_cmd_unref(RedCursorCmd *red)
{
    if (--red->refs) {
        return;
    }
    red_put_cursor_cmd(red);
    g_free(red);
}
