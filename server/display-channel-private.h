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

#ifndef DISPLAY_CHANNEL_PRIVATE_H_
#define DISPLAY_CHANNEL_PRIVATE_H_

#include "display-channel.h"

#define TRACE_ITEMS_SHIFT 3
#define NUM_TRACE_ITEMS (1 << TRACE_ITEMS_SHIFT)
#define ITEMS_TRACE_MASK (NUM_TRACE_ITEMS - 1)

typedef struct DrawContext {
    SpiceCanvas *canvas;
    int canvas_draws_on_surface;
    int top_down;
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    void *line_0;
} DrawContext;

typedef struct RedSurface {
    uint32_t refs;
    /* A Ring representing a hierarchical tree structure. This tree includes
     * DrawItems, Containers, and Shadows. It is used to efficiently determine
     * which drawables overlap, and to exclude regions of drawables that are
     * obscured by other drawables */
    Ring current;
    /* A ring of pending Drawables associated with this surface. This ring is
     * actually used for drawing. The ring is maintained in order of age, the
     * tail being the oldest drawable. */
    Ring current_list;
    DrawContext context;

    Ring depend_on_me;
    QRegion draw_dirty_region;

    //fix me - better handling here
    /* 'create_cmd' holds surface data through a pointer to guest memory, it
     * must be valid as long as the surface is valid */
    RedSurfaceCmd *create_cmd;
    /* QEMU expects the guest data for the command to be valid as long as the
     * surface is valid */
    RedSurfaceCmd *destroy_cmd;
} RedSurface;

typedef struct MonitorsConfig {
    int refs;
    int count;
    int max_allowed;
    QXLHead heads[0];
} MonitorsConfig;

#define NUM_DRAWABLES 1000
typedef struct _Drawable _Drawable;
struct _Drawable {
    union {
        Drawable drawable;
        _Drawable *next;
    } u;
};

struct DisplayChannelPrivate
{
    DisplayChannel *pub;

    QXLInstance *qxl;

    uint32_t bits_unique;

    MonitorsConfig *monitors_config;

    uint32_t renderer;
    int enable_jpeg;
    int enable_zlib_glz_wrap;

    /* A ring of pending drawables for this DisplayChannel, regardless of which
     * surface they're associated with. This list is mainly used to flush older
     * drawables when we need to make room for new drawables.  The ring is
     * maintained in order of age, the tail being the oldest drawable */
    Ring current_list;

    uint32_t drawable_count;
    _Drawable drawables[NUM_DRAWABLES];
    _Drawable *free_drawables;

    int stream_video;
    GArray *video_codecs;
    uint32_t stream_count;
    VideoStream streams_buf[NUM_STREAMS];
    VideoStream *free_streams;
    Ring streams;
    ItemTrace items_trace[NUM_TRACE_ITEMS];
    uint32_t next_item_trace;
    uint64_t streams_size_total;

    RedSurface surfaces[NUM_SURFACES];
    uint32_t n_surfaces;
    SpiceImageSurfaces image_surfaces;

    ImageCache image_cache;

    int gl_draw_async_count;
    VideoStream *forced_stream;

/* TODO: some day unify this, make it more runtime.. */
    stat_info_t add_stat;
    stat_info_t exclude_stat;
    stat_info_t __exclude_stat;
#ifdef RED_WORKER_STAT
    uint32_t add_count;
    uint32_t add_with_shadow_count;
#endif
    RedStatCounter cache_hits_counter;
    RedStatCounter add_to_cache_counter;
    RedStatCounter non_cache_counter;
    ImageEncoderSharedData encoder_shared_data;
};

#define FOREACH_DCC(_channel, _data) \
    GLIST_FOREACH((_channel ? red_channel_get_clients(RED_CHANNEL(_channel)) : NULL), \
                  DisplayChannelClient, _data)

typedef struct RedMonitorsConfigItem {
    RedPipeItem base;
    MonitorsConfig *monitors_config;
} RedMonitorsConfigItem;

enum {
    RED_PIPE_ITEM_TYPE_DRAW = RED_PIPE_ITEM_TYPE_COMMON_LAST,
    RED_PIPE_ITEM_TYPE_IMAGE,
    RED_PIPE_ITEM_TYPE_STREAM_CREATE,
    RED_PIPE_ITEM_TYPE_STREAM_CLIP,
    RED_PIPE_ITEM_TYPE_STREAM_DESTROY,
    RED_PIPE_ITEM_TYPE_UPGRADE,
    RED_PIPE_ITEM_TYPE_MIGRATE_DATA,
    RED_PIPE_ITEM_TYPE_PIXMAP_SYNC,
    RED_PIPE_ITEM_TYPE_PIXMAP_RESET,
    RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE,
    RED_PIPE_ITEM_TYPE_CREATE_SURFACE,
    RED_PIPE_ITEM_TYPE_DESTROY_SURFACE,
    RED_PIPE_ITEM_TYPE_MONITORS_CONFIG,
    RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
    RED_PIPE_ITEM_TYPE_GL_SCANOUT,
    RED_PIPE_ITEM_TYPE_GL_DRAW,
};

void drawable_unref(Drawable *drawable);

MonitorsConfig *monitors_config_ref(MonitorsConfig *config);
void monitors_config_unref(MonitorsConfig *config);

void display_channel_draw_until(DisplayChannel *display,
                                const SpiceRect *area,
                                int surface_id,
                                Drawable *last);
GArray* display_channel_get_video_codecs(DisplayChannel *display);
int display_channel_get_stream_video(DisplayChannel *display);
void display_channel_current_flush(DisplayChannel *display,
                                   int surface_id);
uint32_t display_channel_generate_uid(DisplayChannel *display);

int display_channel_get_video_stream_id(DisplayChannel *display, VideoStream *stream);
VideoStream *display_channel_get_nth_video_stream(DisplayChannel *display, gint i);

typedef struct RedSurfaceDestroyItem {
    RedPipeItem base;
    SpiceMsgSurfaceDestroy surface_destroy;
} RedSurfaceDestroyItem;

static inline int is_equal_path(SpicePath *path1, SpicePath *path2)
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
static inline int is_equal_brush(SpiceBrush *b1, SpiceBrush *b2)
{
    return b1->type == b2->type &&
           b1->type == SPICE_BRUSH_TYPE_SOLID &&
           b1->u.color == b2->u.color;
}

// partial imp
static inline int is_equal_line_attr(SpiceLineAttr *a1, SpiceLineAttr *a2)
{
    return a1->flags == a2->flags &&
           a1->style_nseg == a2->style_nseg &&
           a1->style_nseg == 0;
}

// partial imp
static inline int is_same_geometry(Drawable *d1, Drawable *d2)
{
    if (d1->red_drawable->type != d2->red_drawable->type) {
        return FALSE;
    }

    switch (d1->red_drawable->type) {
    case QXL_DRAW_STROKE:
        return is_equal_line_attr(&d1->red_drawable->u.stroke.attr,
                                  &d2->red_drawable->u.stroke.attr) &&
               is_equal_path(d1->red_drawable->u.stroke.path,
                             d2->red_drawable->u.stroke.path);
    case QXL_DRAW_FILL:
        return rect_is_equal(&d1->red_drawable->bbox, &d2->red_drawable->bbox);
    default:
        return FALSE;
    }
}

static inline int is_same_drawable(Drawable *d1, Drawable *d2)
{
    if (!is_same_geometry(d1, d2)) {
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

static inline int is_drawable_independent_from_surfaces(Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        if (drawable->surface_deps[x] != -1) {
            return FALSE;
        }
    }
    return TRUE;
}

static inline int has_shadow(RedDrawable *drawable)
{
    return drawable->type == QXL_COPY_BITS;
}

static inline int is_primary_surface(DisplayChannel *display, uint32_t surface_id)
{
    if (surface_id == 0) {
        return TRUE;
    }
    return FALSE;
}

static inline void region_add_clip_rects(QRegion *rgn, SpiceClipRects *data)
{
    int i;

    for (i = 0; i < data->num_rects; i++) {
        region_add(rgn, data->rects + i);
    }
}

#endif /* DISPLAY_CHANNEL_PRIVATE_H_ */
