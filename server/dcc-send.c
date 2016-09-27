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

#include <common/marshaller.h>
#include <common/generated_server_marshallers.h>

#include "dcc-private.h"
#include "egl.h"
#include "display-channel-private.h"
#include "red-qxl.h"

typedef enum {
    FILL_BITS_TYPE_INVALID,
    FILL_BITS_TYPE_CACHE,
    FILL_BITS_TYPE_SURFACE,
    FILL_BITS_TYPE_COMPRESS_LOSSLESS,
    FILL_BITS_TYPE_COMPRESS_LOSSY,
    FILL_BITS_TYPE_BITMAP,
} FillBitsType;

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

static int dcc_pixmap_cache_unlocked_hit(DisplayChannelClient *dcc, uint64_t id, int *lossy)
{
    PixmapCache *cache = dcc->priv->pixmap_cache;
    NewCacheItem *item;
    uint64_t serial;

    serial = red_channel_client_get_message_serial(RED_CHANNEL_CLIENT(dcc));
    item = cache->hash_table[BITS_CACHE_HASH_KEY(id)];

    while (item) {
        if (item->id == id) {
            ring_remove(&item->lru_link);
            ring_add(&cache->lru, &item->lru_link);
            spice_assert(dcc->priv->id < MAX_CACHE_CLIENTS);
            item->sync[dcc->priv->id] = serial;
            cache->sync[dcc->priv->id] = serial;
            *lossy = item->lossy;
            break;
        }
        item = item->next;
    }

    return !!item;
}

static int dcc_pixmap_cache_hit(DisplayChannelClient *dcc, uint64_t id, int *lossy)
{
    int hit;
    PixmapCache *cache = dcc->priv->pixmap_cache;

    pthread_mutex_lock(&cache->lock);
    hit = dcc_pixmap_cache_unlocked_hit(dcc, id, lossy);
    pthread_mutex_unlock(&cache->lock);
    return hit;
}

/* set area=NULL for testing the whole surface */
static bool is_surface_area_lossy(DisplayChannelClient *dcc, uint32_t surface_id,
                                  const SpiceRect *area, SpiceRect *out_lossy_area)
{
    RedSurface *surface;
    QRegion *surface_lossy_region;
    QRegion lossy_region;
    DisplayChannel *display = DCC_TO_DC(dcc);

    spice_return_val_if_fail(display_channel_validate_surface(display, surface_id), FALSE);

    surface = &display->priv->surfaces[surface_id];
    surface_lossy_region = &dcc->priv->surface_client_lossy_region[surface_id];

    if (!area) {
        if (region_is_empty(surface_lossy_region)) {
            return FALSE;
        }
        out_lossy_area->top = 0;
        out_lossy_area->left = 0;
        out_lossy_area->bottom = surface->context.height;
        out_lossy_area->right = surface->context.width;
        return TRUE;
    }

    region_init(&lossy_region);
    region_add(&lossy_region, area);
    region_and(&lossy_region, surface_lossy_region);
    if (region_is_empty(&lossy_region)) {
        return FALSE;
    }
    out_lossy_area->left = lossy_region.extents.x1;
    out_lossy_area->top = lossy_region.extents.y1;
    out_lossy_area->right = lossy_region.extents.x2;
    out_lossy_area->bottom = lossy_region.extents.y2;
    region_destroy(&lossy_region);
    return TRUE;
}

/* returns if the bitmap was already sent lossy to the client. If the bitmap hasn't been sent yet
   to the client, returns false. "area" is for surfaces. If area = NULL,
   all the surface is considered. out_lossy_data will hold info about the bitmap, and its lossy
   area in case it is lossy and part of a surface. */
static bool is_bitmap_lossy(RedChannelClient *rcc, SpiceImage *image, SpiceRect *area,
                            BitmapData *out_data)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);

    if (image == NULL) {
        // self bitmap
        out_data->type = BITMAP_DATA_TYPE_BITMAP;
        return FALSE;
    }

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int is_hit_lossy;

        out_data->id = image->descriptor.id;
        if (dcc_pixmap_cache_hit(dcc, image->descriptor.id, &is_hit_lossy)) {
            out_data->type = BITMAP_DATA_TYPE_CACHE;
            return is_hit_lossy;
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

    return is_surface_area_lossy(dcc, out_data->id,
                                 area, &out_data->lossy_rect);
}

static bool is_brush_lossy(RedChannelClient *rcc, SpiceBrush *brush,
                           BitmapData *out_data)
{
    if (brush->type == SPICE_BRUSH_TYPE_PATTERN) {
        return is_bitmap_lossy(rcc, brush->u.pattern.pat, NULL,
                               out_data);
    } else {
        out_data->type = BITMAP_DATA_TYPE_INVALID;
        return FALSE;
    }
}

static GList *dcc_get_tail(DisplayChannelClient *dcc)
{
    return red_channel_client_get_pipe(RED_CHANNEL_CLIENT(dcc))->tail;
}

static void red_display_add_image_to_pixmap_cache(RedChannelClient *rcc,
                                                  SpiceImage *image, SpiceImage *io_image,
                                                  int is_lossy)
{
    DisplayChannel *display_channel G_GNUC_UNUSED =
        DISPLAY_CHANNEL(red_channel_client_get_channel(rcc));
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);

    if ((image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        spice_assert(image->descriptor.width * image->descriptor.height > 0);
        if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_REPLACE_ME)) {
            if (dcc_pixmap_cache_unlocked_add(dcc, image->descriptor.id,
                                              image->descriptor.width * image->descriptor.height,
                                              is_lossy)) {
                io_image->descriptor.flags |= SPICE_IMAGE_FLAGS_CACHE_ME;
                dcc->priv->send_data.pixmap_cache_items[dcc->priv->send_data.num_pixmap_cache_items++] =
                                                                               image->descriptor.id;
                stat_inc_counter(display_channel->priv->add_to_cache_counter, 1);
            }
        }
    }

    if (!(io_image->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        stat_inc_counter(display_channel->priv->non_cache_counter, 1);
    }
}

static void marshal_sub_msg_inval_list(SpiceMarshaller *m,
                                       FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_DISPLAY_INVAL_LIST);
    spice_marshaller_add_uint32(m, sizeof(*free_list->res) +
                                free_list->res->count * sizeof(free_list->res->resources[0]));
    spice_marshall_msg_display_inval_list(m, free_list->res);
}

static void marshal_sub_msg_inval_list_wait(SpiceMarshaller *m,
                                            FreeList *free_list)
{
    /* type + size + submessage */
    spice_marshaller_add_uint16(m, SPICE_MSG_WAIT_FOR_CHANNELS);
    spice_marshaller_add_uint32(m, sizeof(free_list->wait.header) +
                                free_list->wait.header.wait_count * sizeof(free_list->wait.buf[0]));
    spice_marshall_msg_wait_for_channels(m, &free_list->wait.header);
}

/* use legacy SpiceDataHeader (with sub_list) */
static void send_free_list_legacy(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    FreeList *free_list = &dcc->priv->send_data.free_list;
    SpiceMarshaller *marshaller;
    int sub_list_len = 1;
    SpiceMarshaller *wait_m = NULL;
    SpiceMarshaller *inval_m;
    SpiceMarshaller *sub_list_m;

    marshaller = red_channel_client_get_marshaller(rcc);
    inval_m = spice_marshaller_get_submarshaller(marshaller);

    marshal_sub_msg_inval_list(inval_m, free_list);

    if (free_list->wait.header.wait_count) {
        wait_m = spice_marshaller_get_submarshaller(marshaller);
        marshal_sub_msg_inval_list_wait(wait_m, free_list);
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
static void send_free_list(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    FreeList *free_list = &dcc->priv->send_data.free_list;
    const int sub_list_len = 2;
    SpiceMarshaller *urgent_marshaller;
    SpiceMarshaller *wait_m;
    SpiceMarshaller *inval_m;
    uint32_t sub_arr_offset;
    uint32_t wait_offset = 0;
    uint32_t inval_offset = 0;
    int i;

    urgent_marshaller = red_channel_client_switch_to_urgent_sender(rcc);
    for (i = 0; i < dcc->priv->send_data.num_pixmap_cache_items; i++) {
        int dummy;
        /* When using the urgent marshaller, the serial number of the message that is
         * going to be sent right after the SPICE_MSG_LIST, is increased by one.
         * But all this message pixmaps cache references used its old serial.
         * we use pixmap_cache_items to collect these pixmaps, and we update their serial
         * by calling pixmap_cache_hit. */
        dcc_pixmap_cache_hit(dcc, dcc->priv->send_data.pixmap_cache_items[i], &dummy);
    }

    if (!free_list->wait.header.wait_count) {
        /* only one message, no need for a list */
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_LIST);
        spice_marshall_msg_display_inval_list(urgent_marshaller, free_list->res);
        return;
    }

    red_channel_client_init_send_data(rcc, SPICE_MSG_LIST);

    /* append invalidate list */
    inval_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    marshal_sub_msg_inval_list(inval_m, free_list);

    /* append wait list */
    wait_m = spice_marshaller_get_submarshaller(urgent_marshaller);
    marshal_sub_msg_inval_list_wait(wait_m, free_list);

    sub_arr_offset = sub_list_len * sizeof(uint32_t);

    spice_marshaller_add_uint16(urgent_marshaller, sub_list_len);
    inval_offset = spice_marshaller_get_offset(inval_m); // calc the offset before
                                                         // adding the sub list
                                                         // offsets array to the marshaller
    /* adding the array of offsets */
    wait_offset = spice_marshaller_get_offset(wait_m);
    spice_marshaller_add_uint32(urgent_marshaller, wait_offset + sub_arr_offset);
    spice_marshaller_add_uint32(urgent_marshaller, inval_offset + sub_arr_offset);
}

static void fill_base(SpiceMarshaller *base_marshaller, Drawable *drawable)
{
    SpiceMsgDisplayBase base;

    base.surface_id = drawable->surface_id;
    base.box = drawable->red_drawable->bbox;
    base.clip = drawable->red_drawable->clip;

    spice_marshall_DisplayBase(base_marshaller, &base);
}

static void marshaller_compress_buf_free(uint8_t *data, void *opaque)
{
    compress_buf_free(opaque);
}

static void marshaller_add_compressed(SpiceMarshaller *m,
                                      RedCompressBuf *comp_buf, size_t size)
{
    size_t max = size;
    size_t now;
    do {
        spice_return_if_fail(comp_buf);
        now = MIN(sizeof(comp_buf->buf), max);
        max -= now;
        spice_marshaller_add_by_ref_full(m, comp_buf->buf.bytes, now,
                                         marshaller_compress_buf_free, comp_buf);
        comp_buf = comp_buf->send_next;
    } while (max);
}

static void marshaller_unref_drawable(uint8_t *data, void *opaque)
{
    Drawable *drawable = opaque;
    drawable_unref(drawable);
}

/* if the number of times fill_bits can be called per one qxl_drawable increases -
   MAX_LZ_DRAWABLE_INSTANCES must be increased as well */
/* NOTE: 'simage' should be owned by the drawable. The drawable will be kept
 * alive until the marshalled message has been sent. See comments below for
 * more information */
static FillBitsType fill_bits(DisplayChannelClient *dcc, SpiceMarshaller *m,
                              SpiceImage *simage, Drawable *drawable, int can_lossy)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(dcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    SpiceImage image;
    compress_send_data_t comp_send_data = {0};
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    if (simage == NULL) {
        spice_assert(drawable->red_drawable->self_bitmap_image);
        simage = drawable->red_drawable->self_bitmap_image;
    }

    image.descriptor = simage->descriptor;
    image.descriptor.flags = 0;
    if (simage->descriptor.flags & SPICE_IMAGE_FLAGS_HIGH_BITS_SET) {
        image.descriptor.flags = SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
    }
    pthread_mutex_lock(&dcc->priv->pixmap_cache->lock);

    if ((simage->descriptor.flags & SPICE_IMAGE_FLAGS_CACHE_ME)) {
        int lossy_cache_item;
        if (dcc_pixmap_cache_unlocked_hit(dcc, image.descriptor.id, &lossy_cache_item)) {
            dcc->priv->send_data.pixmap_cache_items[dcc->priv->send_data.num_pixmap_cache_items++] =
                image.descriptor.id;
            if (can_lossy || !lossy_cache_item) {
                if (!display->priv->enable_jpeg || lossy_cache_item) {
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
                stat_inc_counter(display->priv->cache_hits_counter, 1);
                pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
                return FILL_BITS_TYPE_CACHE;
            } else {
                pixmap_cache_unlocked_set_lossy(dcc->priv->pixmap_cache, simage->descriptor.id,
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
        if (!display_channel_validate_surface(display, surface_id)) {
            spice_warning("Invalid surface in SPICE_IMAGE_TYPE_SURFACE");
            pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
            return FILL_BITS_TYPE_SURFACE;
        }

        surface = &display->priv->surfaces[surface_id];
        image.descriptor.type = SPICE_IMAGE_TYPE_SURFACE;
        image.descriptor.flags = 0;
        image.descriptor.width = surface->context.width;
        image.descriptor.height = surface->context.height;

        image.u.surface.surface_id = surface_id;
        spice_marshall_Image(m, &image,
                             &bitmap_palette_out, &lzplt_palette_out);
        spice_assert(bitmap_palette_out == NULL);
        spice_assert(lzplt_palette_out == NULL);
        pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
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
        if (red_stream_get_family(red_channel_client_get_stream(rcc)) == AF_UNIX ||
            !dcc_compress_image(dcc, &image, &simage->u.bitmap,
                                drawable, can_lossy, &comp_send_data)) {
            SpicePalette *palette;

            red_display_add_image_to_pixmap_cache(rcc, simage, &image, FALSE);

            *bitmap = simage->u.bitmap;
            bitmap->flags = bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;

            palette = bitmap->palette;
            dcc_palette_cache_palette(dcc, palette, &bitmap->flags);
            spice_marshall_Image(m, &image,
                                 &bitmap_palette_out, &lzplt_palette_out);
            spice_assert(lzplt_palette_out == NULL);

            if (bitmap_palette_out && palette) {
                spice_marshall_Palette(bitmap_palette_out, palette);
            }

            /* 'drawable' owns this bitmap data, so it must be kept
             * alive until the message is sent. */
            for (unsigned int i = 0; i < bitmap->data->num_chunks; i++) {
                drawable->refs++;
                spice_marshaller_add_by_ref_full(m, bitmap->data->chunk[i].data,
                                                 bitmap->data->chunk[i].len,
                                                 marshaller_unref_drawable, drawable);
            }
            pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
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
            pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
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
        /* 'drawable' owns this image data, so it must be kept
         * alive until the message is sent. */
        for (unsigned int i = 0; i < image.u.quic.data->num_chunks; i++) {
            drawable->refs++;
            spice_marshaller_add_by_ref_full(m, image.u.quic.data->chunk[i].data,
                                             image.u.quic.data->chunk[i].len,
                                             marshaller_unref_drawable, drawable);
        }
        pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
        return FILL_BITS_TYPE_COMPRESS_LOSSLESS;
    default:
        spice_error("invalid image type %u", image.descriptor.type);
    }
    pthread_mutex_unlock(&dcc->priv->pixmap_cache->lock);
    return FILL_BITS_TYPE_INVALID;
}

static void fill_mask(RedChannelClient *rcc, SpiceMarshaller *m,
                      SpiceImage *mask_bitmap, Drawable *drawable)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);

    if (mask_bitmap && m) {
        if (dcc->priv->image_compression != SPICE_IMAGE_COMPRESSION_OFF) {
            /* todo: pass compression argument */
            SpiceImageCompression save_img_comp = dcc->priv->image_compression;
            dcc->priv->image_compression = SPICE_IMAGE_COMPRESSION_OFF;
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
            dcc->priv->image_compression = save_img_comp;
        } else {
            fill_bits(dcc, m, mask_bitmap, drawable, FALSE);
        }
    }
}

static void fill_attr(SpiceMarshaller *m, SpiceLineAttr *attr)
{
    int i;

    if (m && attr->style_nseg) {
        for (i = 0 ; i < attr->style_nseg; i++) {
            spice_marshaller_add_uint32(m, attr->style[i]);
        }
    }
}

static void marshall_qxl_draw_fill(RedChannelClient *rcc,
                                   SpiceMarshaller *base_marshaller,
                                   RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceFill fill;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_FILL);
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

static void surface_lossy_region_update(DisplayChannelClient *dcc,
                                        Drawable *item, int has_mask, int lossy)
{
    QRegion *surface_lossy_region;
    RedDrawable *drawable;

    if (has_mask && !lossy) {
        return;
    }

    surface_lossy_region = &dcc->priv->surface_client_lossy_region[item->surface_id];
    drawable = item->red_drawable;

    if (drawable->clip.type == SPICE_CLIP_TYPE_RECTS ) {
        QRegion clip_rgn;
        QRegion draw_region;
        region_init(&clip_rgn);
        region_init(&draw_region);
        region_add(&draw_region, &drawable->bbox);
        region_add_clip_rects(&clip_rgn, drawable->clip.rects);
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

static bool drawable_intersects_with_areas(Drawable *drawable, int surface_ids[],
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

static bool pipe_rendered_drawables_intersect_with_areas(DisplayChannelClient *dcc,
                                                         int surface_ids[],
                                                         SpiceRect *surface_areas[],
                                                         int num_surfaces)
{
    GList *l;

    spice_assert(num_surfaces);

    for (l = red_channel_client_get_pipe(RED_CHANNEL_CLIENT(dcc))->head; l != NULL; l = l->next) {
        Drawable *drawable;
        RedPipeItem *pipe_item = l->data;

        if (pipe_item->type != RED_PIPE_ITEM_TYPE_DRAW)
            continue;
        drawable = SPICE_UPCAST(RedDrawablePipeItem, pipe_item)->drawable;

        if (ring_item_is_linked(&drawable->list_link))
            continue; // item hasn't been rendered

        if (drawable_intersects_with_areas(drawable, surface_ids, surface_areas, num_surfaces)) {
            return TRUE;
        }
    }

    return FALSE;
}

static bool drawable_depends_on_areas(Drawable *drawable, int surface_ids[],
                                      SpiceRect surface_areas[], int num_surfaces)
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
            dep_surface_id = drawable->surface_deps[x];
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

static void red_pipe_replace_rendered_drawables_with_images(DisplayChannelClient *dcc,
                                                            int first_surface_id,
                                                            SpiceRect *first_area)
{
    int resent_surface_ids[MAX_PIPE_SIZE];
    SpiceRect resent_areas[MAX_PIPE_SIZE]; // not pointers since drawables may be released
    int num_resent;
    GList *l;
    GQueue *pipe;

    resent_surface_ids[0] = first_surface_id;
    resent_areas[0] = *first_area;
    num_resent = 1;

    pipe = red_channel_client_get_pipe(RED_CHANNEL_CLIENT(dcc));

    // going from the oldest to the newest
    for (l = pipe->tail; l != NULL; l = l->prev) {
        RedPipeItem *pipe_item = l->data;
        Drawable *drawable;
        RedDrawablePipeItem *dpi;
        RedImageItem *image;

        if (pipe_item->type != RED_PIPE_ITEM_TYPE_DRAW)
            continue;
        dpi = SPICE_UPCAST(RedDrawablePipeItem, pipe_item);
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

        image = dcc_add_surface_area_image(dcc, drawable->red_drawable->surface_id,
                                           &drawable->red_drawable->bbox, l, TRUE);
        resent_surface_ids[num_resent] = drawable->red_drawable->surface_id;
        resent_areas[num_resent] = drawable->red_drawable->bbox;
        num_resent++;

        spice_assert(image);
        red_channel_client_pipe_remove_and_release_pos(RED_CHANNEL_CLIENT(dcc), l);
        pipe_item = &image->base;
    }
}

static void red_add_lossless_drawable_dependencies(RedChannelClient *rcc,
                                                   Drawable *item,
                                                   int deps_surfaces_ids[],
                                                   SpiceRect *deps_areas[],
                                                   int num_deps)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    RedDrawable *drawable = item->red_drawable;
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
            if (pipe_rendered_drawables_intersect_with_areas(dcc,
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
            display_channel_draw_until(display, deps_areas[i], deps_surfaces_ids[i], item);
        }
    }

    if (!sync_rendered) {
        // pushing the pipe item back to the pipe
        dcc_append_drawable(dcc, item);
        // the surfaces areas will be sent as DRAW_COPY commands, that
        // will be executed before the current drawable
        for (i = 0; i < num_deps; i++) {
            dcc_add_surface_area_image(dcc, deps_surfaces_ids[i], deps_areas[i],
                                       dcc_get_tail(dcc), FALSE);

        }
    } else {
        int drawable_surface_id[1];
        SpiceRect *drawable_bbox[1];

        drawable_surface_id[0] = drawable->surface_id;
        drawable_bbox[0] = &drawable->bbox;

        // check if the other rendered images in the pipe have updated the drawable bbox
        if (pipe_rendered_drawables_intersect_with_areas(dcc,
                                                         drawable_surface_id,
                                                         drawable_bbox,
                                                         1)) {
            red_pipe_replace_rendered_drawables_with_images(dcc,
                                                            drawable->surface_id,
                                                            &drawable->bbox);
        }

        dcc_add_surface_area_image(dcc, drawable->surface_id, &drawable->bbox,
                                   dcc_get_tail(dcc), TRUE);
    }
}

static void red_lossy_marshall_qxl_draw_fill(RedChannelClient *rcc,
                                             SpiceMarshaller *m,
                                             RedDrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
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

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.fill.brush,
                                    &brush_bitmap_data);
    if (!dest_allowed_lossy) {
        dest_is_lossy = is_surface_area_lossy(dcc, item->surface_id, &drawable->bbox,
                                              &dest_lossy_area);
    }

    if (!dest_is_lossy &&
        !(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        int has_mask = !!drawable->u.fill.mask.bitmap;

        marshall_qxl_draw_fill(rcc, m, dpi);
        // either the brush operation is opaque, or the dest is not lossy
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_opaque(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller,
                                                 RedDrawablePipeItem *dpi,
                                                 int src_allowed_lossy)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceOpaque opaque;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_OPAQUE);
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

static void red_lossy_marshall_qxl_draw_opaque(RedChannelClient *rcc,
                                               SpiceMarshaller *m,
                                               RedDrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
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

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.opaque.brush,
                                    &brush_bitmap_data);

    if (!src_allowed_lossy) {
        src_is_lossy = is_bitmap_lossy(rcc, drawable->u.opaque.src_bitmap,
                                       &drawable->u.opaque.src_area,
                                       &src_bitmap_data);
    }

    if (!(brush_is_lossy && (brush_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE)) &&
        !(src_is_lossy && (src_bitmap_data.type == BITMAP_DATA_TYPE_SURFACE))) {
        FillBitsType src_send_type;
        int has_mask = !!drawable->u.opaque.mask.bitmap;

        src_send_type = red_marshall_qxl_draw_opaque(rcc, m, dpi, src_allowed_lossy);
        if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
            src_is_lossy = TRUE;
        } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
            src_is_lossy = FALSE;
        }

        surface_lossy_region_update(dcc, item, has_mask, src_is_lossy);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static FillBitsType red_marshall_qxl_draw_copy(RedChannelClient *rcc,
                                               SpiceMarshaller *base_marshaller,
                                               RedDrawablePipeItem *dpi,
                                               int src_allowed_lossy)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceCopy copy;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY);
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

static void red_lossy_marshall_qxl_draw_copy(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.copy.mask.bitmap;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.copy.src_bitmap,
                                   &drawable->u.copy.src_area, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_copy(rcc, base_marshaller, dpi, TRUE);
    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }
    surface_lossy_region_update(dcc, item, has_mask,
                                src_is_lossy);
}

static void red_marshall_qxl_draw_transparent(RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              RedDrawablePipeItem *dpi)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceTransparent transparent;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TRANSPARENT);
    fill_base(base_marshaller, item);
    transparent = drawable->u.transparent;
    spice_marshall_Transparent(base_marshaller,
                               &transparent,
                               &src_bitmap_out);
    fill_bits(dcc, src_bitmap_out, transparent.src_bitmap, item, FALSE);
}

static void red_lossy_marshall_qxl_draw_transparent(RedChannelClient *rcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.transparent.src_bitmap,
                                   &drawable->u.transparent.src_area, &src_bitmap_data);

    if (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) {
        red_marshall_qxl_draw_transparent(rcc, base_marshaller, dpi);
        // don't update surface lossy region since transperent areas might be lossy
    } else {
        int resend_surface_ids[1];
        SpiceRect *resend_areas[1];

        resend_surface_ids[0] = src_bitmap_data.id;
        resend_areas[0] = &src_bitmap_data.lossy_rect;

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, 1);
    }
}

static FillBitsType red_marshall_qxl_draw_alpha_blend(RedChannelClient *rcc,
                                                      SpiceMarshaller *base_marshaller,
                                                      RedDrawablePipeItem *dpi,
                                                      int src_allowed_lossy)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceAlphaBlend alpha_blend;
    FillBitsType src_send_type;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND);
    fill_base(base_marshaller, item);
    alpha_blend = drawable->u.alpha_blend;
    spice_marshall_AlphaBlend(base_marshaller,
                              &alpha_blend,
                              &src_bitmap_out);
    src_send_type = fill_bits(dcc, src_bitmap_out, alpha_blend.src_bitmap, item,
                              src_allowed_lossy);

    return src_send_type;
}

static void red_lossy_marshall_qxl_draw_alpha_blend(RedChannelClient *rcc,
                                                    SpiceMarshaller *base_marshaller,
                                                    RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    FillBitsType src_send_type;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.alpha_blend.src_bitmap,
                                   &drawable->u.alpha_blend.src_area, &src_bitmap_data);

    src_send_type = red_marshall_qxl_draw_alpha_blend(rcc, base_marshaller, dpi, TRUE);

    if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSY) {
        src_is_lossy = TRUE;
    } else if (src_send_type == FILL_BITS_TYPE_COMPRESS_LOSSLESS) {
        src_is_lossy = FALSE;
    }

    if (src_is_lossy) {
        surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
    } // else, the area stays lossy/lossless as the destination
}

static void red_marshall_qxl_copy_bits(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpicePoint copy_bits;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_COPY_BITS);
    fill_base(base_marshaller, item);
    copy_bits = drawable->u.copy_bits.src_pos;
    spice_marshall_Point(base_marshaller,
                         &copy_bits);
}

static void red_lossy_marshall_qxl_copy_bits(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRect src_rect;
    int horz_offset;
    int vert_offset;
    int src_is_lossy;
    SpiceRect src_lossy_area;

    red_marshall_qxl_copy_bits(rcc, base_marshaller, dpi);

    horz_offset = drawable->u.copy_bits.src_pos.x - drawable->bbox.left;
    vert_offset = drawable->u.copy_bits.src_pos.y - drawable->bbox.top;

    src_rect.left = drawable->u.copy_bits.src_pos.x;
    src_rect.top = drawable->u.copy_bits.src_pos.y;
    src_rect.right = drawable->bbox.right + horz_offset;
    src_rect.bottom = drawable->bbox.bottom + vert_offset;

    src_is_lossy = is_surface_area_lossy(dcc, item->surface_id,
                                         &src_rect, &src_lossy_area);

    surface_lossy_region_update(dcc, item, FALSE, src_is_lossy);
}

static void red_marshall_qxl_draw_blend(RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlend blend;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLEND);
    fill_base(base_marshaller, item);
    blend = drawable->u.blend;
    spice_marshall_Blend(base_marshaller,
                         &blend,
                         &src_bitmap_out,
                         &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, blend.src_bitmap, item, FALSE);

    fill_mask(rcc, mask_bitmap_out, blend.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blend(RedChannelClient *rcc,
                                              SpiceMarshaller *base_marshaller,
                                              RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.blend.src_bitmap,
                                   &drawable->u.blend.src_area, &src_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if (!dest_is_lossy &&
        (!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))) {
        red_marshall_qxl_draw_blend(rcc, base_marshaller, dpi);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_blackness(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceBlackness blackness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_BLACKNESS);
    fill_base(base_marshaller, item);
    blackness = drawable->u.blackness;

    spice_marshall_Blackness(base_marshaller,
                             &blackness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, blackness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_blackness(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.blackness.mask.bitmap;

    red_marshall_qxl_draw_blackness(rcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_whiteness(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceWhiteness whiteness;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_WHITENESS);
    fill_base(base_marshaller, item);
    whiteness = drawable->u.whiteness;

    spice_marshall_Whiteness(base_marshaller,
                             &whiteness,
                             &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, whiteness.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_whiteness(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int has_mask = !!drawable->u.whiteness.mask.bitmap;

    red_marshall_qxl_draw_whiteness(rcc, base_marshaller, dpi);

    surface_lossy_region_update(dcc, item, has_mask, FALSE);
}

static void red_marshall_qxl_draw_inverse(RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          Drawable *item)
{
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *mask_bitmap_out;
    SpiceInvers inverse;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_INVERS);
    fill_base(base_marshaller, item);
    inverse = drawable->u.invers;

    spice_marshall_Invers(base_marshaller,
                          &inverse,
                          &mask_bitmap_out);

    fill_mask(rcc, mask_bitmap_out, inverse.mask.bitmap, item);
}

static void red_lossy_marshall_qxl_draw_inverse(RedChannelClient *rcc,
                                                SpiceMarshaller *base_marshaller,
                                                Drawable *item)
{
    red_marshall_qxl_draw_inverse(rcc, base_marshaller, item);
}

static void red_marshall_qxl_draw_rop3(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceRop3 rop3;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *mask_bitmap_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_ROP3);
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

static void red_lossy_marshall_qxl_draw_rop3(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.rop3.src_bitmap,
                                   &drawable->u.rop3.src_area, &src_bitmap_data);
    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.rop3.brush,
                                    &brush_bitmap_data);
    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        (!brush_is_lossy || (brush_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        int has_mask = !!drawable->u.rop3.mask.bitmap;
        red_marshall_qxl_draw_rop3(rcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, has_mask, FALSE);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_composite(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceMarshaller *src_bitmap_out;
    SpiceMarshaller *mask_bitmap_out;
    SpiceComposite composite;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COMPOSITE);
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

static void red_lossy_marshall_qxl_draw_composite(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller,
                                                  RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int src_is_lossy;
    BitmapData src_bitmap_data;
    int mask_is_lossy;
    BitmapData mask_bitmap_data;
    int dest_is_lossy;
    SpiceRect dest_lossy_area;

    src_is_lossy = is_bitmap_lossy(rcc, drawable->u.composite.src_bitmap,
                                   NULL, &src_bitmap_data);
    mask_is_lossy = drawable->u.composite.mask_bitmap &&
        is_bitmap_lossy(rcc, drawable->u.composite.mask_bitmap, NULL, &mask_bitmap_data);

    dest_is_lossy = is_surface_area_lossy(dcc, drawable->surface_id,
                                          &drawable->bbox, &dest_lossy_area);

    if ((!src_is_lossy || (src_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE))   &&
        (!mask_is_lossy || (mask_bitmap_data.type != BITMAP_DATA_TYPE_SURFACE)) &&
        !dest_is_lossy) {
        red_marshall_qxl_draw_composite(rcc, base_marshaller, dpi);
        surface_lossy_region_update(dcc, item, FALSE, FALSE);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_stroke(RedChannelClient *rcc,
                                         SpiceMarshaller *base_marshaller,
                                         RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceStroke stroke;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *style_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_STROKE);
    fill_base(base_marshaller, item);
    stroke = drawable->u.stroke;
    spice_marshall_Stroke(base_marshaller,
                          &stroke,
                          &style_out,
                          &brush_pat_out);

    fill_attr(style_out, &stroke.attr);
    if (brush_pat_out) {
        fill_bits(dcc, brush_pat_out, stroke.brush.u.pattern.pat, item, FALSE);
    }
}

static void red_lossy_marshall_qxl_draw_stroke(RedChannelClient *rcc,
                                               SpiceMarshaller *base_marshaller,
                                               RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int brush_is_lossy;
    BitmapData brush_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop;

    brush_is_lossy = is_brush_lossy(rcc, &drawable->u.stroke.brush,
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
        red_marshall_qxl_draw_stroke(rcc, base_marshaller, dpi);
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

        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_marshall_qxl_draw_text(RedChannelClient *rcc,
                                       SpiceMarshaller *base_marshaller,
                                       RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    SpiceText text;
    SpiceMarshaller *brush_pat_out;
    SpiceMarshaller *back_brush_pat_out;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_TEXT);
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

static void red_lossy_marshall_qxl_draw_text(RedChannelClient *rcc,
                                             SpiceMarshaller *base_marshaller,
                                             RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedDrawable *drawable = item->red_drawable;
    int fg_is_lossy;
    BitmapData fg_bitmap_data;
    int bg_is_lossy;
    BitmapData bg_bitmap_data;
    int dest_is_lossy = FALSE;
    SpiceRect dest_lossy_area;
    int rop = 0;

    fg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.fore_brush,
                                 &fg_bitmap_data);
    bg_is_lossy = is_brush_lossy(rcc, &drawable->u.text.back_brush,
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
        red_marshall_qxl_draw_text(rcc, base_marshaller, dpi);
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
        red_add_lossless_drawable_dependencies(rcc, item,
                                               resend_surface_ids, resend_areas, num_resend);
    }
}

static void red_release_video_encoder_buffer(uint8_t *data, void *opaque)
{
    VideoBuffer *buffer = (VideoBuffer*)opaque;
    buffer->free(buffer);
}

static char *
get_timestamp(char *buffer, uint64_t time)
{
    time /= 10000000u;
    time %= 3600llu * 100llu;
    unsigned hour = time / 100u;
    sprintf(buffer, "%02u:%02u.%02u", hour/60u, hour%60u, (unsigned) (time % 100u));
    return buffer;
}

static bool red_marshall_stream_data(RedChannelClient *rcc,
                                     SpiceMarshaller *base_marshaller,
                                     Drawable *drawable)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    DisplayChannel *display = DCC_TO_DC(dcc);
    VideoStream *stream = drawable->stream;
    RedDrawable *rd = drawable->red_drawable;
    SpiceCopy *copy;
    uint32_t frame_mm_time;
    int is_sized;
    int ret;

    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);

    copy = &drawable->red_drawable->u.copy;
    if (copy->src_bitmap->descriptor.type != SPICE_IMAGE_TYPE_BITMAP &&
        copy->src_bitmap->descriptor.type != SPICE_IMAGE_TYPE_DRM_PRIME) {
        return FALSE;
    }

    is_sized = (copy->src_area.right - copy->src_area.left != stream->width) ||
               (copy->src_area.bottom - copy->src_area.top != stream->height) ||
               !rect_is_equal(&drawable->red_drawable->bbox, &stream->dest_area);

    if (is_sized &&
        !red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_SIZED_STREAM)) {
        return FALSE;
    }

    int stream_id = display_channel_get_video_stream_id(display, stream);
    VideoStreamAgent *agent = &dcc->priv->stream_agents[stream_id];
    VideoBuffer *outbuf;
    /* workaround for vga streams */
    frame_mm_time =  drawable->red_drawable->mm_time ?
                        drawable->red_drawable->mm_time :
                        reds_get_mm_time();
    rd->time_encoding = spice_get_real_time_ns();
    ret = !agent->video_encoder ? VIDEO_ENCODER_FRAME_UNSUPPORTED :
          agent->video_encoder->encode_frame(agent->video_encoder,
                                             frame_mm_time,
                                             copy->src_bitmap,
                                             &copy->src_area, stream->top_down,
                                             drawable->red_drawable,
                                             &outbuf);
    rd->time_encoded = spice_get_real_time_ns();
#define TM(x) (((double) (rd->x - rd->time_queued)) / 1000000000.0)
    char tsbuf[32];
    printf("encoded res %d size %u times %s %g %g %g %g\n", ret, outbuf ? outbuf->size : 0,
           get_timestamp(tsbuf, rd->time_queued), TM(time_scanout) * 1000, TM(time_extracted), TM(time_encoding), TM(time_encoded));
#undef TM

    switch (ret) {
    case VIDEO_ENCODER_FRAME_DROP:
#ifdef STREAM_STATS
        agent->stats.num_drops_fps++;
#endif
        return TRUE;
    case VIDEO_ENCODER_FRAME_UNSUPPORTED:
        return FALSE;
    case VIDEO_ENCODER_FRAME_ENCODE_DONE:
        break;
    default:
        spice_error("bad return value (%d) from VideoEncoder::encode_frame", ret);
        return FALSE;
    }

    if (!is_sized) {
        SpiceMsgDisplayStreamData stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA);

        stream_data.base.id = stream_id;
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = outbuf->size;

        spice_marshall_msg_display_stream_data(base_marshaller, &stream_data);
    } else {
        SpiceMsgDisplayStreamDataSized stream_data;

        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA_SIZED);

        stream_data.base.id = stream_id;
        stream_data.base.multi_media_time = frame_mm_time;
        stream_data.data_size = outbuf->size;
        stream_data.width = copy->src_area.right - copy->src_area.left;
        stream_data.height = copy->src_area.bottom - copy->src_area.top;
        stream_data.dest = drawable->red_drawable->bbox;

        spice_debug("stream %d: sized frame: dest ==> ", stream_data.base.id);
        rect_debug(&stream_data.dest);
        spice_marshall_msg_display_stream_data_sized(base_marshaller, &stream_data);
    }
    spice_marshaller_add_by_ref_full(base_marshaller, outbuf->data, outbuf->size,
                                     &red_release_video_encoder_buffer, outbuf);
#ifdef STREAM_STATS
    agent->stats.num_frames_sent++;
    agent->stats.size_sent += outbuf->size;
    agent->stats.end = frame_mm_time;
#endif

    return TRUE;
}

static inline void marshall_inval_palette(RedChannelClient *rcc,
                                          SpiceMarshaller *base_marshaller,
                                          RedCacheItem *cache_item)
{
    SpiceMsgDisplayInvalOne inval_one;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_PALETTE);
    inval_one.id = cache_item->id;

    spice_marshall_msg_display_inval_palette(base_marshaller, &inval_one);

}

static void display_channel_marshall_migrate_data_surfaces(DisplayChannelClient *dcc,
                                                           SpiceMarshaller *m,
                                                           int lossy)
{
    SpiceMarshaller *m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    uint32_t num_surfaces_created;
    uint8_t *num_surfaces_created_ptr;
    uint32_t i;

    num_surfaces_created_ptr = spice_marshaller_reserve_space(m2, sizeof(uint32_t));
    num_surfaces_created = 0;
    for (i = 0; i < NUM_SURFACES; i++) {
        SpiceRect lossy_rect;

        if (!dcc->priv->surface_client_created[i]) {
            continue;
        }
        spice_marshaller_add_uint32(m2, i);
        num_surfaces_created++;

        if (!lossy) {
            continue;
        }
        region_extents(&dcc->priv->surface_client_lossy_region[i], &lossy_rect);
        spice_marshaller_add_int32(m2, lossy_rect.left);
        spice_marshaller_add_int32(m2, lossy_rect.top);
        spice_marshaller_add_int32(m2, lossy_rect.right);
        spice_marshaller_add_int32(m2, lossy_rect.bottom);
    }
    spice_marshaller_set_uint32(m2, num_surfaces_created_ptr, num_surfaces_created);
}

static void display_channel_marshall_migrate_data(RedChannelClient *rcc,
                                                  SpiceMarshaller *base_marshaller)
{
    DisplayChannel *display_channel;
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    ImageEncoders *encoders = dcc_get_encoders(dcc);
    SpiceMigrateDataDisplay display_data = {0,};

    display_channel = DISPLAY_CHANNEL(red_channel_client_get_channel(rcc));

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_MAGIC);
    spice_marshaller_add_uint32(base_marshaller, SPICE_MIGRATE_DATA_DISPLAY_VERSION);

    spice_assert(dcc->priv->pixmap_cache);
    spice_assert(MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == 4 &&
                 MIGRATE_DATA_DISPLAY_MAX_CACHE_CLIENTS == MAX_CACHE_CLIENTS);

    display_data.message_serial = red_channel_client_get_message_serial(rcc);
    display_data.low_bandwidth_setting = dcc_is_low_bandwidth(dcc);

    display_data.pixmap_cache_freezer = pixmap_cache_freeze(dcc->priv->pixmap_cache);
    display_data.pixmap_cache_id = dcc->priv->pixmap_cache->id;
    display_data.pixmap_cache_size = dcc->priv->pixmap_cache->size;
    memcpy(display_data.pixmap_cache_clients, dcc->priv->pixmap_cache->sync,
           sizeof(display_data.pixmap_cache_clients));

    image_encoders_glz_get_restore_data(encoders, &display_data.glz_dict_id,
                                        &display_data.glz_dict_data);

    /* all data besided the surfaces ref */
    spice_marshaller_add(base_marshaller,
                         (uint8_t *)&display_data, sizeof(display_data) - sizeof(uint32_t));
    display_channel_marshall_migrate_data_surfaces(dcc, base_marshaller,
                                                   display_channel->priv->enable_jpeg);
}

static void display_channel_marshall_pixmap_sync(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMsgWaitForChannels wait;
    PixmapCache *pixmap_cache;

    red_channel_client_init_send_data(rcc, SPICE_MSG_WAIT_FOR_CHANNELS);
    pixmap_cache = dcc->priv->pixmap_cache;

    pthread_mutex_lock(&pixmap_cache->lock);

    wait.wait_count = 1;
    wait.wait_list[0].channel_type = SPICE_CHANNEL_DISPLAY;
    wait.wait_list[0].channel_id = pixmap_cache->generation_initiator.client;
    wait.wait_list[0].message_serial = pixmap_cache->generation_initiator.message;
    dcc->priv->pixmap_cache_generation = pixmap_cache->generation;
    dcc->priv->pending_pixmaps_sync = FALSE;

    pthread_mutex_unlock(&pixmap_cache->lock);

    spice_marshall_msg_wait_for_channels(base_marshaller, &wait);
}

static void dcc_pixmap_cache_reset(DisplayChannelClient *dcc, SpiceMsgWaitForChannels* sync_data)
{
    PixmapCache *cache = dcc->priv->pixmap_cache;
    uint8_t wait_count;
    uint64_t serial;
    uint32_t i;

    serial = red_channel_client_get_message_serial(RED_CHANNEL_CLIENT(dcc));
    pthread_mutex_lock(&cache->lock);
    pixmap_cache_clear(cache);

    dcc->priv->pixmap_cache_generation = ++cache->generation;
    cache->generation_initiator.client = dcc->priv->id;
    cache->generation_initiator.message = serial;
    cache->sync[dcc->priv->id] = serial;

    wait_count = 0;
    for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
        if (cache->sync[i] && i != dcc->priv->id) {
            sync_data->wait_list[wait_count].channel_type = SPICE_CHANNEL_DISPLAY;
            sync_data->wait_list[wait_count].channel_id = i;
            sync_data->wait_list[wait_count++].message_serial = cache->sync[i];
        }
    }
    sync_data->wait_count = wait_count;
    pthread_mutex_unlock(&cache->lock);
}

static void display_channel_marshall_reset_cache(RedChannelClient *rcc,
                                                 SpiceMarshaller *base_marshaller)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMsgWaitForChannels wait;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS);
    dcc_pixmap_cache_reset(dcc, &wait);

    spice_marshall_msg_display_inval_all_pixmaps(base_marshaller,
                                                 &wait);
}

static void red_marshall_image(RedChannelClient *rcc,
                               SpiceMarshaller *m,
                               RedImageItem *item)
{
    DisplayChannelClient *dcc;
    DisplayChannel *display;
    SpiceImage red_image;
    SpiceBitmap bitmap;
    SpiceChunks *chunks;
    QRegion *surface_lossy_region;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;
    SpiceMarshaller *bitmap_palette_out, *lzplt_palette_out;

    spice_assert(rcc && item);

    dcc = DISPLAY_CHANNEL_CLIENT(rcc);

    display = DCC_TO_DC(dcc);
    spice_assert(display);

    QXL_SET_IMAGE_ID(&red_image, QXL_IMAGE_GROUP_RED, display_channel_generate_uid(display));
    red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    red_image.descriptor.flags = item->image_flags;
    red_image.descriptor.width = item->width;
    red_image.descriptor.height = item->height;

    bitmap.format = item->image_format;
    bitmap.flags = 0;
    if (item->top_down) {
        bitmap.flags |= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
    bitmap.x = item->width;
    bitmap.y = item->height;
    bitmap.stride = item->stride;
    bitmap.palette = 0;
    bitmap.palette_id = 0;

    chunks = spice_chunks_new_linear(item->data, bitmap.stride * bitmap.y);
    bitmap.data = chunks;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY);

    copy.base.surface_id = item->surface_id;
    copy.base.box.left = item->pos.x;
    copy.base.box.top = item->pos.y;
    copy.base.box.right = item->pos.x + bitmap.x;
    copy.base.box.bottom = item->pos.y + bitmap.y;
    copy.base.clip.type = SPICE_CLIP_TYPE_NONE;
    copy.data.rop_descriptor = SPICE_ROPD_OP_PUT;
    copy.data.src_area.left = 0;
    copy.data.src_area.top = 0;
    copy.data.src_area.right = bitmap.x;
    copy.data.src_area.bottom = bitmap.y;
    copy.data.scale_mode = 0;
    copy.data.src_bitmap = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.flags = 0;
    copy.data.mask.pos.x = 0;
    copy.data.mask.pos.y = 0;
    copy.data.mask.bitmap = 0;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    compress_send_data_t comp_send_data = {0};

    int comp_succeeded = dcc_compress_image(dcc, &red_image, &bitmap, NULL, item->can_lossy, &comp_send_data);

    surface_lossy_region = &dcc->priv->surface_client_lossy_region[item->surface_id];
    if (comp_succeeded) {
        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);

        marshaller_add_compressed(src_bitmap_out,
                                  comp_send_data.comp_buf, comp_send_data.comp_buf_size);

        if (lzplt_palette_out && comp_send_data.lzplt_palette) {
            spice_marshall_Palette(lzplt_palette_out, comp_send_data.lzplt_palette);
        }

        if (spice_image_descriptor_is_lossy(&red_image.descriptor)) {
            region_add(surface_lossy_region, &copy.base.box);
        } else {
            region_remove(surface_lossy_region, &copy.base.box);
        }
    } else {
        red_image.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        red_image.u.bitmap = bitmap;

        spice_marshall_Image(src_bitmap_out, &red_image,
                             &bitmap_palette_out, &lzplt_palette_out);
        red_pipe_item_ref(&item->base);
        spice_marshaller_add_by_ref_full(src_bitmap_out, item->data,
                                         bitmap.y * bitmap.stride,
                                         marshaller_unref_pipe_item, item);
        region_remove(surface_lossy_region, &copy.base.box);
    }
    spice_chunks_destroy(chunks);
}

static void marshall_lossy_qxl_drawable(RedChannelClient *rcc,
                                        SpiceMarshaller *base_marshaller,
                                        RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    switch (item->red_drawable->type) {
    case QXL_DRAW_FILL:
        red_lossy_marshall_qxl_draw_fill(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_lossy_marshall_qxl_draw_opaque(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COPY:
        red_lossy_marshall_qxl_draw_copy(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_lossy_marshall_qxl_draw_transparent(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_lossy_marshall_qxl_draw_alpha_blend(rcc, base_marshaller, dpi);
        break;
    case QXL_COPY_BITS:
        red_lossy_marshall_qxl_copy_bits(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_lossy_marshall_qxl_draw_blend(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_lossy_marshall_qxl_draw_blackness(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_lossy_marshall_qxl_draw_whiteness(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_lossy_marshall_qxl_draw_inverse(rcc, base_marshaller, item);
        break;
    case QXL_DRAW_ROP3:
        red_lossy_marshall_qxl_draw_rop3(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_lossy_marshall_qxl_draw_composite(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_lossy_marshall_qxl_draw_stroke(rcc, base_marshaller, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_lossy_marshall_qxl_draw_text(rcc, base_marshaller, dpi);
        break;
    default:
        spice_warn_if_reached();
    }
}

static void marshall_lossless_qxl_drawable(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           RedDrawablePipeItem *dpi)
{
    Drawable *item = dpi->drawable;
    RedDrawable *drawable = item->red_drawable;

    switch (drawable->type) {
    case QXL_DRAW_FILL:
        marshall_qxl_draw_fill(rcc, m, dpi);
        break;
    case QXL_DRAW_OPAQUE:
        red_marshall_qxl_draw_opaque(rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_COPY:
        red_marshall_qxl_draw_copy(rcc, m, dpi, FALSE);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_marshall_qxl_draw_transparent(rcc, m, dpi);
        break;
    case QXL_DRAW_ALPHA_BLEND:
        red_marshall_qxl_draw_alpha_blend(rcc, m, dpi, FALSE);
        break;
    case QXL_COPY_BITS:
        red_marshall_qxl_copy_bits(rcc, m, dpi);
        break;
    case QXL_DRAW_BLEND:
        red_marshall_qxl_draw_blend(rcc, m, dpi);
        break;
    case QXL_DRAW_BLACKNESS:
        red_marshall_qxl_draw_blackness(rcc, m, dpi);
        break;
    case QXL_DRAW_WHITENESS:
        red_marshall_qxl_draw_whiteness(rcc, m, dpi);
        break;
    case QXL_DRAW_INVERS:
        red_marshall_qxl_draw_inverse(rcc, m, item);
        break;
    case QXL_DRAW_ROP3:
        red_marshall_qxl_draw_rop3(rcc, m, dpi);
        break;
    case QXL_DRAW_STROKE:
        red_marshall_qxl_draw_stroke(rcc, m, dpi);
        break;
    case QXL_DRAW_COMPOSITE:
        red_marshall_qxl_draw_composite(rcc, m, dpi);
        break;
    case QXL_DRAW_TEXT:
        red_marshall_qxl_draw_text(rcc, m, dpi);
        break;
    default:
        spice_warn_if_reached();
    }
}

void image_extract_drm(SpiceImage *image)
{
    if (image->descriptor.type != SPICE_IMAGE_TYPE_DRM_PRIME) {
        return;
    }
    spice_debug("raw data extracted from texture\n");

    size_t data_size;
    void *data = get_scanout_raw_data(&image->u.drm_prime, &data_size);
    spice_assert(data != NULL);

    /* convert the image from DRM_PRIME to BITMAP */
    SpiceChunks *chunks = spice_chunks_new(1);
    chunks->flags = SPICE_CHUNKS_FLAGS_FREE;
    chunks->data_size = data_size;
    chunks->chunk[0].data = data;
    chunks->chunk[0].len = data_size;

    uint32_t w = image->u.drm_prime.width;
    uint32_t h = image->u.drm_prime.height;
    uint8_t flags = image->u.drm_prime.flags & 1 ? 0 : SPICE_BITMAP_FLAGS_TOP_DOWN;
    close(image->u.drm_prime.drm_dma_buf_fd);

    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->u.bitmap.x = w;
    image->u.bitmap.y = h;
    image->u.bitmap.format = SPICE_BITMAP_FMT_32BIT;
    image->u.bitmap.flags = flags;
    image->u.bitmap.stride = w * 4;
    image->u.bitmap.data = chunks;
    image->u.bitmap.palette = NULL;
    image->u.bitmap.palette_id = 0;
}

void red_drawable_extract_drm(RedDrawable *red_drawable)
{
    if (red_drawable->effect != QXL_EFFECT_OPAQUE
        || red_drawable->type != QXL_DRAW_COPY) {
        return;
    }

    image_extract_drm(red_drawable->u.copy.src_bitmap);
}

static void marshall_qxl_drawable(RedChannelClient *rcc,
                                  SpiceMarshaller *m,
                                  RedDrawablePipeItem *dpi)
{
    spice_return_if_fail(rcc);

    Drawable *item = dpi->drawable;
    DisplayChannel *display =
        DISPLAY_CHANNEL(red_channel_client_get_channel(rcc));

    spice_return_if_fail(display);
    /* allow sized frames to be streamed, even if they where replaced by another frame, since
     * newer frames might not cover sized frames completely if they are bigger */
    if (item->stream && red_marshall_stream_data(rcc, m, item)) {
        return;
    }
    red_drawable_extract_drm(item->red_drawable);
    if (display->priv->enable_jpeg)
        marshall_lossy_qxl_drawable(rcc, m, dpi);
    else
        marshall_lossless_qxl_drawable(rcc, m, dpi);
}

static void marshall_stream_start(RedChannelClient *rcc,
                                  SpiceMarshaller *base_marshaller,
                                  VideoStreamAgent *agent)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    VideoStream *stream = agent->stream;

    spice_assert(stream);
    if (!agent->video_encoder) {
        /* Without a video encoder nothing will be streamed */
        return;
    }
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CREATE);
    SpiceMsgDisplayStreamCreate stream_create;
    SpiceClipRects clip_rects;

    stream_create.surface_id = 0;
    stream_create.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), stream);
    stream_create.flags = stream->top_down ? SPICE_STREAM_FLAGS_TOP_DOWN : 0;
    stream_create.codec_type = agent->video_encoder->codec_type;

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

static void marshall_stream_clip(RedChannelClient *rcc,
                                 SpiceMarshaller *base_marshaller,
                                 VideoStreamClipItem *item)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    VideoStreamAgent *agent = item->stream_agent;

    spice_return_if_fail(agent->stream);

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CLIP);
    SpiceMsgDisplayStreamClip stream_clip;

    stream_clip.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), agent->stream);
    stream_clip.clip.type = item->clip_type;
    stream_clip.clip.rects = item->rects;

    spice_marshall_msg_display_stream_clip(base_marshaller, &stream_clip);
}

static void marshall_stream_end(RedChannelClient *rcc,
                                SpiceMarshaller *base_marshaller,
                                VideoStreamAgent* agent)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMsgDisplayStreamDestroy destroy;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DESTROY);
    destroy.id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), agent->stream);
    video_stream_agent_stop(agent);
    spice_marshall_msg_display_stream_destroy(base_marshaller, &destroy);
}

static void marshall_upgrade(RedChannelClient *rcc, SpiceMarshaller *m,
                             RedUpgradeItem *item)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    RedChannel *channel = red_channel_client_get_channel(rcc);
    RedDrawable *red_drawable;
    SpiceMsgDisplayDrawCopy copy;
    SpiceMarshaller *src_bitmap_out, *mask_bitmap_out;

    spice_assert(channel && item && item->drawable);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_COPY);

    red_drawable = item->drawable->red_drawable;
    spice_assert(red_drawable->type == QXL_DRAW_COPY);
    spice_assert(red_drawable->u.copy.rop_descriptor == SPICE_ROPD_OP_PUT);
    spice_assert(red_drawable->u.copy.mask.bitmap == 0);
    red_drawable_extract_drm(red_drawable);

    copy.base.surface_id = 0;
    copy.base.box = red_drawable->bbox;
    copy.base.clip.type = SPICE_CLIP_TYPE_RECTS;
    copy.base.clip.rects = item->rects;
    copy.data = red_drawable->u.copy;

    spice_marshall_msg_display_draw_copy(m, &copy,
                                         &src_bitmap_out, &mask_bitmap_out);

    fill_bits(dcc, src_bitmap_out, copy.data.src_bitmap, item->drawable, FALSE);
}

static void marshall_surface_create(RedChannelClient *rcc,
                                    SpiceMarshaller *base_marshaller,
                                    SpiceMsgSurfaceCreate *surface_create)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);

    region_init(&dcc->priv->surface_client_lossy_region[surface_create->surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_CREATE);

    spice_marshall_msg_display_surface_create(base_marshaller, surface_create);
}

static void marshall_surface_destroy(RedChannelClient *rcc,
                                     SpiceMarshaller *base_marshaller, uint32_t surface_id)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMsgSurfaceDestroy surface_destroy;

    region_destroy(&dcc->priv->surface_client_lossy_region[surface_id]);
    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_DESTROY);

    surface_destroy.surface_id = surface_id;

    spice_marshall_msg_display_surface_destroy(base_marshaller, &surface_destroy);
}

static void marshall_monitors_config(RedChannelClient *rcc, SpiceMarshaller *base_marshaller,
                                     MonitorsConfig *monitors_config)
{
    int heads_size = sizeof(SpiceHead) * monitors_config->count;
    int i;
    SpiceMsgDisplayMonitorsConfig *msg = g_malloc0(sizeof(*msg) + heads_size);
    int count = 0; // ignore monitors_config->count, it may contain zero width monitors, remove them now

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_MONITORS_CONFIG);
    for (i = 0 ; i < monitors_config->count; ++i) {
        if (monitors_config->heads[i].width == 0 || monitors_config->heads[i].height == 0) {
            continue;
        }
        msg->heads[count].monitor_id = monitors_config->heads[i].id;
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
    g_free(msg);
}

static void marshall_stream_activate_report(RedChannelClient *rcc,
                                            SpiceMarshaller *base_marshaller,
                                            RedStreamActivateReportItem *report_item)
{
    SpiceMsgDisplayStreamActivateReport msg;

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT);
    msg.stream_id = report_item->stream_id;
    msg.unique_id = report_item->report_id;
    msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
    msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
    spice_marshall_msg_display_stream_activate_report(base_marshaller, &msg);
}

static void marshall_gl_scanout(RedChannelClient *rcc,
                                SpiceMarshaller *m,
                                RedPipeItem *item)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    QXLInstance* qxl = display_channel->priv->qxl;

    SpiceMsgDisplayGlScanoutUnix *scanout = red_qxl_get_gl_scanout(qxl);
    if (scanout != NULL) {
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_GL_SCANOUT_UNIX);
        spice_marshall_msg_display_gl_scanout_unix(m, scanout);
    }
    red_qxl_put_gl_scanout(qxl, scanout);
}

static void marshall_gl_draw(RedChannelClient *rcc,
                             SpiceMarshaller *m,
                             RedPipeItem *item)
{
    RedGlDrawItem *p = SPICE_UPCAST(RedGlDrawItem, item);

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_GL_DRAW);
    spice_marshall_msg_display_gl_draw(m, &p->draw);
}


static void begin_send_message(RedChannelClient *rcc)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    FreeList *free_list = &dcc->priv->send_data.free_list;

    if (free_list->res->count) {
        int sync_count = 0;
        int i;

        for (i = 0; i < MAX_CACHE_CLIENTS; i++) {
            if (i != dcc->priv->id && free_list->sync[i] != 0) {
                free_list->wait.header.wait_list[sync_count].channel_type = SPICE_CHANNEL_DISPLAY;
                free_list->wait.header.wait_list[sync_count].channel_id = i;
                free_list->wait.header.wait_list[sync_count++].message_serial = free_list->sync[i];
            }
        }
        free_list->wait.header.wait_count = sync_count;

        if (red_channel_client_is_mini_header(rcc)) {
            send_free_list(rcc);
        } else {
            send_free_list_legacy(rcc);
        }
    }
    red_channel_client_begin_send_message(rcc);
}

static void reset_send_data(DisplayChannelClient *dcc)
{
    dcc->priv->send_data.free_list.res->count = 0;
    dcc->priv->send_data.num_pixmap_cache_items = 0;
    memset(dcc->priv->send_data.free_list.sync, 0,
           sizeof(dcc->priv->send_data.free_list.sync));
}

void dcc_send_item(RedChannelClient *rcc, RedPipeItem *pipe_item)
{
    DisplayChannelClient *dcc = DISPLAY_CHANNEL_CLIENT(rcc);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    reset_send_data(dcc);
    switch (pipe_item->type) {
    case RED_PIPE_ITEM_TYPE_DRAW: {
        RedDrawablePipeItem *dpi = SPICE_UPCAST(RedDrawablePipeItem, pipe_item);
        marshall_qxl_drawable(rcc, m, dpi);
        break;
    }
    case RED_PIPE_ITEM_TYPE_INVAL_ONE:
        marshall_inval_palette(rcc, m, SPICE_CONTAINEROF(pipe_item, RedCacheItem, u.pipe_data));
        break;
    case RED_PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamCreateDestroyItem *item = SPICE_UPCAST(StreamCreateDestroyItem, pipe_item);
        marshall_stream_start(rcc, m, item->agent);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_CLIP:
        marshall_stream_clip(rcc, m, SPICE_UPCAST(VideoStreamClipItem, pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_STREAM_DESTROY: {
        StreamCreateDestroyItem *item = SPICE_UPCAST(StreamCreateDestroyItem, pipe_item);
        marshall_stream_end(rcc, m, item->agent);
        break;
    }
    case RED_PIPE_ITEM_TYPE_UPGRADE:
        marshall_upgrade(rcc, m, SPICE_UPCAST(RedUpgradeItem, pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_MIGRATE_DATA:
        display_channel_marshall_migrate_data(rcc, m);
        break;
    case RED_PIPE_ITEM_TYPE_IMAGE:
        red_marshall_image(rcc, m, SPICE_UPCAST(RedImageItem, pipe_item));
        break;
    case RED_PIPE_ITEM_TYPE_PIXMAP_SYNC:
        display_channel_marshall_pixmap_sync(rcc, m);
        break;
    case RED_PIPE_ITEM_TYPE_PIXMAP_RESET:
        display_channel_marshall_reset_cache(rcc, m);
        break;
    case RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE:
        dcc_palette_cache_reset(dcc);
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);
        break;
    case RED_PIPE_ITEM_TYPE_CREATE_SURFACE: {
        RedSurfaceCreateItem *surface_create = SPICE_UPCAST(RedSurfaceCreateItem, pipe_item);
        marshall_surface_create(rcc, m, &surface_create->surface_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_DESTROY_SURFACE: {
        RedSurfaceDestroyItem *surface_destroy = SPICE_UPCAST(RedSurfaceDestroyItem, pipe_item);
        marshall_surface_destroy(rcc, m, surface_destroy->surface_destroy.surface_id);
        break;
    }
    case RED_PIPE_ITEM_TYPE_MONITORS_CONFIG: {
        RedMonitorsConfigItem *monconf_item = SPICE_UPCAST(RedMonitorsConfigItem, pipe_item);
        marshall_monitors_config(rcc, m, monconf_item->monitors_config);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        RedStreamActivateReportItem *report_item =
            SPICE_UPCAST(RedStreamActivateReportItem, pipe_item);
        marshall_stream_activate_report(rcc, m, report_item);
        break;
    }
    case RED_PIPE_ITEM_TYPE_GL_SCANOUT:
        marshall_gl_scanout(rcc, m, pipe_item);
        break;
    case RED_PIPE_ITEM_TYPE_GL_DRAW:
        marshall_gl_draw(rcc, m, pipe_item);
        break;
    default:
        spice_warn_if_reached();
    }

    // a message is pending
    if (red_channel_client_send_message_pending(rcc)) {
        begin_send_message(rcc);
    }
}
