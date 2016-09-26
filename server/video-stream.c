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

#include "video-stream.h"
#include "display-channel-private.h"
#include "main-channel-client.h"
#include "red-client.h"
#include "egl.h"

#define FPS_TEST_INTERVAL 1
#define FOREACH_STREAMS(display, item)                  \
    RING_FOREACH(item, &(display)->priv->streams)

static void video_stream_agent_stats_print(VideoStreamAgent *agent)
{
#ifdef STREAM_STATS
    StreamStats *stats = &agent->stats;
    double passed_mm_time = (stats->end - stats->start) / 1000.0;
    VideoEncoderStats encoder_stats = {0};

    if (agent->video_encoder) {
        agent->video_encoder->get_stats(agent->video_encoder, &encoder_stats);
    }

    spice_debug("stream=%p dim=(%dx%d) #in-frames=%"PRIu64" #in-avg-fps=%.2f #out-frames=%"PRIu64" "
                "out/in=%.2f #drops=%"PRIu64" (#pipe=%"PRIu64" #fps=%"PRIu64") out-avg-fps=%.2f "
                "passed-mm-time(sec)=%.2f size-total(MB)=%.2f size-per-sec(Mbps)=%.2f "
                "size-per-frame(KBpf)=%.2f avg-quality=%.2f "
                "start-bit-rate(Mbps)=%.2f end-bit-rate(Mbps)=%.2f",
                agent, agent->stream->width, agent->stream->height,
                stats->num_input_frames,
                stats->num_input_frames / passed_mm_time,
                stats->num_frames_sent,
                (stats->num_frames_sent + 0.0) / stats->num_input_frames,
                stats->num_drops_pipe +
                stats->num_drops_fps,
                stats->num_drops_pipe,
                stats->num_drops_fps,
                stats->num_frames_sent / passed_mm_time,
                passed_mm_time,
                stats->size_sent / 1024.0 / 1024.0,
                ((stats->size_sent * 8.0) / (1024.0 * 1024)) / passed_mm_time,
                stats->size_sent / 1000.0 / stats->num_frames_sent,
                encoder_stats.avg_quality,
                encoder_stats.starting_bit_rate / (1024.0 * 1024),
                encoder_stats.cur_bit_rate / (1024.0 * 1024));
#endif
}

static void video_stream_create_destroy_item_release(RedPipeItem *base)
{
    StreamCreateDestroyItem *item = SPICE_UPCAST(StreamCreateDestroyItem, base);
    DisplayChannel *display = DCC_TO_DC(item->agent->dcc);
    video_stream_agent_unref(display, item->agent);
    g_free(item);
}

static RedPipeItem *video_stream_create_destroy_item_new(VideoStreamAgent *agent,
                                                         gint type)
{
    StreamCreateDestroyItem *item = g_new0(StreamCreateDestroyItem, 1);

    red_pipe_item_init_full(&item->base, type,
                            video_stream_create_destroy_item_release);
    agent->stream->refs++;
    item->agent = agent;
    return &item->base;
}

static RedPipeItem *video_stream_create_item_new(VideoStreamAgent *agent)
{
    return video_stream_create_destroy_item_new(agent, RED_PIPE_ITEM_TYPE_STREAM_CREATE);
}

static RedPipeItem *video_stream_destroy_item_new(VideoStreamAgent *agent)
{
    return video_stream_create_destroy_item_new(agent, RED_PIPE_ITEM_TYPE_STREAM_DESTROY);
}


void video_stream_stop(DisplayChannel *display, VideoStream *stream)
{
    DisplayChannelClient *dcc;
    int stream_id = display_channel_get_video_stream_id(display, stream);

    spice_return_if_fail(ring_item_is_linked(&stream->link));
    spice_return_if_fail(!stream->current);

    spice_debug("stream %d", stream_id);
    FOREACH_DCC(display, dcc) {
        VideoStreamAgent *stream_agent;

        stream_agent = dcc_get_video_stream_agent(dcc, stream_id);
        region_clear(&stream_agent->vis_region);
        region_clear(&stream_agent->clip);
        if (stream_agent->video_encoder) {
            uint64_t stream_bit_rate = stream_agent->video_encoder->get_bit_rate(stream_agent->video_encoder);

            if (stream_bit_rate > dcc_get_max_stream_bit_rate(dcc)) {
                spice_debug("old max-bit-rate=%.2f new=%.2f",
                            dcc_get_max_stream_bit_rate(dcc) / 8.0 / 1024.0 / 1024.0,
                            stream_bit_rate / 8.0 / 1024.0 / 1024.0);
                dcc_set_max_stream_bit_rate(dcc, stream_bit_rate);
            }
        }
        red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc),
                                    video_stream_destroy_item_new(stream_agent));
        video_stream_agent_stats_print(stream_agent);
    }
    display->priv->streams_size_total -= stream->width * stream->height;
    ring_remove(&stream->link);
    video_stream_unref(display, stream);
}

static void video_stream_free(DisplayChannel *display, VideoStream *stream)
{
    stream->next = display->priv->free_streams;
    display->priv->free_streams = stream;
}

void display_channel_init_video_streams(DisplayChannel *display)
{
    int i;

    ring_init(&display->priv->streams);
    display->priv->free_streams = NULL;
    for (i = 0; i < NUM_STREAMS; i++) {
        VideoStream *stream = display_channel_get_nth_video_stream(display, i);
        ring_item_init(&stream->link);
        video_stream_free(display, stream);
    }
}

void video_stream_unref(DisplayChannel *display, VideoStream *stream)
{
    if (--stream->refs != 0)
        return;

    spice_warn_if_fail(!ring_item_is_linked(&stream->link));

    video_stream_free(display, stream);
    display->priv->stream_count--;
}

void video_stream_agent_unref(DisplayChannel *display, VideoStreamAgent *agent)
{
    video_stream_unref(display, agent->stream);
}

static void video_stream_clip_item_free(RedPipeItem *base)
{
    g_return_if_fail(base != NULL);
    VideoStreamClipItem *item = SPICE_UPCAST(VideoStreamClipItem, base);
    DisplayChannel *display = DCC_TO_DC(item->stream_agent->dcc);

    g_return_if_fail(item->base.refcount == 0);

    video_stream_agent_unref(display, item->stream_agent);
    g_free(item->rects);
    g_free(item);
}

VideoStreamClipItem *video_stream_clip_item_new(VideoStreamAgent *agent)
{
    VideoStreamClipItem *item = g_new(VideoStreamClipItem, 1);
    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_STREAM_CLIP,
                            video_stream_clip_item_free);

    item->stream_agent = agent;
    agent->stream->refs++;

    item->clip_type = SPICE_CLIP_TYPE_RECTS;

    int n_rects = pixman_region32_n_rects(&agent->clip);
    item->rects = g_malloc(sizeof(SpiceClipRects) + n_rects * sizeof(SpiceRect));
    item->rects->num_rects = n_rects;
    region_ret_rects(&agent->clip, item->rects->rects, n_rects);

    return item;
}

static int is_stream_start(Drawable *drawable)
{
    return ((drawable->frames_count >= RED_STREAM_FRAMES_START_CONDITION) &&
            (drawable->gradual_frames_count >=
             (RED_STREAM_GRADUAL_FRAMES_START_CONDITION * drawable->frames_count)));
}

static void update_copy_graduality(DisplayChannel *display, Drawable *drawable)
{
    SpiceBitmap *bitmap;
    spice_return_if_fail(drawable->red_drawable->type == QXL_DRAW_COPY);

    if (display_channel_get_stream_video(display) != SPICE_STREAM_VIDEO_FILTER) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_INVALID;
        return;
    }

    if (drawable->copy_bitmap_graduality != BITMAP_GRADUAL_INVALID) {
        return; // already set
    }

    red_drawable_extract_drm(drawable->red_drawable);
    bitmap = &drawable->red_drawable->u.copy.src_bitmap->u.bitmap;

    if (!bitmap_fmt_has_graduality(bitmap->format) || bitmap_has_extra_stride(bitmap) ||
        (bitmap->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE)) {
        drawable->copy_bitmap_graduality = BITMAP_GRADUAL_NOT_AVAIL;
    } else  {
        drawable->copy_bitmap_graduality = bitmap_get_graduality_level(bitmap);
    }
}

static bool is_next_stream_frame(const Drawable *candidate,
                                 const int other_src_width,
                                 const int other_src_height,
                                 const SpiceRect *other_dest,
                                 const red_time_t other_time,
                                 const VideoStream *stream,
                                 int container_candidate_allowed)
{
    RedDrawable *red_drawable;

    if (!candidate->streamable) {
        return FALSE;
    }

    if (candidate->creation_time - other_time >
            (stream ? RED_STREAM_CONTINUOUS_MAX_DELTA : RED_STREAM_DETECTION_MAX_DELTA)) {
        return FALSE;
    }

    red_drawable = candidate->red_drawable;
    if (!container_candidate_allowed) {
        SpiceRect* candidate_src;

        if (!rect_is_equal(&red_drawable->bbox, other_dest)) {
            return FALSE;
        }

        candidate_src = &red_drawable->u.copy.src_area;
        if (candidate_src->right - candidate_src->left != other_src_width ||
            candidate_src->bottom - candidate_src->top != other_src_height) {
            return FALSE;
        }
    } else {
        if (!rect_contains(&red_drawable->bbox, other_dest)) {
            return FALSE;
        }
        int candidate_area = rect_get_area(&red_drawable->bbox);
        int other_area = rect_get_area(other_dest);
        /* do not stream drawables that are significantly
         * bigger than the original frame */
        if (candidate_area > 2 * other_area) {
            spice_debug("too big candidate:");
            spice_debug("prev box ==>");
            rect_debug(other_dest);
            spice_debug("new box ==>");
            rect_debug(&red_drawable->bbox);
            return FALSE;
        }
    }

    if (stream) {
        if (stream->top_down != spice_image_get_top_down(red_drawable->u.copy.src_bitmap)) {
            return FALSE;
        }
    }
    return TRUE;
}

static void attach_stream(DisplayChannel *display, Drawable *drawable, VideoStream *stream)
{
    DisplayChannelClient *dcc;

    spice_assert(drawable && stream);
    spice_assert(!drawable->stream && !stream->current);
    stream->current = drawable;
    drawable->stream = stream;
    stream->last_time = drawable->creation_time;

    uint64_t duration = drawable->creation_time - stream->input_fps_start_time;
    if (duration >= RED_STREAM_INPUT_FPS_TIMEOUT) {
        /* Round to the nearest integer, for instance 24 for 23.976 */
        stream->input_fps = ((uint64_t)stream->num_input_frames * 1000 * 1000 * 1000 + duration / 2) / duration;
        spice_debug("input-fps=%u", stream->input_fps);
        stream->num_input_frames = 0;
        stream->input_fps_start_time = drawable->creation_time;
    } else {
        stream->num_input_frames++;
    }

    int stream_id = display_channel_get_video_stream_id(display, stream);
    FOREACH_DCC(display, dcc) {
        VideoStreamAgent *agent;
        QRegion clip_in_draw_dest;

        agent = dcc_get_video_stream_agent(dcc, stream_id);
        region_or(&agent->vis_region, &drawable->tree_item.base.rgn);

        region_init(&clip_in_draw_dest);
        region_add(&clip_in_draw_dest, &drawable->red_drawable->bbox);
        region_and(&clip_in_draw_dest, &agent->clip);

        if (!region_is_equal(&clip_in_draw_dest, &drawable->tree_item.base.rgn)) {
            region_remove(&agent->clip, &drawable->red_drawable->bbox);
            region_or(&agent->clip, &drawable->tree_item.base.rgn);
            dcc_video_stream_agent_clip(dcc, agent);
        }
        region_destroy(&clip_in_draw_dest);
#ifdef STREAM_STATS
        agent->stats.num_input_frames++;
#endif
    }
}

void video_stream_detach_drawable(VideoStream *stream)
{
    spice_assert(stream->current && stream->current->stream);
    spice_assert(stream->current->stream == stream);
    stream->current->stream = NULL;
    stream->current = NULL;
}

static void before_reattach_stream(DisplayChannel *display,
                                   VideoStream *stream, Drawable *new_frame)
{
    DisplayChannelClient *dcc;
    int index;
    VideoStreamAgent *agent;
    GList *dpi_link, *dpi_next;

    spice_return_if_fail(stream->current);

    if (!red_channel_is_connected(RED_CHANNEL(display))) {
        return;
    }

    if (new_frame->process_commands_generation == stream->current->process_commands_generation) {
        spice_debug("ignoring drop, same process_commands_generation as previous frame");
        return;
    }

    index = display_channel_get_video_stream_id(display, stream);
    for (dpi_link = stream->current->pipes; dpi_link; dpi_link = dpi_next) {
        RedDrawablePipeItem *dpi = dpi_link->data;
        dpi_next = dpi_link->next;
        dcc = dpi->dcc;
        agent = dcc_get_video_stream_agent(dcc, index);

        if (red_channel_client_pipe_item_is_linked(RED_CHANNEL_CLIENT(dcc),
                                                   &dpi->base)) {
#ifdef STREAM_STATS
            agent->stats.num_drops_pipe++;
#endif
            agent->video_encoder->notify_server_frame_drop(agent->video_encoder);
        }
    }
}

static VideoStream *display_channel_stream_try_new(DisplayChannel *display)
{
    VideoStream *stream;
    if (!display->priv->free_streams) {
        return NULL;
    }
    stream = display->priv->free_streams;
    display->priv->free_streams = display->priv->free_streams->next;
    return stream;
}

static void display_channel_create_stream(DisplayChannel *display, Drawable *drawable)
{
    DisplayChannelClient *dcc;
    VideoStream *stream;
    SpiceRect* src_rect;

    spice_assert(!drawable->stream);

    if (!(stream = display_channel_stream_try_new(display))) {
        return;
    }

    spice_assert(drawable->red_drawable->type == QXL_DRAW_COPY);
    src_rect = &drawable->red_drawable->u.copy.src_area;

    ring_add(&display->priv->streams, &stream->link);
    stream->current = drawable;
    stream->last_time = drawable->creation_time;
    stream->width = src_rect->right - src_rect->left;
    stream->height = src_rect->bottom - src_rect->top;
    stream->dest_area = drawable->red_drawable->bbox;
    stream->refs = 1;
    stream->top_down = spice_image_get_top_down(drawable->red_drawable->u.copy.src_bitmap);
    drawable->stream = stream;
    /* Provide an fps estimate the video encoder can use when initializing
     * based on the frames that lead to the creation of the stream. Round to
     * the nearest integer, for instance 24 for 23.976.
     */
    uint64_t duration = drawable->creation_time - drawable->first_frame_time;
    if (duration > NSEC_PER_SEC * drawable->frames_count / MAX_FPS) {
        stream->input_fps = (NSEC_PER_SEC * drawable->frames_count + duration / 2) / duration;
    } else {
        stream->input_fps = MAX_FPS;
    }
    stream->num_input_frames = 0;
    stream->input_fps_start_time = drawable->creation_time;
    display->priv->streams_size_total += stream->width * stream->height;
    display->priv->stream_count++;
    FOREACH_DCC(display, dcc) {
        dcc_create_stream(dcc, stream);
    }
    spice_debug("stream %d %dx%d (%d, %d) (%d, %d) %u fps",
                display_channel_get_video_stream_id(display, stream), stream->width,
                stream->height, stream->dest_area.left, stream->dest_area.top,
                stream->dest_area.right, stream->dest_area.bottom,
                stream->input_fps);
}

// returns whether a stream was created
static bool video_stream_add_frame(DisplayChannel *display,
                             Drawable *frame_drawable,
                             red_time_t first_frame_time,
                             int frames_count,
                             int gradual_frames_count,
                             int last_gradual_frame)
{
    update_copy_graduality(display, frame_drawable);
    frame_drawable->first_frame_time = first_frame_time;
    frame_drawable->frames_count = frames_count + 1;
    frame_drawable->gradual_frames_count  = gradual_frames_count;

    if (frame_drawable->copy_bitmap_graduality != BITMAP_GRADUAL_LOW) {
        if ((frame_drawable->frames_count - last_gradual_frame) >
            RED_STREAM_FRAMES_RESET_CONDITION) {
            frame_drawable->frames_count = 1;
            frame_drawable->gradual_frames_count = 1;
        } else {
            frame_drawable->gradual_frames_count++;
        }

        frame_drawable->last_gradual_frame = frame_drawable->frames_count;
    } else {
        frame_drawable->last_gradual_frame = last_gradual_frame;
    }

    if (is_stream_start(frame_drawable)) {
        display_channel_create_stream(display, frame_drawable);
        return TRUE;
    }
    return FALSE;
}

/* TODO: document the difference between the 2 functions below */
void video_stream_trace_update(DisplayChannel *display, Drawable *drawable)
{
    ItemTrace *trace;
    ItemTrace *trace_end;
    RingItem *item;

    if (drawable->stream || !drawable->streamable || drawable->frames_count) {
        return;
    }

    FOREACH_STREAMS(display, item) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        bool is_next_frame = is_next_stream_frame(drawable,
                                                  stream->width,
                                                  stream->height,
                                                  &stream->dest_area,
                                                  stream->last_time,
                                                  stream,
                                                  TRUE);
        if (is_next_frame) {
            if (stream->current) {
                stream->current->streamable = FALSE; //prevent item trace
                before_reattach_stream(display, stream, drawable);
                video_stream_detach_drawable(stream);
            }
            attach_stream(display, drawable, stream);
            return;
        }
    }

    trace = display->priv->items_trace;
    trace_end = trace + NUM_TRACE_ITEMS;
    for (; trace < trace_end; trace++) {
        if (is_next_stream_frame(drawable, trace->width, trace->height,
                                 &trace->dest_area, trace->time, NULL, FALSE)) {
            if (video_stream_add_frame(display, drawable,
                                       trace->first_frame_time,
                                       trace->frames_count,
                                       trace->gradual_frames_count,
                                       trace->last_gradual_frame)) {
                return;
            }
        }
    }
}

void video_stream_maintenance(DisplayChannel *display,
                              Drawable *candidate, Drawable *prev)
{
    bool is_next_frame;

    if (candidate->stream) {
        return;
    }

    if (prev->stream) {
        VideoStream *stream = prev->stream;

        is_next_frame = is_next_stream_frame(candidate,
                                             stream->width, stream->height,
                                             &stream->dest_area, stream->last_time,
                                             stream, TRUE);
        if (is_next_frame) {
            before_reattach_stream(display, stream, candidate);
            video_stream_detach_drawable(stream);
            prev->streamable = FALSE; //prevent item trace
            attach_stream(display, candidate, stream);
        }
    } else if (candidate->streamable) {
        SpiceRect* prev_src = &prev->red_drawable->u.copy.src_area;

        is_next_frame =
            is_next_stream_frame(candidate, prev_src->right - prev_src->left,
                                 prev_src->bottom - prev_src->top,
                                 &prev->red_drawable->bbox, prev->creation_time,
                                 prev->stream,
                                 FALSE);
        if (is_next_frame) {
            video_stream_add_frame(display, candidate,
                                   prev->first_frame_time,
                                   prev->frames_count,
                                   prev->gradual_frames_count,
                                   prev->last_gradual_frame);
        }
    }
}

static void dcc_update_streams_max_latency(DisplayChannelClient *dcc,
                                           VideoStreamAgent *remove_agent)
{
    uint32_t new_max_latency = 0;
    int i;

    if (dcc_get_max_stream_latency(dcc) != remove_agent->client_required_latency) {
        return;
    }

    dcc_set_max_stream_latency(dcc, 0);
    if (DCC_TO_DC(dcc)->priv->stream_count == 1) {
        return;
    }
    for (i = 0; i < NUM_STREAMS; i++) {
        VideoStreamAgent *other_agent = dcc_get_video_stream_agent(dcc, i);
        if (other_agent == remove_agent || !other_agent->video_encoder) {
            continue;
        }
        if (other_agent->client_required_latency > new_max_latency) {
            new_max_latency = other_agent->client_required_latency;
        }
    }
    dcc_set_max_stream_latency(dcc, new_max_latency);
}

static uint64_t get_initial_bit_rate(DisplayChannelClient *dcc, VideoStream *stream)
{
    char *env_bit_rate_str;
    uint64_t bit_rate = 0;

    env_bit_rate_str = getenv("SPICE_BIT_RATE");
    if (env_bit_rate_str != NULL) {
        double env_bit_rate;

        errno = 0;
        env_bit_rate = strtod(env_bit_rate_str, NULL);
        if (errno == 0) {
            bit_rate = env_bit_rate * 1024 * 1024;
        } else {
            spice_warning("error parsing SPICE_BIT_RATE: %s", strerror(errno));
        }
    }

    if (!bit_rate) {
        MainChannelClient *mcc;
        uint64_t net_test_bit_rate;

        mcc = red_client_get_main(red_channel_client_get_client(RED_CHANNEL_CLIENT(dcc)));
        net_test_bit_rate = main_channel_client_is_network_info_initialized(mcc) ?
                                main_channel_client_get_bitrate_per_sec(mcc) :
                                0;
        bit_rate = MAX(dcc_get_max_stream_bit_rate(dcc), net_test_bit_rate);
        if (bit_rate == 0) {
            /*
             * In case we are after a spice session migration,
             * the low_bandwidth flag is retrieved from migration data.
             * If the network info is not initialized due to another reason,
             * the low_bandwidth flag is FALSE.
             */
            bit_rate = dcc_is_low_bandwidth(dcc) ?
                RED_STREAM_DEFAULT_LOW_START_BIT_RATE :
                RED_STREAM_DEFAULT_HIGH_START_BIT_RATE;
        }
    }

    spice_debug("base-bit-rate %.2f (Mbps)", bit_rate / 1024.0 / 1024.0);
    /* dividing the available bandwidth among the active streams, and saving
     * (1-RED_STREAM_CHANNEL_CAPACITY) of it for other messages */
    return (RED_STREAM_CHANNEL_CAPACITY * bit_rate *
            stream->width * stream->height) / DCC_TO_DC(dcc)->priv->streams_size_total;
}

static uint32_t get_roundtrip_ms(void *opaque)
{
    VideoStreamAgent *agent = opaque;
    int roundtrip;
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(agent->dcc);

    roundtrip = red_channel_client_get_roundtrip_ms(rcc);
    if (roundtrip < 0) {
        MainChannelClient *mcc = red_client_get_main(red_channel_client_get_client(rcc));

        /*
         * the main channel client roundtrip might not have been
         * calculated (e.g., after migration). In such case,
         * main_channel_client_get_roundtrip_ms returns 0.
         */
        roundtrip = main_channel_client_get_roundtrip_ms(mcc);
    }

    return roundtrip;
}

static uint32_t get_source_fps(void *opaque)
{
    VideoStreamAgent *agent = opaque;

    return agent->stream->input_fps;
}

static void update_client_playback_delay(void *opaque, uint32_t delay_ms)
{
    VideoStreamAgent *agent = opaque;
    DisplayChannelClient *dcc = agent->dcc;
    RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(dcc));
    RedsState *reds = red_client_get_server(client);

    dcc_update_streams_max_latency(dcc, agent);

    agent->client_required_latency = delay_ms;
    if (delay_ms > dcc_get_max_stream_latency(dcc)) {
        dcc_set_max_stream_latency(dcc, delay_ms);
    }
    spice_debug("resetting client latency: %u", dcc_get_max_stream_latency(dcc));
    main_dispatcher_set_mm_time_latency(reds_get_main_dispatcher(reds),
                                        client,
                                        dcc_get_max_stream_latency(agent->dcc));
}

static void bitmap_ref(gpointer data)
{
    RedDrawable *red_drawable = (RedDrawable*)data;
    red_drawable_ref(red_drawable);
}

static void bitmap_unref(gpointer data)
{
    RedDrawable *red_drawable = (RedDrawable*)data;
    red_drawable_unref(red_drawable);
}

/* A helper for dcc_create_stream(). */
static VideoEncoder* dcc_create_video_encoder(DisplayChannelClient *dcc,
                                              uint64_t starting_bit_rate,
                                              VideoEncoderRateControlCbs *cbs)
{
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(dcc);
    bool client_has_multi_codec = red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_MULTI_CODEC);
    int i;
    GArray *video_codecs;

    video_codecs = dcc_get_preferred_video_codecs_for_encoding(dcc);
    for (i = 0; i < video_codecs->len; i++) {
        RedVideoCodec* video_codec = &g_array_index (video_codecs, RedVideoCodec, i);

        if (!client_has_multi_codec &&
            video_codec->type != SPICE_VIDEO_CODEC_TYPE_MJPEG) {
            /* Old clients only support MJPEG */
            continue;
        }
        if (client_has_multi_codec &&
            !red_channel_client_test_remote_cap(rcc, video_codec->cap)) {
            /* The client is recent but does not support this codec */
            continue;
        }

        VideoEncoder* video_encoder = video_codec->create(video_codec->type, starting_bit_rate, cbs, bitmap_ref, bitmap_unref);
        if (video_encoder) {
            return video_encoder;
        }
    }

    /* Try to use the builtin MJPEG video encoder as a fallback */
    if (!client_has_multi_codec || red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_CODEC_MJPEG)) {
        return mjpeg_encoder_new(SPICE_VIDEO_CODEC_TYPE_MJPEG, starting_bit_rate, cbs, bitmap_ref, bitmap_unref);
    }

    return NULL;
}

void dcc_create_stream(DisplayChannelClient *dcc, VideoStream *stream)
{
    int stream_id = display_channel_get_video_stream_id(DCC_TO_DC(dcc), stream);
    VideoStreamAgent *agent = dcc_get_video_stream_agent(dcc, stream_id);

    spice_return_if_fail(region_is_empty(&agent->vis_region));

    if (stream->current) {
        region_clone(&agent->vis_region, &stream->current->tree_item.base.rgn);
        region_clone(&agent->clip, &agent->vis_region);
    }
    agent->fps = MAX_FPS;
    agent->dcc = dcc;

    VideoEncoderRateControlCbs video_cbs;
    video_cbs.opaque = agent;
    video_cbs.get_roundtrip_ms = get_roundtrip_ms;
    video_cbs.get_source_fps = get_source_fps;
    video_cbs.update_client_playback_delay = update_client_playback_delay;

    uint64_t initial_bit_rate = get_initial_bit_rate(dcc, stream);
    agent->video_encoder = dcc_create_video_encoder(dcc, initial_bit_rate, &video_cbs);
    red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), video_stream_create_item_new(agent));

    if (red_channel_client_test_remote_cap(RED_CHANNEL_CLIENT(dcc), SPICE_DISPLAY_CAP_STREAM_REPORT)) {
        RedStreamActivateReportItem *report_pipe_item = g_new0(RedStreamActivateReportItem, 1);

        agent->report_id = rand();
        red_pipe_item_init(&report_pipe_item->base,
                           RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT);
        report_pipe_item->stream_id = stream_id;
        report_pipe_item->report_id = agent->report_id;
        red_channel_client_pipe_add(RED_CHANNEL_CLIENT(dcc), &report_pipe_item->base);
    }
#ifdef STREAM_STATS
    memset(&agent->stats, 0, sizeof(StreamStats));
    if (stream->current) {
        agent->stats.start = stream->current->red_drawable->mm_time;
    }
#endif
}

void video_stream_agent_stop(VideoStreamAgent *agent)
{
    DisplayChannelClient *dcc = agent->dcc;

    dcc_update_streams_max_latency(dcc, agent);
    if (agent->video_encoder) {
        agent->video_encoder->destroy(agent->video_encoder);
        agent->video_encoder = NULL;
    }
}

static void red_upgrade_item_free(RedPipeItem *base)
{
    g_return_if_fail(base != NULL);

    RedUpgradeItem *item = SPICE_UPCAST(RedUpgradeItem, base);

    g_return_if_fail(item->base.refcount == 0);

    drawable_unref(item->drawable);
    g_free(item->rects);
    g_free(item);
}

/*
 * after dcc_detach_stream_gracefully is called for all the display channel clients,
 * video_stream_detach_drawable should be called. See comment (1).
 */
static void dcc_detach_stream_gracefully(DisplayChannelClient *dcc,
                                         VideoStream *stream,
                                         Drawable *update_area_limit)
{
    DisplayChannel *display = DCC_TO_DC(dcc);
    int stream_id = display_channel_get_video_stream_id(display, stream);
    VideoStreamAgent *agent = dcc_get_video_stream_agent(dcc, stream_id);

    /* stopping the client from playing older frames at once*/
    region_clear(&agent->clip);
    dcc_video_stream_agent_clip(dcc, agent);

    if (region_is_empty(&agent->vis_region)) {
        spice_debug("stream %d: vis region empty", stream_id);
        return;
    }

    if (stream->current &&
        region_contains(&stream->current->tree_item.base.rgn, &agent->vis_region)) {
        RedChannelClient *rcc;
        RedUpgradeItem *upgrade_item;
        int n_rects;

        /* (1) The caller should detach the drawable from the stream. This will
         * lead to sending the drawable losslessly, as an ordinary drawable. */
        if (dcc_drawable_is_in_pipe(dcc, stream->current)) {
            spice_debug("stream %d: upgrade by linked drawable. box ==>",
                        stream_id);
            rect_debug(&stream->current->red_drawable->bbox);
            goto clear_vis_region;
        }
        spice_debug("stream %d: upgrade by drawable. box ==>", stream_id);
        rect_debug(&stream->current->red_drawable->bbox);
        rcc = RED_CHANNEL_CLIENT(dcc);
        upgrade_item = g_new(RedUpgradeItem, 1);
        red_pipe_item_init_full(&upgrade_item->base, RED_PIPE_ITEM_TYPE_UPGRADE,
                                red_upgrade_item_free);
        upgrade_item->drawable = stream->current;
        upgrade_item->drawable->refs++;
        n_rects = pixman_region32_n_rects(&upgrade_item->drawable->tree_item.base.rgn);
        upgrade_item->rects = g_malloc(sizeof(SpiceClipRects) + n_rects * sizeof(SpiceRect));
        upgrade_item->rects->num_rects = n_rects;
        region_ret_rects(&upgrade_item->drawable->tree_item.base.rgn,
                         upgrade_item->rects->rects, n_rects);
        red_channel_client_pipe_add(rcc, &upgrade_item->base);

    } else {
        SpiceRect upgrade_area;

        region_extents(&agent->vis_region, &upgrade_area);
        spice_debug("stream %d: upgrade by screenshot. has current %d. box ==>",
                    stream_id, stream->current != NULL);
        rect_debug(&upgrade_area);
        if (update_area_limit) {
            display_channel_draw_until(display, &upgrade_area, 0, update_area_limit);
        } else {
            display_channel_draw(display, &upgrade_area, 0);
        }
        dcc_add_surface_area_image(dcc, 0, &upgrade_area, NULL, FALSE);
    }
clear_vis_region:
    region_clear(&agent->vis_region);
}

static void detach_video_stream_gracefully(DisplayChannel *display,
                                           VideoStream *stream,
                                           Drawable *update_area_limit)
{
    DisplayChannelClient *dcc;

    FOREACH_DCC(display, dcc) {
        dcc_detach_stream_gracefully(dcc, stream, update_area_limit);
    }
    if (stream->current) {
        video_stream_detach_drawable(stream);
    }
}

/*
 * region  : a primary surface region. Streams that intersects with the given
 *           region will be detached.
 * drawable: If detaching the stream is triggered by the addition of a new drawable
 *           that is dependent on the given region, and the drawable is already a part
 *           of the "current tree", the drawable parameter should be set with
 *           this drawable, otherwise, it should be NULL. Then, if detaching the stream
 *           involves sending an upgrade image to the client, this drawable won't be rendered
 *           (see dcc_detach_stream_gracefully).
 */
void video_stream_detach_behind(DisplayChannel *display,
                                QRegion *region,
                                Drawable *drawable)
{
    Ring *ring = &display->priv->streams;
    RingItem *item = ring_get_head(ring);
    DisplayChannelClient *dcc;
    bool is_connected = red_channel_is_connected(RED_CHANNEL(display));

    while (item) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        int detach = 0;
        item = ring_next(ring, item);
        int stream_id = display_channel_get_video_stream_id(display, stream);

        FOREACH_DCC(display, dcc) {
            VideoStreamAgent *agent = dcc_get_video_stream_agent(dcc, stream_id);

            if (region_intersects(&agent->vis_region, region)) {
                dcc_detach_stream_gracefully(dcc, stream, drawable);
                detach = 1;
                spice_debug("stream %d", stream_id);
            }
        }
        if (detach && stream->current) {
            video_stream_detach_drawable(stream);
        } else if (!is_connected) {
            if (stream->current &&
                region_intersects(&stream->current->tree_item.base.rgn, region)) {
                video_stream_detach_drawable(stream);
            }
        }
    }
}

void video_stream_detach_and_stop(DisplayChannel *display)
{
    RingItem *stream_item;

    spice_debug("trace");
    while ((stream_item = ring_get_head(&display->priv->streams))) {
        VideoStream *stream = SPICE_CONTAINEROF(stream_item, VideoStream, link);

        detach_video_stream_gracefully(display, stream, NULL);
        video_stream_stop(display, stream);
    }
}

void video_stream_timeout(DisplayChannel *display)
{
    Ring *ring = &display->priv->streams;
    RingItem *item;

    red_time_t now = spice_get_monotonic_time_ns();
    item = ring_get_head(ring);
    while (item) {
        VideoStream *stream = SPICE_CONTAINEROF(item, VideoStream, link);
        item = ring_next(ring, item);
        if (now >= (stream->last_time + RED_STREAM_TIMEOUT)) {
            detach_video_stream_gracefully(display, stream, NULL);
            video_stream_stop(display, stream);
        }
    }
}

void video_stream_trace_add_drawable(DisplayChannel *display,
                                     Drawable *item)
{
    ItemTrace *trace;

    if (item->stream || !item->streamable) {
        return;
    }

    trace = &display->priv->items_trace[display->priv->next_item_trace++ & ITEMS_TRACE_MASK];
    trace->time = item->creation_time;
    trace->first_frame_time = item->first_frame_time;
    trace->frames_count = item->frames_count;
    trace->gradual_frames_count = item->gradual_frames_count;
    trace->last_gradual_frame = item->last_gradual_frame;
    SpiceRect* src_area = &item->red_drawable->u.copy.src_area;
    trace->width = src_area->right - src_area->left;
    trace->height = src_area->bottom - src_area->top;
    trace->dest_area = item->red_drawable->bbox;
}
