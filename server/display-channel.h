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

#ifndef DISPLAY_CHANNEL_H_
# define DISPLAY_CHANNEL_H_

#include <setjmp.h>
#include <common/rect.h>

#include "reds-stream.h"
#include "cache-item.h"
#include "pixmap-cache.h"
#include "stat.h"
#include "reds.h"
#include "memslot.h"
#include "red-parse-qxl.h"
#include "red-record-qxl.h"
#include "demarshallers.h"
#include "red-channel.h"
#include "red-qxl.h"
#include "dispatcher.h"
#include "main-channel.h"
#include "migration-protocol.h"
#include "main-dispatcher.h"
#include "spice-bitmap-utils.h"
#include "utils.h"
#include "tree.h"
#include "stream.h"
#include "dcc.h"
#include "image-encoders.h"
#include "common-graphics-channel.h"

G_BEGIN_DECLS

#define TYPE_DISPLAY_CHANNEL display_channel_get_type()

#define DISPLAY_CHANNEL(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_DISPLAY_CHANNEL, DisplayChannel))
#define DISPLAY_CHANNEL_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_DISPLAY_CHANNEL, DisplayChannelClass))
#define IS_DISPLAY_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_DISPLAY_CHANNEL))
#define IS_DISPLAY_CHANNEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_DISPLAY_CHANNEL))
#define DISPLAY_CHANNEL_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_DISPLAY_CHANNEL, DisplayChannelClass))

typedef struct DisplayChannelClass DisplayChannelClass;
typedef struct DisplayChannelPrivate DisplayChannelPrivate;

struct DisplayChannel
{
    CommonGraphicsChannel parent;

    DisplayChannelPrivate *priv;
};

struct DisplayChannelClass
{
    CommonGraphicsChannelClass parent_class;
};

GType display_channel_get_type(void) G_GNUC_CONST;

typedef struct DependItem {
    Drawable *drawable;
    RingItem ring_item;
} DependItem;

struct Drawable {
    uint32_t refs;
    RingItem surface_list_link;
    RingItem list_link;
    DrawItem tree_item;
    GList *pipes;
    RedDrawable *red_drawable;

    GlzImageRetention glz_retention;

    red_time_t creation_time;
    red_time_t first_frame_time;
    int frames_count;
    int gradual_frames_count;
    int last_gradual_frame;
    Stream *stream;
    int streamable;
    BitmapGradualType copy_bitmap_graduality;
    DependItem depend_items[3];

    int surface_id;
    int surface_deps[3];

    uint32_t process_commands_generation;
    DisplayChannel *display;
};

void drawable_unref (Drawable *drawable);

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

typedef struct MonitorsConfig {
    int refs;
    int count;
    int max_allowed;
    QXLHead heads[0];
} MonitorsConfig;

typedef struct RedMonitorsConfigItem {
    RedPipeItem pipe_item;
    MonitorsConfig *monitors_config;
} RedMonitorsConfigItem;

MonitorsConfig *           monitors_config_ref                       (MonitorsConfig *config);
void                       monitors_config_unref                     (MonitorsConfig *config);

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
    QXLReleaseInfoExt create, destroy;
} RedSurface;

#define FOREACH_DCC(_channel, _iter, _data) \
    GLIST_FOREACH((_channel ? red_channel_get_clients(RED_CHANNEL(_channel)) : NULL), \
                  _iter, DisplayChannelClient, _data)

int display_channel_get_stream_id(DisplayChannel *display, Stream *stream);

typedef struct RedSurfaceDestroyItem {
    RedPipeItem pipe_item;
    SpiceMsgSurfaceDestroy surface_destroy;
} RedSurfaceDestroyItem;

typedef struct RedUpgradeItem {
    RedPipeItem base;
    Drawable *drawable;
    SpiceClipRects *rects;
} RedUpgradeItem;


DisplayChannel*            display_channel_new                       (RedsState *reds,
                                                                      QXLInstance *qxl,
                                                                      const SpiceCoreInterfaceInternal *core,
                                                                      int migrate,
                                                                      int stream_video,
                                                                      GArray *video_codecs,
                                                                      uint32_t n_surfaces);
void                       display_channel_create_surface            (DisplayChannel *display, uint32_t surface_id,
                                                                      uint32_t width, uint32_t height,
                                                                      int32_t stride, uint32_t format, void *line_0,
                                                                      int data_is_valid, int send_client);
void                       display_channel_draw                      (DisplayChannel *display,
                                                                      const SpiceRect *area,
                                                                      int surface_id);
void                       display_channel_draw_until                (DisplayChannel *display,
                                                                      const SpiceRect *area,
                                                                      int surface_id,
                                                                      Drawable *last);
void                       display_channel_update                    (DisplayChannel *display,
                                                                      uint32_t surface_id,
                                                                      const QXLRect *area,
                                                                      uint32_t clear_dirty,
                                                                      QXLRect **qxl_dirty_rects,
                                                                      uint32_t *num_dirty_rects);
void                       display_channel_free_some                 (DisplayChannel *display);
void                       display_channel_set_stream_video          (DisplayChannel *display,
                                                                      int stream_video);
void                       display_channel_set_video_codecs          (DisplayChannel *display,
                                                                      GArray *video_codecs);
GArray*                    display_channel_get_video_codecs          (DisplayChannel *display);
int                        display_channel_get_stream_video          (DisplayChannel *display);
int                        display_channel_get_streams_timeout       (DisplayChannel *display);
void                       display_channel_compress_stats_print      (DisplayChannel *display);
void                       display_channel_compress_stats_reset      (DisplayChannel *display);
void                       display_channel_surface_unref             (DisplayChannel *display,
                                                                      uint32_t surface_id);
void                       display_channel_current_flush             (DisplayChannel *display,
                                                                      int surface_id);
bool                       display_channel_wait_for_migrate_data     (DisplayChannel *display);
void                       display_channel_flush_all_surfaces        (DisplayChannel *display);
void                       display_channel_free_glz_drawables_to_free(DisplayChannel *display);
void                       display_channel_free_glz_drawables        (DisplayChannel *display);
void                       display_channel_destroy_surface_wait      (DisplayChannel *display,
                                                                      uint32_t surface_id);
void                       display_channel_destroy_surfaces          (DisplayChannel *display);
uint32_t                   display_channel_generate_uid              (DisplayChannel *display);
void                       display_channel_process_draw              (DisplayChannel *display,
                                                                      RedDrawable *red_drawable,
                                                                      uint32_t process_commands_generation);
void                       display_channel_process_surface_cmd       (DisplayChannel *display,
                                                                      const RedSurfaceCmd *surface_cmd,
                                                                      int loadvm);
void                       display_channel_update_compression        (DisplayChannel *display,
                                                                      DisplayChannelClient *dcc);
void                       display_channel_gl_scanout                (DisplayChannel *display);
void                       display_channel_gl_draw                   (DisplayChannel *display,
                                                                      SpiceMsgDisplayGlDraw *draw);
void                       display_channel_gl_draw_done              (DisplayChannel *display);

void display_channel_update_monitors_config(DisplayChannel *display, QXLMonitorsConfig *config,
                                            uint16_t count, uint16_t max_allowed);
void display_channel_set_monitors_config_to_primary(DisplayChannel *display);

gboolean display_channel_validate_surface(DisplayChannel *display, uint32_t surface_id);
gboolean display_channel_surface_has_canvas(DisplayChannel *display, uint32_t surface_id);
void display_channel_reset_image_cache(DisplayChannel *self);

void display_channel_debug_oom(DisplayChannel *display, const char *msg);

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

G_END_DECLS

#endif /* DISPLAY_CHANNEL_H_ */
