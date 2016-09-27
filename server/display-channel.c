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

#include <common/sw_canvas.h>

#include "display-channel-private.h"
#include "glib-compat.h"
#include "red-qxl.h"
#include "egl.h"

G_DEFINE_TYPE(DisplayChannel, display_channel, TYPE_COMMON_GRAPHICS_CHANNEL)

enum {
    PROP0,
    PROP_N_SURFACES,
    PROP_VIDEO_CODECS,
    PROP_QXL
};

static void
display_channel_get_property(GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
    DisplayChannel *self = DISPLAY_CHANNEL(object);

    switch (property_id)
    {
        case PROP_N_SURFACES:
            g_value_set_uint(value, self->priv->n_surfaces);
            break;
        case PROP_VIDEO_CODECS:
            g_value_set_static_boxed(value, self->priv->video_codecs);
            break;
        case PROP_QXL:
            g_value_set_pointer(value, self->priv->qxl);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
display_channel_set_property(GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
    DisplayChannel *self = DISPLAY_CHANNEL(object);

    switch (property_id)
    {
        case PROP_N_SURFACES:
            self->priv->n_surfaces = MIN(g_value_get_uint(value), NUM_SURFACES);
            break;
        case PROP_VIDEO_CODECS:
            display_channel_set_video_codecs(self, g_value_get_boxed(value));
            break;
        case PROP_QXL:
            self->priv->qxl = g_value_get_pointer(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
display_channel_finalize(GObject *object)
{
    DisplayChannel *self = DISPLAY_CHANNEL(object);

    display_channel_destroy_surfaces(self);
    image_cache_reset(&self->priv->image_cache);

    if (spice_extra_checks) {
        unsigned int count;
        _Drawable *drawable;
        VideoStream *stream;

        count = 0;
        for (drawable = self->priv->free_drawables; drawable; drawable = drawable->u.next) {
            ++count;
        }
        spice_assert(count == NUM_DRAWABLES);

        count = 0;
        for (stream = self->priv->free_streams; stream; stream = stream->next) {
            ++count;
        }
        spice_assert(count == NUM_STREAMS);
        spice_assert(ring_is_empty(&self->priv->streams));

        for (count = 0; count < NUM_SURFACES; ++count) {
            spice_assert(self->priv->surfaces[count].context.canvas == NULL);
        }
    }

    monitors_config_unref(self->priv->monitors_config);
    g_array_unref(self->priv->video_codecs);
    g_free(self->priv);

    G_OBJECT_CLASS(display_channel_parent_class)->finalize(object);
}

static void drawable_draw(DisplayChannel *display, Drawable *drawable);
static Drawable *display_channel_drawable_try_new(DisplayChannel *display,
                                                  uint32_t process_commands_generation);

uint32_t display_channel_generate_uid(DisplayChannel *display)
{
    spice_return_val_if_fail(display != NULL, 0);

    return ++display->priv->bits_unique;
}

#define stat_start(stat, var)                                        \
    stat_start_time_t var; stat_start_time_init(&var, stat);

void display_channel_compress_stats_reset(DisplayChannel *display)
{
    spice_return_if_fail(display);

    image_encoder_shared_stat_reset(&display->priv->encoder_shared_data);
}

void display_channel_compress_stats_print(DisplayChannel *display_channel)
{
#ifdef COMPRESS_STAT
    uint32_t id;

    spice_return_if_fail(display_channel);

    g_object_get(display_channel, "id", &id, NULL);

    spice_info("==> Compression stats for display %u", id);
    image_encoder_shared_stat_print(&display_channel->priv->encoder_shared_data);
#endif
}

MonitorsConfig* monitors_config_ref(MonitorsConfig *monitors_config)
{
    monitors_config->refs++;

    return monitors_config;
}

void monitors_config_unref(MonitorsConfig *monitors_config)
{
    if (!monitors_config) {
        return;
    }
    if (--monitors_config->refs != 0) {
        return;
    }

    spice_debug("freeing monitors config");
    g_free(monitors_config);
}

static void monitors_config_debug(MonitorsConfig *mc)
{
    int i;

    spice_debug("monitors config count:%d max:%d", mc->count, mc->max_allowed);
    for (i = 0; i < mc->count; i++)
        spice_debug("+%d+%d %dx%d",
                    mc->heads[i].x, mc->heads[i].y,
                    mc->heads[i].width, mc->heads[i].height);
}

static MonitorsConfig* monitors_config_new(QXLHead *heads, ssize_t nheads, ssize_t max)
{
    MonitorsConfig *mc;

    mc = g_malloc(sizeof(MonitorsConfig) + nheads * sizeof(QXLHead));
    mc->refs = 1;
    mc->count = nheads;
    mc->max_allowed = max;
    memcpy(mc->heads, heads, nheads * sizeof(QXLHead));
    monitors_config_debug(mc);

    return mc;
}

int display_channel_get_streams_timeout(DisplayChannel *display)
{
    int timeout = INT_MAX;
    Ring *ring = &display->priv->streams;
    RingItem *item = ring;

    red_time_t now = spice_get_monotonic_time_ns();
    while ((item = ring_next(ring, item))) {
        VideoStream *stream;

        stream = SPICE_CONTAINEROF(item, VideoStream, link);
        if (stream == display->priv->forced_stream) {
            continue;
        }
        red_time_t delta = (stream->last_time + RED_STREAM_TIMEOUT) - now;

        if (delta < NSEC_PER_MILLISEC) {
            return 0;
        }
        timeout = MIN(timeout, (unsigned int)(delta / NSEC_PER_MILLISEC));
    }
    return timeout;
}

void display_channel_set_stream_video(DisplayChannel *display, int stream_video)
{
    spice_return_if_fail(display);
    spice_return_if_fail(stream_video != SPICE_STREAM_VIDEO_INVALID);

    switch (stream_video) {
    case SPICE_STREAM_VIDEO_ALL:
        spice_debug("sv all");
        break;
    case SPICE_STREAM_VIDEO_FILTER:
        spice_debug("sv filter");
        break;
    case SPICE_STREAM_VIDEO_OFF:
        spice_debug("sv off");
        break;
    default:
        spice_warn_if_reached();
        return;
    }

    display->priv->stream_video = stream_video;
}

void display_channel_set_video_codecs(DisplayChannel *display, GArray *video_codecs)
{
    spice_return_if_fail(display);

    g_clear_pointer(&display->priv->video_codecs, g_array_unref);
    display->priv->video_codecs = g_array_ref(video_codecs);
    g_object_notify(G_OBJECT(display), "video-codecs");
}

GArray *display_channel_get_video_codecs(DisplayChannel *display)
{
    spice_return_val_if_fail(display, NULL);

    return display->priv->video_codecs;
}

int display_channel_get_stream_video(DisplayChannel *display)
{
    return display->priv->stream_video;
}

static void stop_streams(DisplayChannel *display)
{
    Ring *ring = &display->priv->streams;
    RingItem *item = ring_get_head(ring);

    while (item) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        item = ring_next(ring, item);
        if (!stream->current) {
            video_stream_stop(display, stream);
        } else {
            spice_debug("attached stream");
        }
    }

    display->priv->next_item_trace = 0;
    memset(display->priv->items_trace, 0, sizeof(display->priv->items_trace));
}

void display_channel_surface_unref(DisplayChannel *display, uint32_t surface_id)
{
    RedSurface *surface = &display->priv->surfaces[surface_id];
    DisplayChannelClient *dcc;

    if (--surface->refs != 0) {
        return;
    }

    // only primary surface streams are supported
    if (is_primary_surface(display, surface_id)) {
        stop_streams(display);
    }
    spice_assert(surface->context.canvas);

    surface->context.canvas->ops->destroy(surface->context.canvas);
    if (surface->create_cmd != NULL) {
        red_surface_cmd_unref(surface->create_cmd);
        surface->create_cmd = NULL;
    }
    if (surface->destroy_cmd != NULL) {
        red_surface_cmd_unref(surface->destroy_cmd);
        surface->destroy_cmd = NULL;
    }

    region_destroy(&surface->draw_dirty_region);
    surface->context.canvas = NULL;
    FOREACH_DCC(display, dcc) {
        dcc_destroy_surface(dcc, surface_id);
    }

    spice_warn_if_fail(ring_is_empty(&surface->depend_on_me));
}

/* TODO: perhaps rename to "ready" or "realized" ? */
gboolean display_channel_surface_has_canvas(DisplayChannel *display,
                                            uint32_t surface_id)
{
    return display->priv->surfaces[surface_id].context.canvas != NULL;
}

static void streams_update_visible_region(DisplayChannel *display, Drawable *drawable)
{
    Ring *ring;
    RingItem *item;
    DisplayChannelClient *dcc;

    if (!red_channel_is_connected(RED_CHANNEL(display))) {
        return;
    }

    if (!is_primary_surface(display, drawable->surface_id)) {
        return;
    }

    ring = &display->priv->streams;
    item = ring_get_head(ring);

    while (item) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        VideoStreamAgent *agent;

        item = ring_next(ring, item);

        if (stream->current == drawable) {
            continue;
        }

        FOREACH_DCC(display, dcc) {
            int stream_id = display_channel_get_video_stream_id(display, stream);
            agent = dcc_get_video_stream_agent(dcc, stream_id);

            if (region_intersects(&agent->vis_region, &drawable->tree_item.base.rgn)) {
                region_exclude(&agent->vis_region, &drawable->tree_item.base.rgn);
                region_exclude(&agent->clip, &drawable->tree_item.base.rgn);
                dcc_video_stream_agent_clip(dcc, agent);
            }
        }
    }
}

static void pipes_add_drawable(DisplayChannel *display, Drawable *drawable)
{
    DisplayChannelClient *dcc;

    spice_warn_if_fail(drawable->pipes == NULL);
    FOREACH_DCC(display, dcc) {
        dcc_prepend_drawable(dcc, drawable);
    }
}

static void pipes_add_drawable_after(DisplayChannel *display,
                                     Drawable *drawable, Drawable *pos_after)
{
    RedDrawablePipeItem *dpi_pos_after;
    DisplayChannelClient *dcc;
    int num_other_linked = 0;
    GList *l;

    for (l = pos_after->pipes; l != NULL; l = l->next) {
        dpi_pos_after = l->data;

        num_other_linked++;
        dcc_add_drawable_after(dpi_pos_after->dcc, drawable, &dpi_pos_after->base);
    }

    if (num_other_linked == 0) {
        pipes_add_drawable(display, drawable);
        return;
    }
    if (num_other_linked != red_channel_get_n_clients(RED_CHANNEL(display))) {
        spice_debug("TODO: not O(n^2)");
        FOREACH_DCC(display, dcc) {
            int sent = 0;
            GList *l;
            for (l = pos_after->pipes; l != NULL; l = l->next) {
                dpi_pos_after = l->data;
                if (dpi_pos_after->dcc == dcc) {
                    sent = 1;
                    break;
                }
            }

            if (!sent) {
                dcc_prepend_drawable(dcc, drawable);
            }
        }
    }
}

static void current_add_drawable(DisplayChannel *display,
                                 Drawable *drawable, RingItem *pos)
{
    RedSurface *surface;
    uint32_t surface_id = drawable->surface_id;

    surface = &display->priv->surfaces[surface_id];
    ring_add_after(&drawable->tree_item.base.siblings_link, pos);
    ring_add(&display->priv->current_list, &drawable->list_link);
    ring_add(&surface->current_list, &drawable->surface_list_link);
    drawable->refs++;
}

/* Unrefs the drawable and removes it from any rings that it's in, as well as
 * removing any associated shadow item */
static void current_remove_drawable(DisplayChannel *display, Drawable *item)
{
    /* todo: move all to unref? */
    video_stream_trace_add_drawable(display, item);
    draw_item_remove_shadow(&item->tree_item);
    ring_remove(&item->tree_item.base.siblings_link);
    ring_remove(&item->list_link);
    ring_remove(&item->surface_list_link);
    drawable_unref(item);
}

static void drawable_remove_from_pipes(Drawable *drawable)
{
    RedDrawablePipeItem *dpi;

    GLIST_FOREACH(drawable->pipes, RedDrawablePipeItem, dpi) {
        RedChannelClient *rcc;

        rcc = RED_CHANNEL_CLIENT(dpi->dcc);
        red_channel_client_pipe_remove_and_release(rcc, &dpi->base);
    }
}

/* This function should never be called for Shadow TreeItems */
static void current_remove(DisplayChannel *display, TreeItem *item)
{
    TreeItem *now = item;

    /* depth-first tree traversal, TODO: do a to tree_foreach()? */
    for (;;) {
        Container *container_of_now = now->container;
        RingItem *ring_item;

        if (now->type == TREE_ITEM_TYPE_DRAWABLE) {
            Drawable *drawable = SPICE_CONTAINEROF(now, Drawable, tree_item.base);
            ring_item = now->siblings_link.prev;
            drawable_remove_from_pipes(drawable);
            current_remove_drawable(display, drawable);
        } else {
            Container *now_as_container = CONTAINER(now);

            spice_assert(now->type == TREE_ITEM_TYPE_CONTAINER);

            if ((ring_item = ring_get_head(&now_as_container->items))) {
                /* descend into the container's child ring and continue
                 * iterating and removing those children */
                now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
                continue;
            }
            /* This item is a container but it has no children, so reset our
             * iterator to the item's previous sibling and free this empty
             * container */
            ring_item = now->siblings_link.prev;
            container_free(now_as_container);
        }
        if (now == item) {
            /* This is true if the initial @item was a DRAWABLE, or if @item
             * was a container and we've finished iterating over all of that
             * container's children and returned back up to the parent and
             * freed it (see below) */
            return;
        }

        /* Increment the iterator to the next sibling. Note that if an item was
         * removed above, ring_item will have been reset to the item before the
         * item that was removed */
        if ((ring_item = ring_next(&container_of_now->items, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        } else {
            /* there is no next sibling, so move one level up the tree */
            now = &container_of_now->base;
        }
    }
}

static void current_remove_all(DisplayChannel *display, int surface_id)
{
    Ring *ring = &display->priv->surfaces[surface_id].current;
    RingItem *ring_item;

    while ((ring_item = ring_get_head(ring))) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        /* NOTE: current_remove() should never be called on Shadow type items
         * or we will hit an assert. Fortunately, the 'current' ring is ordered
         * in such a way that a DrawItem will always be placed before its
         * associated Shadow in the tree. Since removing a DrawItem will also
         * result in the associated Shadow item being removed from the tree,
         * this loop will never call current_remove() on a Shadow item unless
         * we change the order that items are inserted into the tree */
        current_remove(display, now);
    }
}

/* Replace an existing Drawable in the tree with a new drawable that is
 * equivalent. The new drawable is also added to the pipe.
 *
 * This function can fail if the items aren't actually equivalent (e.g. either
 * item has a shadow, they have different effects, etc)
 */
static bool current_add_equal(DisplayChannel *display, DrawItem *item, TreeItem *other)
{
    DrawItem *other_draw_item;
    Drawable *drawable;
    Drawable *other_drawable;

    if (other->type != TREE_ITEM_TYPE_DRAWABLE) {
        return FALSE;
    }
    other_draw_item = DRAW_ITEM(other);

    if (item->shadow || other_draw_item->shadow || item->effect != other_draw_item->effect) {
        return FALSE;
    }

    drawable = SPICE_CONTAINEROF(item, Drawable, tree_item);
    other_drawable = SPICE_CONTAINEROF(other_draw_item, Drawable, tree_item);

    if (item->effect == QXL_EFFECT_OPAQUE) {
        /* check whether the new item can safely replace the other drawable at
         * the same position in the pipe, or whether it should be added to the
         * end of the queue */
        int add_after = !!other_drawable->stream &&
                        is_drawable_independent_from_surfaces(drawable);
        video_stream_maintenance(display, drawable, other_drawable);
        current_add_drawable(display, drawable, &other->siblings_link);
        other_drawable->refs++;
        current_remove_drawable(display, other_drawable);
        if (add_after) {
            pipes_add_drawable_after(display, drawable, other_drawable);
        } else {
            pipes_add_drawable(display, drawable);
        }
        drawable_remove_from_pipes(other_drawable);
        drawable_unref(other_drawable);
        return TRUE;
    }

    switch (item->effect) {
    case QXL_EFFECT_REVERT_ON_DUP:
        if (is_same_drawable(drawable, other_drawable)) {

            DisplayChannelClient *dcc;
            GList *dpi_item;

            other_drawable->refs++;
            current_remove_drawable(display, other_drawable);

            /* sending the drawable to clients that already received
             * (or will receive) other_drawable */
            dpi_item = g_list_first(other_drawable->pipes);
            /* dpi contains a sublist of dcc's, ordered the same */
            FOREACH_DCC(display, dcc) {
                if (dpi_item && dcc == ((RedDrawablePipeItem *) dpi_item->data)->dcc) {
                    dpi_item = dpi_item->next;
                } else {
                    dcc_prepend_drawable(dcc, drawable);
                }
            }
            /* not sending other_drawable where possible */
            drawable_remove_from_pipes(other_drawable);

            drawable_unref(other_drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_OPAQUE_BRUSH:
        if (is_same_geometry(drawable, other_drawable)) {
            current_add_drawable(display, drawable, &other->siblings_link);
            drawable_remove_from_pipes(other_drawable);
            current_remove_drawable(display, other_drawable);
            pipes_add_drawable(display, drawable);
            return TRUE;
        }
        break;
    case QXL_EFFECT_NOP_ON_DUP:
        if (is_same_drawable(drawable, other_drawable)) {
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* This function excludes the given region from a single TreeItem. Both @rgn
 * and @item may be modified.
 *
 * If there is overlap between @rgn and the @item region, remove the
 * overlapping intersection from both @rgn and the item's region (NOTE: it's
 * not clear to me why this is done - jjongsma)
 *
 * However, if the item is a DrawItem that has a shadow, we add an additional
 * region to @rgn: the intersection of the shadow item's region with @rgn when
 * @rgn is shifted over by the delta between the DrawItem and the Shadow.
 * [WORKING THEORY: since the destination region for a COPY_BITS operation was
 * excluded, we no longer need the source region that corresponds with that
 * copy operation, so we can also exclude any drawables that affect that
 * region. Not sure if that actually makes sense... ]
 *
 * If the item is a Shadow, we store the intersection between @rgn and the
 * Shadow's region in Shadow::on_hold and remove that region from @rgn. This is
 * done since a Shadow represents the source region for a COPY_BITS operation,
 * and we need to make sure that this source region stays up-to-date until the
 * copy operation is executed.
 *
 * Consider the following case:
 *  1) the surface is fully black at the beginning
 *  2) we add a new item to the tree which paints region A white
 *  3) we add a new item to the tree which copies region A to region B
 *  4) we add another new item to the tree painting region A blue.
 *
 * After all operations are completed, region A should be blue, and region B
 * should be white. If there were no copy operation (step 3), we could simply
 * eliminate step 2 when we add item 4 to the tree, since step 4 overwrites the
 * same region with a different color. However, if we did eliminate step 2,
 * region B would be black after all operations were completed. So these
 * regions that would normally be excluded are put "on hold" if they are part
 * of a source region for a copy operation.
 *
 * @display: the display channel
 * @ring: a fallback toplevel ring???
 * @item: the tree item to exclude from @rgn
 * @rgn: the region to exclude
 * @top_ring: ???
 * @frame_candidate: ???
 */
static void __exclude_region(DisplayChannel *display, Ring *ring, TreeItem *item, QRegion *rgn,
                             Ring **top_ring, Drawable *frame_candidate)
{
    QRegion and_rgn;
    stat_start(&display->priv->__exclude_stat, start_time);

    region_clone(&and_rgn, rgn);
    /* find intersection of the @rgn argument with the region of the @item arg */
    region_and(&and_rgn, &item->rgn);
    if (!region_is_empty(&and_rgn)) {
        if (IS_DRAW_ITEM(item)) {
            DrawItem *draw = DRAW_ITEM(item);

            if (draw->effect == QXL_EFFECT_OPAQUE) {
                /* remove the intersection from the original @rgn */
                region_exclude(rgn, &and_rgn);
            }

            if (draw->shadow) {
                /* @draw represents the destination of a COPY_BITS operation.
                 * @shadow represents the source item for the copy operation */
                Shadow *shadow;
                int32_t x = item->rgn.extents.x1;
                int32_t y = item->rgn.extents.y1;

                /* remove the intersection from the item's region */
                region_exclude(&draw->base.rgn, &and_rgn);
                shadow = draw->shadow;
                /* shift the intersected region by the difference between the
                 * source and destination regions */
                region_offset(&and_rgn, shadow->base.rgn.extents.x1 - x,
                              shadow->base.rgn.extents.y1 - y);
                /* remove the shifted intersection region from the source
                 * (shadow) item's region. If the destination is excluded, we
                 * can also exclude the corresponding area from the source */
                region_exclude(&shadow->base.rgn, &and_rgn);
                /* find the intersection between the shifted intersection
                 * region and the Shadow's 'on_hold' region. This represents
                 * the portion of the Shadow's region that we just removed that
                 * is currently stored in on_hold. */
                region_and(&and_rgn, &shadow->on_hold);
                if (!region_is_empty(&and_rgn)) {
                    /* Since we removed a portion of the Shadow's region, we
                     * can also remove that portion from on_hold */
                    region_exclude(&shadow->on_hold, &and_rgn);
                    /* Since this region is no longer "on hold", add it back to
                     * the @rgn argument */
                    region_or(rgn, &and_rgn);
                    // in flat representation of current, shadow is always his owner next
                    if (!tree_item_contained_by(&shadow->base, *top_ring)) {
                        *top_ring = tree_item_container_items(&shadow->base, ring);
                    }
                }
            } else {
                /* TODO: document the purpose of this code */
                if (frame_candidate) {
                    Drawable *drawable = SPICE_CONTAINEROF(draw, Drawable, tree_item);
                    video_stream_maintenance(display, frame_candidate, drawable);
                }
                /* Remove the intersection from the DrawItem's region */
                region_exclude(&draw->base.rgn, &and_rgn);
            }
        } else if (item->type == TREE_ITEM_TYPE_CONTAINER) {
            /* excludes the intersection between 'rgn' and item->rgn from the
             * item's region */
            region_exclude(&item->rgn, &and_rgn);

            if (region_is_empty(&item->rgn)) {  //assume container removal will follow
                Shadow *shadow;

                /* exclude the intersection from the 'rgn' argument as well,
                 * but only if the item is now empty.
                 * TODO: explain why this is necessary */
                region_exclude(rgn, &and_rgn);
                if ((shadow = tree_item_find_shadow(item))) {
                    /* add the shadow's on_hold region back to the 'rgn' argument */
                    region_or(rgn, &shadow->on_hold);
                    if (!tree_item_contained_by(&shadow->base, *top_ring)) {
                        /* TODO: document why top_ring is set here */
                        *top_ring = tree_item_container_items(&shadow->base, ring);
                    }
                }
            }
        } else {
            Shadow *shadow;

            spice_assert(item->type == TREE_ITEM_TYPE_SHADOW);
            shadow = SHADOW(item);
            /* Since a Shadow represents the source region for a COPY_BITS
             * operation, we need to make sure that we don't remove existing
             * drawables that draw to this source region. If we did, it would
             * affect the copy operation. So we remove the intersection between
             * @rgn and item->rgn from the @rgn argument to avoid excluding
             * these drawables */
            region_exclude(rgn, &and_rgn);
            /* adds this intersection to on_hold */
            region_or(&shadow->on_hold, &and_rgn);
        }
    }
    /* clean up memory */
    region_destroy(&and_rgn);
    stat_add(&display->priv->__exclude_stat, start_time);
}

/* This function iterates through the given @ring starting at @ring_item and
 * continuing until reaching @last. and calls __exclude_region() on each item.
 * Any items that have an empty region as a result of the __exclude_region()
 * call are removed from the tree.
 *
 * TODO: What is the intended use of this function?
 *
 * @ring: every time this function is called, @ring is a Surface's 'current'
 *      ring, or to the ring of children of a container within that ring.
 * @ring_item: callers usually call this argument 'exclude_base'. We will
 *      iterate through the tree starting at this item
 * @rgn: callers usually call this 'exclude_rgn' -- it appears to be the region
 *      we want to exclude from existing items in the tree. It is an in/out
 *      parameter and it may be modified as the result of calling this function
 * @last: We will stop iterating at this item, and the function will return the
 *      next item after iteration is complete (which may be different than the
 *      passed value if that item was removed from the tree
 * @frame_candidate: usually callers pass NULL, sometimes it's the drawable
 *      that's being added to the 'current' ring. TODO: What is its purpose?
 */
static void exclude_region(DisplayChannel *display, Ring *ring, RingItem *ring_item,
                           QRegion *rgn, TreeItem **last, Drawable *frame_candidate)
{
    Ring *top_ring;
    stat_start(&display->priv->exclude_stat, start_time);

    if (!ring_item) {
        return;
    }

    top_ring = ring;

    for (;;) {
        TreeItem *now = SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link);
        Container *container = now->container;

        spice_assert(!region_is_empty(&now->rgn));

        /* check whether the ring_item item intersects the passed-in region */
        if (region_intersects(rgn, &now->rgn)) {
            /* remove the overlapping portions of region and now->rgn, among
             * other things. See documentation for __exclude_region() */
            __exclude_region(display, ring, now, rgn, &top_ring, frame_candidate);

            if (region_is_empty(&now->rgn)) {
                /* __exclude_region() does not remove the region of shadow-type
                 * items */
                spice_assert(now->type != TREE_ITEM_TYPE_SHADOW);
                ring_item = now->siblings_link.prev;
                /* if __exclude_region() removed the entire region for this
                 * sibling item, remove it from the 'current' tree */
                current_remove(display, now);
                if (last && *last == now) {
                    /* the caller wanted to stop at this item, but this item
                     * has been removed, so we set @last to the next item */
                    SPICE_VERIFY(SPICE_OFFSETOF(TreeItem, siblings_link) == 0);
                    *last = (TreeItem *)ring_next(ring, ring_item);
                }
            } else if (now->type == TREE_ITEM_TYPE_CONTAINER) {
                /* if this sibling is a container type, descend into the
                 * container's child ring and continue iterating */
                Container *container = CONTAINER(now);
                if ((ring_item = ring_get_head(&container->items))) {
                    ring = &container->items;
                    spice_assert(SPICE_CONTAINEROF(ring_item, TreeItem, siblings_link)->container);
                    continue;
                }
                /* container had no children, so reset ring_item to the
                 * container itself */
                ring_item = &now->siblings_link;
            }

            if (region_is_empty(rgn)) {
                /* __exclude_region() removed the entire region from 'rgn', so
                 * no need to continue checking further items in the tree */
                stat_add(&display->priv->exclude_stat, start_time);
                return;
            }
        }

        SPICE_VERIFY(SPICE_OFFSETOF(TreeItem, siblings_link) == 0);
        /* if this is the last item to check, or if the current ring is
         * completed, don't go any further */
        while ((last && *last == (TreeItem *)ring_item) ||
               !(ring_item = ring_next(ring, ring_item))) {
            /* we're currently iterating the top ring, so we're done */
            if (ring == top_ring) {
                stat_add(&display->priv->exclude_stat, start_time);
                return;
            }
            /* we're iterating through a container child ring, so climb one
             * level up the heirarchy and continue iterating that ring */
            ring_item = &container->base.siblings_link;
            container = container->base.container;
            ring = (container) ? &container->items : top_ring;
        }
    }
}

/* Add a drawable @item (with a shadow) to the current ring.  The return value
 * indicates whether the new item should be added to the pipe */
static bool current_add_with_shadow(DisplayChannel *display, Ring *ring, Drawable *item)
{
    stat_start(&display->priv->add_stat, start_time);
#ifdef RED_WORKER_STAT
    ++display->priv->add_with_shadow_count;
#endif

    RedDrawable *red_drawable = item->red_drawable;
    SpicePoint delta = {
        .x = red_drawable->u.copy_bits.src_pos.x - red_drawable->bbox.left,
        .y = red_drawable->u.copy_bits.src_pos.y - red_drawable->bbox.top
    };

    Shadow *shadow = shadow_new(&item->tree_item, &delta);
    if (!shadow) {
        stat_add(&display->priv->add_stat, start_time);
        return FALSE;
    }
    // item and his shadow must initially be placed in the same container.
    // for now putting them on root.

    // only primary surface streams are supported
    if (is_primary_surface(display, item->surface_id)) {
        video_stream_detach_behind(display, &shadow->base.rgn, NULL);
    }

    /* Prepend the shadow to the beginning of the current ring */
    ring_add(ring, &shadow->base.siblings_link);
    /* Prepend the draw item to the beginning of the current ring. NOTE: this
     * means that the drawable is placed *before* its associated shadow in the
     * tree. Changing this order will violate several unstated assumptions */
    current_add_drawable(display, item, ring);
    if (item->tree_item.effect == QXL_EFFECT_OPAQUE) {
        QRegion exclude_rgn;
        region_clone(&exclude_rgn, &item->tree_item.base.rgn);
        /* Since the new drawable is opaque, remove overlapped regions from all
         * items already in the tree.  Start iterating through the tree
         * starting with the shadow item to avoid excluding the new item
         * itself */
        exclude_region(display, ring, &shadow->base.siblings_link, &exclude_rgn, NULL, NULL);
        region_destroy(&exclude_rgn);
        streams_update_visible_region(display, item);
    } else {
        if (is_primary_surface(display, item->surface_id)) {
            video_stream_detach_behind(display, &item->tree_item.base.rgn, item);
        }
    }
    stat_add(&display->priv->add_stat, start_time);
    return TRUE;
}

/* Add a @drawable (without a shadow) to the current ring.
 * The return value indicates whether the new item should be added to the pipe */
static bool current_add(DisplayChannel *display, Ring *ring, Drawable *drawable)
{
    DrawItem *item = &drawable->tree_item;
    RingItem *now;
    QRegion exclude_rgn;
    RingItem *exclude_base = NULL;
    stat_start(&display->priv->add_stat, start_time);

    spice_assert(!region_is_empty(&item->base.rgn));
    region_init(&exclude_rgn);
    now = ring_next(ring, ring);

    /* check whether the new drawable region intersects any of the items
     * already in the 'current' ring */
    while (now) {
        TreeItem *sibling = SPICE_CONTAINEROF(now, TreeItem, siblings_link);
        int test_res;

        if (!region_bounds_intersects(&item->base.rgn, &sibling->rgn)) {
            /* the bounds of the two items are totally disjoint, so no need to
             * check further. check the next item */
            now = ring_next(ring, now);
            continue;
        }
        /* bounds overlap, but check whether the regions actually overlap */
        test_res = region_test(&item->base.rgn, &sibling->rgn, REGION_TEST_ALL);
        if (!(test_res & REGION_TEST_SHARED)) {
            /* there's no overlap of the regions between these two items. Move
             * on to the next one. */
            now = ring_next(ring, now);
            continue;
        } else if (sibling->type != TREE_ITEM_TYPE_SHADOW) {
            /* there is an overlap between the two regions */
            /* NOTE: Shadow types represent a source region for a COPY_BITS
             * operation, they don't represent a region that will be drawn.
             * Therefore, we don't check for overlap between the new
             * DrawItem and any shadow items */
            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) &&
                                                   !(test_res & REGION_TEST_LEFT_EXCLUSIVE) &&
                                                   current_add_equal(display, item, sibling)) {
                /* the regions were equivalent, so we just replaced the other
                 * drawable with the new one */
                stat_add(&display->priv->add_stat, start_time);
                /* Caller doesn't need to add the new drawable to the pipe,
                 * since current_add_equal already added it to the pipe */
                return FALSE;
            }

            if (!(test_res & REGION_TEST_RIGHT_EXCLUSIVE) && item->effect == QXL_EFFECT_OPAQUE) {
                /* the new drawable is opaque and entirely contains the sibling item */
                Shadow *shadow;
                int skip = now == exclude_base;

                if ((shadow = tree_item_find_shadow(sibling))) {
                    /* The sibling item was the destination of a COPY_BITS operation */
                    if (exclude_base) {
                        /* During a previous iteration through this loop, an
                         * obscured sibling item was removed from the tree, and
                         * exclude_base was set to the item immediately after
                         * the removed item (see below). This time through the
                         * loop, we encountered another sibling that was
                         * completely obscured, so we call exclude_region()
                         * using the previously saved item as our starting
                         * point. @exlude_rgn will be the union of any previous
                         * 'on_hold' regions from the shadows of previous
                         * iterations
                         *
                         * TODO: it's unclear to me why we only only call
                         * exclude_region() for the previous item if the next
                         * item is obscured and has a shadow. -jjongsma
                         */
                        TreeItem *next = sibling;
                        exclude_region(display, ring, exclude_base, &exclude_rgn, &next, NULL);
                        if (next != sibling) {
                            /* the @next param is only changed if the given item
                             * was removed as a side-effect of calling
                             * exclude_region(), so update our loop variable */
                            now = next ? &next->siblings_link : NULL;
                            exclude_base = NULL;
                            continue;
                        }
                    }
                    /* Since the destination item (sibling) of the COPY_BITS
                     * operation is fully obscured, we no longer need the
                     * source item (shadow) anymore. shadow->on_hold represents
                     * a region that would normally have been excluded by a
                     * previous call to __exclude_region() (see documentation
                     * for that function), but was put on hold to make sure we
                     * kept the source region up to date. Now that we no longer
                     * need this source region, this "on hold" region can be
                     * safely excluded again. */
                    region_or(&exclude_rgn, &shadow->on_hold);
                }
                now = now->prev;
                /* remove the obscured sibling from the 'current' tree, which
                 * will also remove its shadow (if any) */
                current_remove(display, sibling);
                /* advance the loop variable */
                now = ring_next(ring, now);
                if (shadow || skip) {
                    /* 'now' is currently set to the item immediately AFTER
                     * the obscured sibling that we just removed.
                     * TODO: document why this item is used as an
                     * 'exclude_base' */
                    exclude_base = now;
                }
                continue;
            }

            if (!(test_res & REGION_TEST_LEFT_EXCLUSIVE) && is_opaque_item(sibling)) {
                /* the sibling item is opaque and entirely contains the new drawable */
                Container *container;

                /* The first time through, @exclude_base will be NULL, but
                 * subsequent loops may set it to something.  In addition,
                 * @exclude_rgn starts out empty, but previous iterations of
                 * this loop may have added various Shadow::on_hold regions to
                 * it. */
                if (exclude_base) {
                    exclude_region(display, ring, exclude_base, &exclude_rgn, NULL, NULL);
                    region_clear(&exclude_rgn);
                    exclude_base = NULL;
                }
                if (sibling->type == TREE_ITEM_TYPE_CONTAINER) {
                    container = CONTAINER(sibling);
                    /* NOTE: here, ring is reset to the ring of the container's children */
                    ring = &container->items;
                    /* if the sibling item is a container, place the new
                     * drawable into that container */
                    item->base.container = container;
                    /* Start iterating over the container's children to see if
                     * any of them intersect this new drawable */
                    now = ring_next(ring, ring);
                    continue;
                }
                spice_assert(IS_DRAW_ITEM(sibling));
                if (!DRAW_ITEM(sibling)->container_root) {
                    /* Create a new container to hold the sibling and the new
                     * drawable */
                    container = container_new(DRAW_ITEM(sibling));
                    if (!container) {
                        spice_warning("create new container failed");
                        region_destroy(&exclude_rgn);
                        return FALSE;
                    }
                    item->base.container = container;
                    /* reset 'ring' to the container's children ring, so that
                     * we can add the new drawable to this ring below */
                    ring = &container->items;
                }
            }
        }
        /* If we've gotten here, that means that:
         *  - the new item is not opaque
         *  - We just created a container to hold the new drawable and the
         *    sibling that encloses it
         *  - ??? */
        if (!exclude_base) {
            exclude_base = now;
        }
        break;
    }
    /* we've removed any obscured siblings and figured out which ring the new
     * drawable needs to be added to, so let's add it. */
    if (item->effect == QXL_EFFECT_OPAQUE) {
        /* @exclude_rgn may contain the union of on_hold regions from any
         * Shadows that were associated with DrawItems that were removed from
         * the tree.  Add the new item's region to that */
        region_or(&exclude_rgn, &item->base.rgn);
        exclude_region(display, ring, exclude_base, &exclude_rgn, NULL, drawable);
        video_stream_trace_update(display, drawable);
        if (drawable_is_drm(drawable) && !drawable->stream) {
            fprintf(stderr, "%s:%d drm not in stream!\n", __FILE__, __LINE__);
        }
        streams_update_visible_region(display, drawable);
        /*
         * Performing the insertion after exclude_region for
         * safety (todo: Not sure if exclude_region can affect the drawable
         * if it is added to the tree before calling exclude_region).
         */
        current_add_drawable(display, drawable, ring);
    } else {
        /*
         * video_stream_detach_behind can affect the current tree since
         * it may trigger calls to display_channel_draw. Thus, the
         * drawable should be added to the tree before calling
         * video_stream_detach_behind
         */
        current_add_drawable(display, drawable, ring);
        if (is_primary_surface(display, drawable->surface_id)) {
            video_stream_detach_behind(display, &drawable->tree_item.base.rgn, drawable);
        }
    }
    region_destroy(&exclude_rgn);
    stat_add(&display->priv->add_stat, start_time);
    return TRUE;
}

static bool drawable_can_stream(DisplayChannel *display, Drawable *drawable)
{
    RedDrawable *red_drawable = drawable->red_drawable;
    SpiceImage *image;

    if (display->priv->stream_video == SPICE_STREAM_VIDEO_OFF) {
        return FALSE;
    }

    if (!is_primary_surface(display, drawable->surface_id)) {
        return FALSE;
    }

    if (drawable->tree_item.effect != QXL_EFFECT_OPAQUE ||
        red_drawable->type != QXL_DRAW_COPY ||
        red_drawable->u.copy.rop_descriptor != SPICE_ROPD_OP_PUT) {
        return FALSE;
    }

    image = red_drawable->u.copy.src_bitmap;
    if (image == NULL ||
        (image->descriptor.type != SPICE_IMAGE_TYPE_BITMAP &&
         image->descriptor.type != SPICE_IMAGE_TYPE_DRM_PRIME)) {
        return FALSE;
    }

    /* force streaming for DRM primes */
    if (image->descriptor.type == SPICE_IMAGE_TYPE_DRM_PRIME) {
        SpiceRect* rect = &drawable->red_drawable->u.copy.src_area;
        VideoStream *stream = display->priv->forced_stream;

        if (stream && (!ring_item_is_linked(&stream->link) ||
            rect->right - rect->left != stream->width ||
            rect->bottom - rect->top != stream->height ||
            spice_image_get_top_down(image) != stream->top_down)) {
            video_stream_unref(display, stream);
            display->priv->forced_stream = stream = NULL;
        }
        if (!stream) {
            display->priv->forced_stream = display_channel_create_video_stream(display, drawable);
            if (display->priv->forced_stream) {
                display->priv->forced_stream->refs++;
            }
        } else {
            /* force stream to stay alive */
            stream->last_time = drawable->creation_time;
        }
        return TRUE;
    }

    if (display->priv->stream_video == SPICE_STREAM_VIDEO_FILTER) {
        SpiceRect* rect;
        int size;

        rect = &drawable->red_drawable->u.copy.src_area;
        size = (rect->right - rect->left) * (rect->bottom - rect->top);
        if (size < RED_STREAM_MIN_SIZE) {
            return FALSE;
        }
    }

    return TRUE;
}

#ifdef RED_WORKER_STAT
static void display_channel_print_stats(DisplayChannel *display)
{
    stat_time_t total = display->priv->add_stat.total;
    spice_debug("add with shadow count %u",
               display->priv->add_with_shadow_count);
    display->priv->add_with_shadow_count = 0;
    spice_debug("add[%u] %f exclude[%u] %f __exclude[%u] %f",
               display->priv->add_stat.count,
               stat_cpu_time_to_sec(total),
               display->priv->exclude_stat.count,
               stat_cpu_time_to_sec(display->priv->exclude_stat.total),
               display->priv->__exclude_stat.count,
               stat_cpu_time_to_sec(display->priv->__exclude_stat.total));
    spice_debug("add %f%% exclude %f%% exclude2 %f%% __exclude %f%%",
               (double)(total - display->priv->exclude_stat.total) / total * 100,
               (double)(display->priv->exclude_stat.total) / total * 100,
               (double)(display->priv->exclude_stat.total -
                        display->priv->__exclude_stat.total) / display->priv->exclude_stat.total * 100,
               (double)(display->priv->__exclude_stat.total) / display->priv->exclude_stat.total * 100);
    stat_reset(&display->priv->add_stat);
    stat_reset(&display->priv->exclude_stat);
    stat_reset(&display->priv->__exclude_stat);
}
#endif

static void drawable_ref_surface_deps(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;
    RedSurface *surface;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id == -1) {
            continue;
        }
        surface = &display->priv->surfaces[surface_id];
        surface->refs++;
    }
}

static void surface_read_bits(DisplayChannel *display, int surface_id,
                              const SpiceRect *area, uint8_t *dest, int dest_stride)
{
    SpiceCanvas *canvas;
    RedSurface *surface = &display->priv->surfaces[surface_id];

    canvas = surface->context.canvas;
    canvas->ops->read_bits(canvas, dest, dest_stride, area);
}

static void handle_self_bitmap(DisplayChannel *display, Drawable *drawable)
{
    RedDrawable *red_drawable = drawable->red_drawable;
    SpiceImage *image;
    int32_t width;
    int32_t height;
    uint8_t *dest;
    int dest_stride;
    RedSurface *surface;
    int bpp;
    int all_set;

    surface = &display->priv->surfaces[drawable->surface_id];

    bpp = SPICE_SURFACE_FMT_DEPTH(surface->context.format) / 8;
    width = red_drawable->self_bitmap_area.right - red_drawable->self_bitmap_area.left;
    height = red_drawable->self_bitmap_area.bottom - red_drawable->self_bitmap_area.top;
    dest_stride = SPICE_ALIGN(width * bpp, 4);

    image = g_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.flags = 0;

    QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_RED, display_channel_generate_uid(display));
    image->u.bitmap.flags = surface->context.top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    image->u.bitmap.format = spice_bitmap_from_surface_type(surface->context.format);
    image->u.bitmap.stride = dest_stride;
    image->descriptor.width = image->u.bitmap.x = width;
    image->descriptor.height = image->u.bitmap.y = height;
    image->u.bitmap.palette = NULL;

    dest = (uint8_t *)spice_malloc_n(height, dest_stride);
    image->u.bitmap.data = spice_chunks_new_linear(dest, height * dest_stride);
    image->u.bitmap.data->flags |= SPICE_CHUNKS_FLAGS_FREE;

    display_channel_draw(display, &red_drawable->self_bitmap_area, drawable->surface_id);
    surface_read_bits(display, drawable->surface_id,
        &red_drawable->self_bitmap_area, dest, dest_stride);

    /* For 32bit non-primary surfaces we need to keep any non-zero
       high bytes as the surface may be used as source to an alpha_blend */
    if (!is_primary_surface(display, drawable->surface_id) &&
        image->u.bitmap.format == SPICE_BITMAP_FMT_32BIT &&
        rgb32_data_has_alpha(width, height, dest_stride, dest, &all_set)) {
        if (all_set) {
            image->descriptor.flags |= SPICE_IMAGE_FLAGS_HIGH_BITS_SET;
        } else {
            image->u.bitmap.format = SPICE_BITMAP_FMT_RGBA;
        }
    }

    red_drawable->self_bitmap_image = image;
}

static void surface_add_reverse_dependency(DisplayChannel *display, int surface_id,
                                             DependItem *depend_item, Drawable *drawable)
{
    RedSurface *surface;

    if (surface_id == -1) {
        depend_item->drawable = NULL;
        return;
    }

    surface = &display->priv->surfaces[surface_id];

    depend_item->drawable = drawable;
    ring_add(&surface->depend_on_me, &depend_item->ring_item);
}

static bool handle_surface_deps(DisplayChannel *display, Drawable *drawable)
{
    int x;

    for (x = 0; x < 3; ++x) {
        // surface self dependency is handled by shadows in "current", or by
        // handle_self_bitmap
        if (drawable->surface_deps[x] != drawable->surface_id) {
            surface_add_reverse_dependency(display, drawable->surface_deps[x],
                                      &drawable->depend_items[x], drawable);

            if (drawable->surface_deps[x] == 0) {
                QRegion depend_region;
                region_init(&depend_region);
                region_add(&depend_region, &drawable->red_drawable->surfaces_rects[x]);
                video_stream_detach_behind(display, &depend_region, NULL);
            }
        }
    }

    return TRUE;
}

static void draw_depend_on_me(DisplayChannel *display, uint32_t surface_id)
{
    RedSurface *surface;
    RingItem *ring_item;

    surface = &display->priv->surfaces[surface_id];

    while ((ring_item = ring_get_tail(&surface->depend_on_me))) {
        Drawable *drawable;
        DependItem *depended_item = SPICE_CONTAINEROF(ring_item, DependItem, ring_item);
        drawable = depended_item->drawable;
        display_channel_draw(display, &drawable->red_drawable->bbox, drawable->surface_id);
    }
}

static bool validate_drawable_bbox(DisplayChannel *display, RedDrawable *drawable)
{
        DrawContext *context;
        uint32_t surface_id = drawable->surface_id;

        /* surface_id must be validated before calling into
         * validate_drawable_bbox
         */
        if (!display_channel_validate_surface(display, drawable->surface_id)) {
            return FALSE;
        }
        context = &display->priv->surfaces[surface_id].context;

        if (drawable->bbox.top < 0)
                return FALSE;
        if (drawable->bbox.left < 0)
                return FALSE;
        if (drawable->bbox.bottom < 0)
                return FALSE;
        if (drawable->bbox.right < 0)
                return FALSE;
        if (drawable->bbox.bottom > context->height)
                return FALSE;
        if (drawable->bbox.right > context->width)
                return FALSE;

        return TRUE;
}

/**
 * @brief Get a new Drawable
 *
 * The Drawable returned is fully initialized.
 *
 * @return initialized Drawable or NULL on failure
 */
static Drawable *display_channel_get_drawable(DisplayChannel *display, uint8_t effect,
                                              RedDrawable *red_drawable,
                                              uint32_t process_commands_generation)
{
    Drawable *drawable;
    int x;

    /* Validate all surface ids before updating counters
     * to avoid invalid updates if we find an invalid id.
     */
    if (!validate_drawable_bbox(display, red_drawable)) {
        return NULL;
    }
    for (x = 0; x < 3; ++x) {
        if (red_drawable->surface_deps[x] != -1
            && !display_channel_validate_surface(display, red_drawable->surface_deps[x])) {
            return NULL;
        }
    }

    drawable = display_channel_drawable_try_new(display, process_commands_generation);
    if (!drawable) {
        return NULL;
    }

    drawable->tree_item.effect = effect;
    drawable->red_drawable = red_drawable_ref(red_drawable);

    drawable->surface_id = red_drawable->surface_id;
    display->priv->surfaces[drawable->surface_id].refs++;

    memcpy(drawable->surface_deps, red_drawable->surface_deps, sizeof(drawable->surface_deps));
    /*
        surface->refs is affected by a drawable (that is
        dependent on the surface) as long as the drawable is alive.
        However, surface->depend_on_me is affected by a drawable only
        as long as it is in the current tree (hasn't been rendered yet).
    */
    drawable_ref_surface_deps(display, drawable);

    return drawable;
}

/**
 * Add a Drawable to the items to draw.
 * On failure the Drawable is not added.
 */
static void display_channel_add_drawable(DisplayChannel *display, Drawable *drawable)
{
    int surface_id = drawable->surface_id;
    RedDrawable *red_drawable = drawable->red_drawable;

    red_drawable->mm_time = reds_get_mm_time();

    region_add(&drawable->tree_item.base.rgn, &red_drawable->bbox);

    if (red_drawable->clip.type == SPICE_CLIP_TYPE_RECTS) {
        QRegion rgn;

        region_init(&rgn);
        region_add_clip_rects(&rgn, red_drawable->clip.rects);
        region_and(&drawable->tree_item.base.rgn, &rgn);
        region_destroy(&rgn);
    }

    if (region_is_empty(&drawable->tree_item.base.rgn)) {
        return;
    }

    if (red_drawable->self_bitmap) {
        handle_self_bitmap(display, drawable);
    }

    draw_depend_on_me(display, surface_id);

    if (!handle_surface_deps(display, drawable)) {
        return;
    }

    Ring *ring = &display->priv->surfaces[surface_id].current;
    int add_to_pipe;
    if (has_shadow(red_drawable)) {
        add_to_pipe = current_add_with_shadow(display, ring, drawable);
    } else {
        drawable->streamable = drawable_can_stream(display, drawable);
        add_to_pipe = current_add(display, ring, drawable);
    }

    if (add_to_pipe)
        pipes_add_drawable(display, drawable);

#ifdef RED_WORKER_STAT
    if ((++display->priv->add_count % 100) == 0)
        display_channel_print_stats(display);
#endif
}

void display_channel_process_draw(DisplayChannel *display, RedDrawable *red_drawable,
                                  uint32_t process_commands_generation)
{
    Drawable *drawable =
        display_channel_get_drawable(display, red_drawable->effect, red_drawable,
                                     process_commands_generation);

    if (!drawable) {
        return;
    }

    display_channel_add_drawable(display, drawable);

    drawable_unref(drawable);
}

bool display_channel_wait_for_migrate_data(DisplayChannel *display)
{
    uint64_t end_time = spice_get_monotonic_time_ns() + DISPLAY_CLIENT_MIGRATE_DATA_TIMEOUT;
    RedChannelClient *rcc;
    int ret = FALSE;
    GList *clients = red_channel_get_clients(RED_CHANNEL(display));

    if (!red_channel_is_waiting_for_migrate_data(RED_CHANNEL(display))) {
        return FALSE;
    }

    spice_debug("trace");
    spice_warn_if_fail(g_list_length(clients) == 1);

    rcc = g_list_nth_data(clients, 0);

    g_object_ref(rcc);
    for (;;) {
        red_channel_client_receive(rcc);
        if (!red_channel_client_is_connected(rcc)) {
            break;
        }

        if (!red_channel_client_is_waiting_for_migrate_data(rcc)) {
            ret = TRUE;
            break;
        }
        if (spice_get_monotonic_time_ns() > end_time) {
            spice_warning("timeout");
            red_channel_client_disconnect(rcc);
            break;
        }
        usleep(DISPLAY_CLIENT_RETRY_INTERVAL);
    }
    g_object_unref(rcc);
    return ret;
}

void display_channel_flush_all_surfaces(DisplayChannel *display)
{
    int x;

    for (x = 0; x < NUM_SURFACES; ++x) {
        if (display->priv->surfaces[x].context.canvas) {
            display_channel_current_flush(display, x);
        }
    }
}

void display_channel_free_glz_drawables_to_free(DisplayChannel *display)
{
    DisplayChannelClient *dcc;

    spice_return_if_fail(display);

    FOREACH_DCC(display, dcc) {
        image_encoders_free_glz_drawables_to_free(dcc_get_encoders(dcc));
    }
}

void display_channel_free_glz_drawables(DisplayChannel *display)
{
    DisplayChannelClient *dcc;

    spice_return_if_fail(display);

    FOREACH_DCC(display, dcc) {
        image_encoders_free_glz_drawables(dcc_get_encoders(dcc));
    }
}

static bool free_one_drawable(DisplayChannel *display, int force_glz_free)
{
    RingItem *ring_item = ring_get_tail(&display->priv->current_list);
    Drawable *drawable;
    Container *container;

    if (!ring_item) {
        return FALSE;
    }

    drawable = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
    if (force_glz_free) {
        glz_retention_free_drawables(&drawable->glz_retention);
    }
    drawable_draw(display, drawable);
    container = drawable->tree_item.base.container;

    current_remove_drawable(display, drawable);
    container_cleanup(container);
    return TRUE;
}

void display_channel_current_flush(DisplayChannel *display, int surface_id)
{
    while (!ring_is_empty(&display->priv->surfaces[surface_id].current_list)) {
        free_one_drawable(display, FALSE);
    }
    current_remove_all(display, surface_id);
}

void display_channel_free_some(DisplayChannel *display)
{
    int n = 0;
    DisplayChannelClient *dcc;

    spice_debug("#draw=%d, #glz_draw=%d", display->priv->drawable_count,
                display->priv->encoder_shared_data.glz_drawable_count);
    FOREACH_DCC(display, dcc) {
        ImageEncoders *encoders = dcc_get_encoders(dcc);

        // encoding using the dictionary is prevented since the following operations might
        // change the dictionary
        if (image_encoders_glz_encode_lock(encoders)) {
            n = image_encoders_free_some_independent_glz_drawables(encoders);
        }
    }

    while (!ring_is_empty(&display->priv->current_list) && n++ < RED_RELEASE_BUNCH_SIZE) {
        free_one_drawable(display, TRUE);
    }

    FOREACH_DCC(display, dcc) {
        ImageEncoders *encoders = dcc_get_encoders(dcc);

        image_encoders_glz_encode_unlock(encoders);
    }
}

static Drawable* drawable_try_new(DisplayChannel *display)
{
    Drawable *drawable;

    if (!display->priv->free_drawables)
        return NULL;

    drawable = &display->priv->free_drawables->u.drawable;
    display->priv->free_drawables = display->priv->free_drawables->u.next;
    display->priv->drawable_count++;

    return drawable;
}

static void drawable_free(DisplayChannel *display, Drawable *drawable)
{
    ((_Drawable *)drawable)->u.next = display->priv->free_drawables;
    display->priv->free_drawables = (_Drawable *)drawable;
}

static void drawables_init(DisplayChannel *display)
{
    int i;

    display->priv->free_drawables = NULL;
    for (i = 0; i < NUM_DRAWABLES; i++) {
        drawable_free(display, &display->priv->drawables[i].u.drawable);
    }
}

/**
 * Allocate a Drawable
 *
 * @return pointer to uninitialized Drawable or NULL on failure
 */
static Drawable *display_channel_drawable_try_new(DisplayChannel *display,
                                                  uint32_t process_commands_generation)
{
    Drawable *drawable;

    while (!(drawable = drawable_try_new(display))) {
        if (!free_one_drawable(display, FALSE))
            return NULL;
    }

    memset(drawable, 0, sizeof(Drawable));
    /* Pointer to the display from which the drawable is allocated.  This
     * pointer is safe to be retained as DisplayChannel lifespan is bigger than
     * all drawables.  */
    drawable->display = display;
    drawable->refs = 1;
    drawable->creation_time = drawable->first_frame_time = spice_get_monotonic_time_ns();
    ring_item_init(&drawable->list_link);
    ring_item_init(&drawable->surface_list_link);
    ring_item_init(&drawable->tree_item.base.siblings_link);
    drawable->tree_item.base.type = TREE_ITEM_TYPE_DRAWABLE;
    region_init(&drawable->tree_item.base.rgn);
    glz_retention_init(&drawable->glz_retention);
    drawable->process_commands_generation = process_commands_generation;

    return drawable;
}

static void depended_item_remove(DependItem *item)
{
    spice_return_if_fail(item->drawable);
    spice_return_if_fail(ring_item_is_linked(&item->ring_item));

    item->drawable = NULL;
    ring_remove(&item->ring_item);
}

static void drawable_remove_dependencies(Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            depended_item_remove(&drawable->depend_items[x]);
        }
    }
}

static void drawable_unref_surface_deps(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id == -1) {
            continue;
        }
        display_channel_surface_unref(display, surface_id);
    }
}

void drawable_unref(Drawable *drawable)
{
    DisplayChannel *display = drawable->display;

    if (--drawable->refs != 0)
        return;

    spice_warn_if_fail(!drawable->tree_item.shadow);
    spice_warn_if_fail(drawable->pipes == NULL);

    if (drawable->stream) {
        video_stream_detach_drawable(drawable->stream);
    }
    region_destroy(&drawable->tree_item.base.rgn);

    drawable_remove_dependencies(drawable);
    drawable_unref_surface_deps(display, drawable);
    display_channel_surface_unref(display, drawable->surface_id);

    glz_retention_detach_drawables(&drawable->glz_retention);

    if (drawable->red_drawable) {
        red_drawable_unref(drawable->red_drawable);
    }
    drawable_free(display, drawable);
    display->priv->drawable_count--;
}

static void drawable_deps_draw(DisplayChannel *display, Drawable *drawable)
{
    int x;
    int surface_id;

    for (x = 0; x < 3; ++x) {
        surface_id = drawable->surface_deps[x];
        if (surface_id != -1 && drawable->depend_items[x].drawable) {
            depended_item_remove(&drawable->depend_items[x]);
            display_channel_draw(display, &drawable->red_drawable->surfaces_rects[x], surface_id);
        }
    }
}

static void drawable_draw(DisplayChannel *display, Drawable *drawable)
{
    RedSurface *surface;
    SpiceCanvas *canvas;
    SpiceClip clip = drawable->red_drawable->clip;

    drawable_deps_draw(display, drawable);

    surface = &display->priv->surfaces[drawable->surface_id];
    canvas = surface->context.canvas;
    spice_return_if_fail(canvas);

    image_cache_aging(&display->priv->image_cache);

    region_add(&surface->draw_dirty_region, &drawable->red_drawable->bbox);

    switch (drawable->red_drawable->type) {
    case QXL_DRAW_FILL: {
        SpiceFill fill = drawable->red_drawable->u.fill;
        SpiceImage img1, img2;
        image_cache_localize_brush(&display->priv->image_cache, &fill.brush, &img1);
        image_cache_localize_mask(&display->priv->image_cache, &fill.mask, &img2);
        canvas->ops->draw_fill(canvas, &drawable->red_drawable->bbox,
                               &clip, &fill);
        break;
    }
    case QXL_DRAW_OPAQUE: {
        SpiceOpaque opaque = drawable->red_drawable->u.opaque;
        SpiceImage img1, img2, img3;
        image_cache_localize_brush(&display->priv->image_cache, &opaque.brush, &img1);
        image_cache_localize(&display->priv->image_cache, &opaque.src_bitmap, &img2, drawable);
        image_cache_localize_mask(&display->priv->image_cache, &opaque.mask, &img3);
        canvas->ops->draw_opaque(canvas, &drawable->red_drawable->bbox, &clip, &opaque);
        break;
    }
    case QXL_DRAW_COPY: {
        SpiceCopy copy;
        SpiceImage img1, img2;
        red_drawable_extract_drm(drawable->red_drawable);
        copy = drawable->red_drawable->u.copy;
        image_cache_localize(&display->priv->image_cache, &copy.src_bitmap, &img1, drawable);
        image_cache_localize_mask(&display->priv->image_cache, &copy.mask, &img2);
        canvas->ops->draw_copy(canvas, &drawable->red_drawable->bbox,
                               &clip, &copy);
        break;
    }
    case QXL_DRAW_TRANSPARENT: {
        SpiceTransparent transparent = drawable->red_drawable->u.transparent;
        SpiceImage img1;
        image_cache_localize(&display->priv->image_cache, &transparent.src_bitmap, &img1, drawable);
        canvas->ops->draw_transparent(canvas,
                                      &drawable->red_drawable->bbox, &clip, &transparent);
        break;
    }
    case QXL_DRAW_ALPHA_BLEND: {
        SpiceAlphaBlend alpha_blend = drawable->red_drawable->u.alpha_blend;
        SpiceImage img1;
        image_cache_localize(&display->priv->image_cache, &alpha_blend.src_bitmap, &img1, drawable);
        canvas->ops->draw_alpha_blend(canvas,
                                      &drawable->red_drawable->bbox, &clip, &alpha_blend);
        break;
    }
    case QXL_COPY_BITS: {
        canvas->ops->copy_bits(canvas, &drawable->red_drawable->bbox,
                               &clip, &drawable->red_drawable->u.copy_bits.src_pos);
        break;
    }
    case QXL_DRAW_BLEND: {
        SpiceBlend blend = drawable->red_drawable->u.blend;
        SpiceImage img1, img2;
        image_cache_localize(&display->priv->image_cache, &blend.src_bitmap, &img1, drawable);
        image_cache_localize_mask(&display->priv->image_cache, &blend.mask, &img2);
        canvas->ops->draw_blend(canvas, &drawable->red_drawable->bbox,
                                &clip, &blend);
        break;
    }
    case QXL_DRAW_BLACKNESS: {
        SpiceBlackness blackness = drawable->red_drawable->u.blackness;
        SpiceImage img1;
        image_cache_localize_mask(&display->priv->image_cache, &blackness.mask, &img1);
        canvas->ops->draw_blackness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &blackness);
        break;
    }
    case QXL_DRAW_WHITENESS: {
        SpiceWhiteness whiteness = drawable->red_drawable->u.whiteness;
        SpiceImage img1;
        image_cache_localize_mask(&display->priv->image_cache, &whiteness.mask, &img1);
        canvas->ops->draw_whiteness(canvas,
                                    &drawable->red_drawable->bbox, &clip, &whiteness);
        break;
    }
    case QXL_DRAW_INVERS: {
        SpiceInvers invers = drawable->red_drawable->u.invers;
        SpiceImage img1;
        image_cache_localize_mask(&display->priv->image_cache, &invers.mask, &img1);
        canvas->ops->draw_invers(canvas,
                                 &drawable->red_drawable->bbox, &clip, &invers);
        break;
    }
    case QXL_DRAW_ROP3: {
        SpiceRop3 rop3 = drawable->red_drawable->u.rop3;
        SpiceImage img1, img2, img3;
        image_cache_localize_brush(&display->priv->image_cache, &rop3.brush, &img1);
        image_cache_localize(&display->priv->image_cache, &rop3.src_bitmap, &img2, drawable);
        image_cache_localize_mask(&display->priv->image_cache, &rop3.mask, &img3);
        canvas->ops->draw_rop3(canvas, &drawable->red_drawable->bbox,
                               &clip, &rop3);
        break;
    }
    case QXL_DRAW_COMPOSITE: {
        SpiceComposite composite = drawable->red_drawable->u.composite;
        SpiceImage src, mask;
        image_cache_localize(&display->priv->image_cache, &composite.src_bitmap, &src, drawable);
        if (composite.mask_bitmap)
            image_cache_localize(&display->priv->image_cache, &composite.mask_bitmap, &mask, drawable);
        canvas->ops->draw_composite(canvas, &drawable->red_drawable->bbox,
                                    &clip, &composite);
        break;
    }
    case QXL_DRAW_STROKE: {
        SpiceStroke stroke = drawable->red_drawable->u.stroke;
        SpiceImage img1;
        image_cache_localize_brush(&display->priv->image_cache, &stroke.brush, &img1);
        canvas->ops->draw_stroke(canvas,
                                 &drawable->red_drawable->bbox, &clip, &stroke);
        break;
    }
    case QXL_DRAW_TEXT: {
        SpiceText text = drawable->red_drawable->u.text;
        SpiceImage img1, img2;
        image_cache_localize_brush(&display->priv->image_cache, &text.fore_brush, &img1);
        image_cache_localize_brush(&display->priv->image_cache, &text.back_brush, &img2);
        canvas->ops->draw_text(canvas, &drawable->red_drawable->bbox,
                               &clip, &text);
        break;
    }
    default:
        spice_warning("invalid type");
    }
}

static void surface_update_dest(RedSurface *surface, const SpiceRect *area)
{
    SpiceCanvas *canvas = surface->context.canvas;
    int stride = surface->context.stride;
    uint8_t *line_0 = surface->context.line_0;

    if (surface->context.canvas_draws_on_surface)
        return;

    int h = area->bottom - area->top;
    if (h == 0)
        return;

    spice_return_if_fail(stride < 0);

    uint8_t *dest = line_0 + (area->top * stride) + area->left * sizeof(uint32_t);
    dest += (h - 1) * stride;
    canvas->ops->read_bits(canvas, dest, -stride, area);
}

/* Draws all drawables associated with @surface, starting from the tail of the
 * ring, and stopping after it draws @last */
static void draw_until(DisplayChannel *display, RedSurface *surface, Drawable *last)
{
    RingItem *ring_item;
    Container *container;
    Drawable *now;

    do {
        ring_item = ring_get_tail(&surface->current_list);
        now = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        now->refs++;
        container = now->tree_item.base.container;
        current_remove_drawable(display, now);
        container_cleanup(container);
        /* drawable_draw may call display_channel_draw for the surfaces 'now' depends on. Notice,
           that it is valid to call display_channel_draw in this case and not display_channel_draw_till:
           It is impossible that there was newer item then 'last' in one of the surfaces
           that display_channel_draw is called for, Otherwise, 'now' would have already been rendered.
           See the call for red_handle_depends_on_target_surface in red_process_draw */
        drawable_draw(display, now);
        drawable_unref(now);
    } while (now != last);
}

/* Find the first Drawable in the @current ring that intersects the given
 * @area, starting at item @from (or the head of the ring if @from is NULL).
 *
 * NOTE: this function expects @current to be a ring of Drawables, and more
 * specifically an instance of Surface::current_list (not Surface::current) */
static Drawable* current_find_intersects_rect(Ring *current, RingItem *from,
                                              const SpiceRect *area)
{
    RingItem *it;
    QRegion rgn;
    Drawable *last = NULL;

    region_init(&rgn);
    region_add(&rgn, area);

    for (it = from ? from : ring_next(current, current); it != NULL; it = ring_next(current, it)) {
        Drawable *now = SPICE_CONTAINEROF(it, Drawable, surface_list_link);
        if (region_intersects(&rgn, &now->tree_item.base.rgn)) {
            last = now;
            break;
        }
    }

    region_destroy(&rgn);
    return last;
}

/*
 * Renders drawables for updating the requested area, but only drawables that are older
 * than 'last' (exclusive).
 * FIXME: merge with display_channel_draw()?
 */
void display_channel_draw_until(DisplayChannel *display, const SpiceRect *area, int surface_id,
                               Drawable *last)
{
    RedSurface *surface;
    Drawable *surface_last = NULL;
    Ring *ring;
    RingItem *ring_item;
    Drawable *now;

    spice_return_if_fail(last);
    spice_return_if_fail(ring_item_is_linked(&last->list_link));

    surface = &display->priv->surfaces[surface_id];

    if (surface_id != last->surface_id) {
        // find the nearest older drawable from the appropriate surface
        ring = &display->priv->current_list;
        ring_item = &last->list_link;
        while ((ring_item = ring_next(ring, ring_item))) {
            now = SPICE_CONTAINEROF(ring_item, Drawable, list_link);
            if (now->surface_id == surface_id) {
                surface_last = now;
                break;
            }
        }
    } else {
        ring_item = ring_next(&surface->current_list, &last->surface_list_link);
        if (ring_item) {
            surface_last = SPICE_CONTAINEROF(ring_item, Drawable, surface_list_link);
        }
    }

    if (!surface_last)
        return;

    last = current_find_intersects_rect(&surface->current_list,
                                        &surface_last->surface_list_link, area);
    if (!last)
        return;

    draw_until(display, surface, last);
    surface_update_dest(surface, area);
}

void display_channel_draw(DisplayChannel *display, const SpiceRect *area, int surface_id)
{
    RedSurface *surface;
    Drawable *last;

    spice_return_if_fail(surface_id >= 0 && surface_id < NUM_SURFACES);
    spice_return_if_fail(area);
    spice_return_if_fail(area->left >= 0 && area->top >= 0 &&
                         area->left < area->right && area->top < area->bottom);

    surface = &display->priv->surfaces[surface_id];

    last = current_find_intersects_rect(&surface->current_list, NULL, area);
    if (last)
        draw_until(display, surface, last);

    surface_update_dest(surface, area);
}

static void region_to_qxlrects(QRegion *region, QXLRect *qxl_rects, uint32_t num_rects)
{
    SpiceRect *rects;
    int i;

    rects = g_new0(SpiceRect, num_rects);
    region_ret_rects(region, rects, num_rects);
    for (i = 0; i < num_rects; i++) {
        qxl_rects[i].top    = rects[i].top;
        qxl_rects[i].left   = rects[i].left;
        qxl_rects[i].bottom = rects[i].bottom;
        qxl_rects[i].right  = rects[i].right;
    }
    g_free(rects);
}

void display_channel_update(DisplayChannel *display,
                            uint32_t surface_id, const QXLRect *area, uint32_t clear_dirty,
                            QXLRect **qxl_dirty_rects, uint32_t *num_dirty_rects)
{
    SpiceRect rect;
    RedSurface *surface;

    spice_return_if_fail(display_channel_validate_surface(display, surface_id));

    red_get_rect_ptr(&rect, area);
    display_channel_draw(display, &rect, surface_id);

    surface = &display->priv->surfaces[surface_id];
    if (*qxl_dirty_rects == NULL) {
        *num_dirty_rects = pixman_region32_n_rects(&surface->draw_dirty_region);
        *qxl_dirty_rects = g_new0(QXLRect, *num_dirty_rects);
    }

    region_to_qxlrects(&surface->draw_dirty_region, *qxl_dirty_rects, *num_dirty_rects);
    if (clear_dirty)
        region_clear(&surface->draw_dirty_region);
}

static void clear_surface_drawables_from_pipes(DisplayChannel *display, int surface_id,
                                               int wait_if_used)
{
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, dcc) {
        if (!dcc_clear_surface_drawables_from_pipe(dcc, surface_id, wait_if_used)) {
            red_channel_client_disconnect(RED_CHANNEL_CLIENT(dcc));
        }
    }
}

/* TODO: cleanup/refactor destroy functions */
static void display_channel_destroy_surface(DisplayChannel *display, uint32_t surface_id)
{
    draw_depend_on_me(display, surface_id);
    /* note that draw_depend_on_me must be called before current_remove_all.
       otherwise "current" will hold items that other drawables may depend on, and then
       current_remove_all will remove them from the pipe. */
    current_remove_all(display, surface_id);
    clear_surface_drawables_from_pipes(display, surface_id, FALSE);
    display_channel_surface_unref(display, surface_id);
}

void display_channel_destroy_surface_wait(DisplayChannel *display, uint32_t surface_id)
{
    if (!display_channel_validate_surface(display, surface_id))
        return;
    if (!display->priv->surfaces[surface_id].context.canvas)
        return;

    draw_depend_on_me(display, surface_id);
    /* note that draw_depend_on_me must be called before current_remove_all.
       otherwise "current" will hold items that other drawables may depend on, and then
       current_remove_all will remove them from the pipe. */
    current_remove_all(display, surface_id);
    clear_surface_drawables_from_pipes(display, surface_id, TRUE);
}

/* called upon device reset */
/* TODO: split me*/
void display_channel_destroy_surfaces(DisplayChannel *display)
{
    int i;

    spice_debug("trace");
    //to handle better
    for (i = 0; i < NUM_SURFACES; ++i) {
        if (display->priv->surfaces[i].context.canvas) {
            display_channel_destroy_surface_wait(display, i);
            if (display->priv->surfaces[i].context.canvas) {
                display_channel_surface_unref(display, i);
            }
            spice_assert(!display->priv->surfaces[i].context.canvas);
        }
    }
    spice_warn_if_fail(ring_is_empty(&display->priv->streams));

    if (red_channel_is_connected(RED_CHANNEL(display))) {
        red_channel_pipes_add_type(RED_CHANNEL(display), RED_PIPE_ITEM_TYPE_INVAL_PALETTE_CACHE);
        red_channel_pipes_add_empty_msg(RED_CHANNEL(display), SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL);
    }

    display_channel_free_glz_drawables(display);
}

static void send_create_surface(DisplayChannel *display, int surface_id, int image_ready)
{
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, dcc) {
        dcc_create_surface(dcc, surface_id);
        if (image_ready)
            dcc_push_surface_image(dcc, surface_id);
    }
}

static SpiceCanvas*
create_canvas_for_surface(DisplayChannel *display, RedSurface *surface, uint32_t renderer)
{
    SpiceCanvas *canvas;

    switch (renderer) {
    case RED_RENDERER_SW:
        canvas = canvas_create_for_data(surface->context.width, surface->context.height, surface->context.format,
                                        surface->context.line_0, surface->context.stride,
                                        &display->priv->image_cache.base,
                                        &display->priv->image_surfaces, NULL, NULL, NULL);
        surface->context.top_down = TRUE;
        surface->context.canvas_draws_on_surface = TRUE;
        return canvas;
    default:
        spice_warn_if_reached();
    };

    return NULL;
}

void display_channel_create_surface(DisplayChannel *display, uint32_t surface_id, uint32_t width,
                                    uint32_t height, int32_t stride, uint32_t format,
                                    void *line_0, int data_is_valid, int send_client)
{
    RedSurface *surface = &display->priv->surfaces[surface_id];

    spice_warn_if_fail(!surface->context.canvas);

    surface->context.canvas_draws_on_surface = FALSE;
    surface->context.width = width;
    surface->context.height = height;
    surface->context.format = format;
    surface->context.stride = stride;
    surface->context.line_0 = line_0;
    if (!data_is_valid) {
        char *data = line_0;
        if (stride < 0) {
            data -= abs(stride) * (height - 1);
        }
        memset(data, 0, height*abs(stride));
    }
    g_warn_if_fail(surface->create_cmd == NULL);
    g_warn_if_fail(surface->destroy_cmd == NULL);
    ring_init(&surface->current);
    ring_init(&surface->current_list);
    ring_init(&surface->depend_on_me);
    region_init(&surface->draw_dirty_region);
    surface->refs = 1;

    if (display->priv->renderer == RED_RENDERER_INVALID) {
        int i;
        RedsState *reds = red_channel_get_server(RED_CHANNEL(display));
        GArray *renderers = reds_get_renderers(reds);
        for (i = 0; i < renderers->len; i++) {
            uint32_t renderer = g_array_index(renderers, uint32_t, i);
            surface->context.canvas = create_canvas_for_surface(display, surface, renderer);
            if (surface->context.canvas) {
                display->priv->renderer = renderer;
                break;
            }
        }
    } else {
        surface->context.canvas = create_canvas_for_surface(display, surface, display->priv->renderer);
    }

    spice_return_if_fail(surface->context.canvas);
    if (send_client)
        send_create_surface(display, surface_id, data_is_valid);
}

static bool handle_migrate_flush_mark(RedChannelClient *rcc)
{
    RedChannel *channel = red_channel_client_get_channel(rcc);

    red_channel_pipes_add_type(channel, RED_PIPE_ITEM_TYPE_MIGRATE_DATA);
    return TRUE;
}

static uint64_t handle_migrate_data_get_serial(RedChannelClient *rcc, uint32_t size, void *message)
{
    SpiceMigrateDataDisplay *migrate_data;

    migrate_data = (SpiceMigrateDataDisplay *)((uint8_t *)message + sizeof(SpiceMigrateDataHeader));

    return migrate_data->message_serial;
}

static bool handle_migrate_data(RedChannelClient *rcc, uint32_t size, void *message)
{
    return dcc_handle_migrate_data(DISPLAY_CHANNEL_CLIENT(rcc), size, message);
}

static SpiceCanvas *image_surfaces_get(SpiceImageSurfaces *surfaces, uint32_t surface_id)
{
    DisplayChannelPrivate *p = SPICE_CONTAINEROF(surfaces, DisplayChannelPrivate, image_surfaces);
    DisplayChannel *display = p->pub;

    spice_return_val_if_fail(display_channel_validate_surface(display, surface_id), NULL);

    return p->surfaces[surface_id].context.canvas;
}

DisplayChannel* display_channel_new(RedsState *reds,
                                    QXLInstance *qxl,
                                    const SpiceCoreInterfaceInternal *core,
                                    int migrate, int stream_video,
                                    GArray *video_codecs,
                                    uint32_t n_surfaces)
{
    DisplayChannel *display;

    /* FIXME: migrate is not used...? */
    spice_debug("create display channel");
    display = g_object_new(TYPE_DISPLAY_CHANNEL,
                           "spice-server", reds,
                           "core-interface", core,
                           "channel-type", SPICE_CHANNEL_DISPLAY,
                           "id", qxl->id,
                           "migration-flags",
                           (SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER),
                           "qxl", qxl,
                           "n-surfaces", n_surfaces,
                           "video-codecs", video_codecs,
                           "handle-acks", TRUE,
                           NULL);
    if (display) {
        display_channel_set_stream_video(display, stream_video);
    }
    return display;
}

static SpiceCanvas *image_surfaces_get(SpiceImageSurfaces *surfaces, uint32_t surface_id);
static void drawables_init(DisplayChannel *display);
static void
display_channel_init(DisplayChannel *self)
{
    static const SpiceImageSurfacesOps image_surfaces_ops = {
        image_surfaces_get,
    };

    /* must be manually allocated here since g_type_class_add_private() only
     * supports structs smaller than 64k */
    self->priv = g_new0(DisplayChannelPrivate, 1);
    self->priv->pub = self;

    image_encoder_shared_init(&self->priv->encoder_shared_data);

    ring_init(&self->priv->current_list);
    drawables_init(self);
    self->priv->image_surfaces.ops = &image_surfaces_ops;
}

static void
display_channel_constructed(GObject *object)
{
    DisplayChannel *self = DISPLAY_CHANNEL(object);
    RedChannel *channel = RED_CHANNEL(self);

    G_OBJECT_CLASS(display_channel_parent_class)->constructed(object);

    spice_assert(self->priv->video_codecs);

    self->priv->renderer = RED_RENDERER_INVALID;

    stat_init(&self->priv->add_stat, "add", CLOCK_THREAD_CPUTIME_ID);
    stat_init(&self->priv->exclude_stat, "exclude", CLOCK_THREAD_CPUTIME_ID);
    stat_init(&self->priv->__exclude_stat, "__exclude", CLOCK_THREAD_CPUTIME_ID);
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));
    const RedStatNode *stat = red_channel_get_stat_node(channel);
    stat_init_counter(&self->priv->cache_hits_counter, reds, stat,
                      "cache_hits", TRUE);
    stat_init_counter(&self->priv->add_to_cache_counter, reds, stat,
                      "add_to_cache", TRUE);
    stat_init_counter(&self->priv->non_cache_counter, reds, stat,
                      "non_cache", TRUE);
    image_cache_init(&self->priv->image_cache);
    self->priv->stream_video = SPICE_STREAM_VIDEO_OFF;
    display_channel_init_video_streams(self);

    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_MONITORS_CONFIG);
    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_PREF_COMPRESSION);
    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_PREF_VIDEO_CODEC_TYPE);
    red_channel_set_cap(channel, SPICE_DISPLAY_CAP_STREAM_REPORT);
}

void display_channel_process_surface_cmd(DisplayChannel *display,
                                         RedSurfaceCmd *surface_cmd,
                                         int loadvm)
{
    uint32_t surface_id;
    RedSurface *surface;
    uint8_t *data;

    surface_id = surface_cmd->surface_id;
    if SPICE_UNLIKELY(surface_id >= display->priv->n_surfaces) {
        return;
    }

    surface = &display->priv->surfaces[surface_id];

    switch (surface_cmd->type) {
    case QXL_SURFACE_CMD_CREATE: {
        const RedSurfaceCreate *create = &surface_cmd->u.surface_create;
        uint32_t height = create->height;
        int32_t stride = create->stride;
        int reloaded_surface = loadvm || (surface_cmd->flags & QXL_SURF_FLAG_KEEP_DATA);

        if (surface->refs) {
            spice_warning("avoiding creating a surface twice");
            break;
        }
        data = create->data;
        if (stride < 0) {
            /* No need to worry about overflow here, command should already be validated
             * when it is read, specifically red_get_surface_cmd */
            data -= (int32_t)(stride * (height - 1));
        }
        display_channel_create_surface(display, surface_id, create->width,
                                       height, stride, create->format, data,
                                       reloaded_surface,
                                       // reloaded surfaces will be sent on demand
                                       !reloaded_surface);
        surface->create_cmd = red_surface_cmd_ref(surface_cmd);
        break;
    }
    case QXL_SURFACE_CMD_DESTROY:
        if (!surface->refs) {
            spice_warning("avoiding destroying a surface twice");
            break;
        }
        surface->destroy_cmd = red_surface_cmd_ref(surface_cmd);
        display_channel_destroy_surface(display, surface_id);
        break;
    default:
        spice_warn_if_reached();
    };
}

void display_channel_update_compression(DisplayChannel *display, DisplayChannelClient *dcc)
{
    if (dcc_get_jpeg_state(dcc) == SPICE_WAN_COMPRESSION_AUTO) {
        display->priv->enable_jpeg = dcc_is_low_bandwidth(dcc);
    } else {
        display->priv->enable_jpeg = (dcc_get_jpeg_state(dcc) == SPICE_WAN_COMPRESSION_ALWAYS);
    }

    if (dcc_get_zlib_glz_state(dcc) == SPICE_WAN_COMPRESSION_AUTO) {
        display->priv->enable_zlib_glz_wrap = dcc_is_low_bandwidth(dcc);
    } else {
        display->priv->enable_zlib_glz_wrap = (dcc_get_zlib_glz_state(dcc) == SPICE_WAN_COMPRESSION_ALWAYS);
    }
    spice_debug("jpeg %s", display->priv->enable_jpeg ? "enabled" : "disabled");
    spice_debug("zlib-over-glz %s", display->priv->enable_zlib_glz_wrap ? "enabled" : "disabled");
}

void display_channel_gl_scanout(DisplayChannel *display)
{
    red_channel_pipes_new_add(RED_CHANNEL(display), dcc_gl_scanout_item_new, NULL);
}

static void set_gl_draw_async_count(DisplayChannel *display, int num)
{
    display->priv->gl_draw_async_count = num;

    if (num == 0) {
        red_qxl_gl_draw_async_complete(display->priv->qxl);
    }
}

void display_channel_gl_draw(DisplayChannel *display, SpiceMsgDisplayGlDraw *draw)
{
    int num;

    spice_return_if_fail(display->priv->gl_draw_async_count == 0);

    num = red_channel_pipes_new_add(RED_CHANNEL(display), dcc_gl_draw_item_new, draw);
    set_gl_draw_async_count(display, num);
}

void display_channel_gl_draw_done(DisplayChannel *display)
{
    set_gl_draw_async_count(display, display->priv->gl_draw_async_count - 1);
}

int display_channel_get_video_stream_id(DisplayChannel *display, VideoStream *stream)
{
    return (int)(stream - display->priv->streams_buf);
}

VideoStream *display_channel_get_nth_video_stream(DisplayChannel *display, gint i)
{
    return &display->priv->streams_buf[i];
}

gboolean display_channel_validate_surface(DisplayChannel *display, uint32_t surface_id)
{
    if SPICE_UNLIKELY(surface_id >= display->priv->n_surfaces) {
        spice_warning("invalid surface_id %u", surface_id);
        return FALSE;
    }
    if (!display->priv->surfaces[surface_id].context.canvas) {
        spice_warning("canvas address is %p for %d (and is NULL)",
                   &(display->priv->surfaces[surface_id].context.canvas), surface_id);
        spice_warning("failed on %d", surface_id);
        return FALSE;
    }
    return TRUE;
}

void display_channel_push_monitors_config(DisplayChannel *display)
{
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, dcc) {
        dcc_push_monitors_config(dcc);
    }
}

void display_channel_update_monitors_config(DisplayChannel *display,
                                            QXLMonitorsConfig *config,
                                            uint16_t count, uint16_t max_allowed)
{

    if (display->priv->monitors_config)
        monitors_config_unref(display->priv->monitors_config);

    display->priv->monitors_config =
        monitors_config_new(config->heads, count, max_allowed);

    display_channel_push_monitors_config(display);
}

void display_channel_set_monitors_config_to_primary(DisplayChannel *display)
{
    DrawContext *context = &display->priv->surfaces[0].context;
    QXLHead head = { 0, };
    uint16_t old_max = 1;

    spice_return_if_fail(display->priv->surfaces[0].context.canvas);

    if (display->priv->monitors_config) {
        old_max = display->priv->monitors_config->max_allowed;
        monitors_config_unref(display->priv->monitors_config);
    }

    head.width = context->width;
    head.height = context->height;
    display->priv->monitors_config = monitors_config_new(&head, 1, old_max);
}

void display_channel_reset_image_cache(DisplayChannel *self)
{
    image_cache_reset(&self->priv->image_cache);
}

static void
display_channel_class_init(DisplayChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->get_property = display_channel_get_property;
    object_class->set_property = display_channel_set_property;
    object_class->constructed = display_channel_constructed;
    object_class->finalize = display_channel_finalize;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_DISPLAY, NULL);
    channel_class->handle_message = dcc_handle_message;

    channel_class->send_item = dcc_send_item;
    channel_class->handle_migrate_flush_mark = handle_migrate_flush_mark;
    channel_class->handle_migrate_data = handle_migrate_data;
    channel_class->handle_migrate_data_get_serial = handle_migrate_data_get_serial;

    g_object_class_install_property(object_class,
                                    PROP_N_SURFACES,
                                    g_param_spec_uint("n-surfaces",
                                                      "number of surfaces",
                                                      "Number of surfaces for this channel",
                                                      1, NUM_SURFACES,
                                                      1,
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object_class,
                                    PROP_VIDEO_CODECS,
                                    g_param_spec_boxed("video-codecs",
                                                       "video codecs",
                                                       "Video Codecs",
                                                       G_TYPE_ARRAY,
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object_class,
                                    PROP_QXL,
                                    g_param_spec_pointer("qxl",
                                                         "qxl",
                                                         "QXLInstance for this channel",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));
}

void display_channel_debug_oom(DisplayChannel *display, const char *msg)
{
    RedChannel *channel = RED_CHANNEL(display);

    spice_debug("%s #draw=%u, #glz_draw=%u current %u pipes %u",
                msg,
                display->priv->drawable_count,
                display->priv->encoder_shared_data.glz_drawable_count,
                ring_get_length(&display->priv->current_list),
                red_channel_sum_pipes_size(channel));
}
