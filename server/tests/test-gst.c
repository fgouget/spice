/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2016 Red Hat, Inc.

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
/* Utility to check video encoder code
 * (see program_description below)
 */
#undef NDEBUG
#include <config.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <common/log.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "video-encoder.h"
#include "spice-bitmap-utils.h"
#include "reds.h" // reds_get_mm_time

static const char program_description[] =
    "2 GStreamer plugins are used in a chain like this:\n"
    "    (1) input pipeline -> (2) video encoder -> (3) output pipeline\n"
    "While converting output from (1) is compared with output of (3)\n"
    "making sure the streaming is working correctly.\n"
    "\n"
    "As an example you can use a command like\n"
    "\n"
    "  $ ./test-gst -e gstreamer:vp8 -i \\\n"
    "    'filesrc location=bbb_sunflower_1080p_30fps_normal.mp4 \\\n"
    "    ! decodebin ! videoconvert'\n"
    "\n"
    "to check vp8 encoding.";

// clipping informations passed in command line
typedef enum {
    COORDS_INVALID,
    COORDS_NUMBER,
    COORDS_PERCENT,
} CoordsUnit;
static struct {
    unsigned int value;
    CoordsUnit unit;
} clipping_coords[4];
enum {
    COORDS_BOX,
    COORDS_SIZE
} clipping_type;

typedef struct {
    gint refs;
    SpiceImage *image;
} TestFrame;

#ifdef HAVE_GSTREAMER_0_10

#define VIDEOCONVERT "ffmpegcolorspace"
#define BGRx_CAPS "caps=video/x-raw-rgb,bpp=32,depth=24,blue_mask=-16777216,green_mask=16711680,red_mask=65280"

typedef GstBuffer GstSample;
#define gst_sample_get_buffer(s) (s)
#define gst_sample_get_caps(s) GST_BUFFER_CAPS(s)
#define gst_sample_unref(s) gst_buffer_unref(s)
#define gst_app_sink_pull_sample(s) gst_app_sink_pull_buffer(s)
typedef struct {
    uint8_t *data;
} GstMapInfo;
#define GST_MAP_READ 1
static void
gst_buffer_unmap(GstBuffer *buffer, GstMapInfo *mapinfo)
{ }

static gboolean
gst_buffer_map(GstBuffer *buffer, GstMapInfo *mapinfo, int flags)
{
    mapinfo->data = GST_BUFFER_DATA(buffer);
    return mapinfo->data != NULL;
}

static GstBuffer*
gst_buffer_new_wrapped_full(int flags SPICE_GNUC_UNUSED, gpointer data, gsize maxsize,
                            gsize offset, gsize size,
                            gpointer user_data, GDestroyNotify notify)
{
    GstBuffer *buffer = gst_buffer_new();

    buffer->malloc_data = user_data;
    GST_BUFFER_FREE_FUNC(buffer) = notify;
    GST_BUFFER_DATA(buffer) = (uint8_t *) data + offset;
    GST_BUFFER_SIZE(buffer) = size;

    return buffer;
}

#define GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS 0

#define gst_bus_set_sync_handler(bus, proc, param, destroy) G_STMT_START {\
    SPICE_VERIFY(destroy == NULL); \
    gst_bus_set_sync_handler(bus, proc, param); \
} G_STMT_END
#else
#define VIDEOCONVERT "videoconvert"
#define BGRx_CAPS "caps=video/x-raw,format=BGRx"
#endif

typedef void (*SampleProc)(GstSample *sample, void *param);

typedef struct {
    GstAppSrc *appsrc;
    GstAppSink *appsink;
    GstElement *gst_pipeline;
    SampleProc sample_proc;
    void *sample_param;
    gboolean got_eos;
} TestPipeline;

typedef struct {
    const char *name;
    new_video_encoder_t new_encoder;
    SpiceVideoCodecType coded_type;
    // see spice-gtk channel-display-gst.c
    const char *caps;
    const char *decoder;
} EncoderInfo;

// our video encoder we are testing
static VideoEncoder *video_encoder = NULL;

// image settings
static gboolean top_down = FALSE;
static SpiceBitmapFmt bitmap_format = SPICE_BITMAP_FMT_32BIT;
static gint image_split_lines = 60000;

static gboolean clipping_type_computed = FALSE;
static SpiceRect clipping_rect;
static pthread_mutex_t frame_queue_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_queue_cond = PTHREAD_COND_INITIALIZER;
static GQueue frame_queue = G_QUEUE_INIT;
// input frames are counted
static unsigned input_frame_index = 0;
// file output for report informations like
// frame output size
static FILE *file_report;
static TestPipeline *input_pipeline, *output_pipeline;
static pthread_mutex_t eos_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t eos_cond = PTHREAD_COND_INITIALIZER;
// minimum image different expected, depends on quality
// and encoder
static gdouble minimum_psnr = 25;
static uint64_t starting_bit_rate = 3000000;

static void compute_clipping_rect(GstSample *sample);
static void parse_clipping(const char *clipping);
static TestFrame *gst_to_spice_frame(GstSample *sample);
static void image_free(SpiceImage *image);
static void frame_ref(TestFrame *frame);
static void frame_unref(TestFrame *frame);
static void pipeline_free(TestPipeline *pipeline);
static void pipeline_send_raw_data(TestPipeline *pipeline, VideoBuffer *buffer);
static void pipeline_wait_eos(TestPipeline *pipeline);
static void create_input_pipeline(const char *input_pipeline,
                                  SampleProc sample_proc, void *param);
static void create_output_pipeline(const EncoderInfo *encoder,
                                   SampleProc output_frames, void *param);
static void create_video_encoder(const EncoderInfo *encoder);
static const EncoderInfo *get_encoder_info(const char *encoder_name);
static SpiceBitmapFmt get_bitmap_format(const char *format);
static double compute_psnr(SpiceBitmap *bitmap1, int32_t x1, int32_t y1,
                           SpiceBitmap *bitmap2, int32_t x2, int32_t y2,
                           int32_t w, int32_t h);

// handle output frames from input pipeline
static void
input_frames(GstSample *sample, void *param)
{
    unsigned curr_frame_index = input_frame_index++;

    spice_assert(video_encoder && sample);

    if (SPICE_UNLIKELY(!clipping_type_computed)) {
        compute_clipping_rect(sample);
    }

    VideoBuffer *p_outbuf = NULL;
    // TODO correct ?? emulate another timer ??
    uint32_t frame_mm_time = reds_get_mm_time();

    // convert frame to SpiceBitmap/DRM prime
    TestFrame *frame = gst_to_spice_frame(sample);

    // send frame to our video encoder (must be from a single thread)
    int res = video_encoder->encode_frame(video_encoder, frame_mm_time, frame->image,
                                          &clipping_rect, top_down, frame,
                                          &p_outbuf);
    switch (res) {
    case VIDEO_ENCODER_FRAME_ENCODE_DONE:
        // save frame into queue for comparison later
        frame_ref(frame);
        pthread_mutex_lock(&frame_queue_mtx);
        g_queue_push_tail(&frame_queue, frame);
        while (g_queue_get_length(&frame_queue) >= 16) {
            pthread_cond_wait(&frame_queue_cond, &frame_queue_mtx);
        }
        pthread_mutex_unlock(&frame_queue_mtx);
        spice_assert(p_outbuf);
        pipeline_send_raw_data(output_pipeline, p_outbuf);
        if (file_report) {
            fprintf(file_report,
                    "Frame: %u\n"
                    "Output size: %u\n",
                    curr_frame_index,
                    (unsigned) p_outbuf->size);
        }
        break;
    case VIDEO_ENCODER_FRAME_UNSUPPORTED:
        // ?? what to do ??
        // should not happen, format passed should be supported
        // could happen for serious problems and encoder gave up
        spice_assert(0);
        break;
    case VIDEO_ENCODER_FRAME_DROP:
        if (file_report) {
            fprintf(file_report,
                    "Frame: %u\n"
                    "Output size: 0\n",
                    curr_frame_index);
        }
        break;
    default:
        // invalid value returned
        spice_assert(0);
    }

    // TODO call client_stream_report to simulate this report from the client

    frame_unref(frame);
}

// handle output frames from output pipeline
static void
output_frames(GstSample *sample, void *param)
{
    TestFrame *curr_frame = gst_to_spice_frame(sample);

    // TODO support DRM prime in the future
    spice_assert(curr_frame->image->descriptor.type == SPICE_IMAGE_TYPE_BITMAP);

    // get first frame queued
    pthread_mutex_lock(&frame_queue_mtx);
    TestFrame *expected_frame = g_queue_pop_head(&frame_queue);
    pthread_cond_signal(&frame_queue_cond);
    pthread_mutex_unlock(&frame_queue_mtx);
    if (!expected_frame) {
        g_printerr("Frame not present in the queue but arrived in output!\n");
        exit(1);
    }

    // TODO try to understand if this is correct
    if (!top_down) {
        curr_frame->image->u.bitmap.flags ^= SPICE_BITMAP_FLAGS_TOP_DOWN;
    }
#ifdef DUMP_BITMAP
    dump_bitmap(&expected_frame->image->u.bitmap);
    dump_bitmap(&curr_frame->image->u.bitmap);
#endif

    // compute difference
    double psnr = compute_psnr(&expected_frame->image->u.bitmap,
                               clipping_rect.left, clipping_rect.top,
                               &curr_frame->image->u.bitmap, 0, 0,
                               clipping_rect.right - clipping_rect.left,
                               clipping_rect.bottom - clipping_rect.top);

    // check is more or less the same
    if (psnr < minimum_psnr) {
        g_printerr("Frame PSNR too low, got %g minimum %g\n", psnr, minimum_psnr);
        exit(1);
    }

    frame_unref(expected_frame);
    frame_unref(curr_frame);
}

static const EncoderInfo encoder_infos[] = {
    { "mjpeg", mjpeg_encoder_new, SPICE_VIDEO_CODEC_TYPE_MJPEG,
      "caps=image/jpeg", "jpegdec" },
#define encoder_info_mjpeg (&encoder_infos[0])
    { "gstreamer:mjpeg", gstreamer_encoder_new, SPICE_VIDEO_CODEC_TYPE_MJPEG,
      "caps=image/jpeg", "jpegdec" },
    { "gstreamer:vp8",   gstreamer_encoder_new, SPICE_VIDEO_CODEC_TYPE_VP8,
      "caps=video/x-vp8", "vp8dec" },
    { "gstreamer:vp9",   gstreamer_encoder_new, SPICE_VIDEO_CODEC_TYPE_VP9,
      "caps=video/x-vp9", "vp9dec" },
    { "gstreamer:h264",  gstreamer_encoder_new, SPICE_VIDEO_CODEC_TYPE_H264,
#ifdef HAVE_GSTREAMER_0_10
      "", "h264parse ! ffdec_h264" },
#else
      "", "h264parse ! avdec_h264" },
#endif
    { NULL, NULL, SPICE_VIDEO_CODEC_TYPE_ENUM_END, NULL, NULL }
};

int main(int argc, char *argv[])
{
    gchar *input_pipeline_desc = NULL;
    gchar *image_format = NULL;
    gchar *encoder_name = NULL;
    gchar *file_report_name = NULL;
    gboolean use_hw_encoder = FALSE; // TODO use
    gchar *clipping = NULL;

    // - input pipeline
    // - top/down
    // - format for video encoder input (bits, rgb/bgr)
    // - encoder (mjpeg/vp8/h264)
    // - use h/w acceleration (if available)
    // - clipping (part of the source)
    // - TODO bandwidth changes?
    // - TODO fps ??
    GOptionEntry entries[] = {
        { "input-pipeline", 'i', 0, G_OPTION_ARG_STRING, &input_pipeline_desc,
          "GStreamer input pipeline", "PIPELINE" },
        { "top-down", 0, 0, G_OPTION_ARG_NONE, &top_down,
          "Image encoded as top-down", NULL },
        { "format", 'f', 0, G_OPTION_ARG_STRING, &image_format,
          "Image format (16BIT/24BIT/32BIT/RGBA)", "FMT" },
        { "encoder", 'e', 0, G_OPTION_ARG_STRING, &encoder_name,
          "Encoder to use", "ENC" },
        { "use-hw-encoder", 0, 0, G_OPTION_ARG_NONE, &use_hw_encoder,
          "Use H/W encoders if possible", NULL },
        { "clipping", 0, 0, G_OPTION_ARG_STRING, &clipping,
          "Clipping region (x1,y1)-(x2,y2) or (x,y)x(w,h). "
          "You can specify coordinates using pixel or percentage.", "STRING" },
        { "starting-bitrate", 0, 0, G_OPTION_ARG_INT64, &starting_bit_rate,
          "Initial bitrate", "BITRATE" },
        { "min-psnr", 0, 0, G_OPTION_ARG_DOUBLE, &minimum_psnr,
          "Minimum PSNR accepted", "PSNR" },
        { "split-lines", 0, 0, G_OPTION_ARG_INT, &image_split_lines,
          "Split image into different chunks every LINES lines", "LINES" },
        { "report", 0, 0, G_OPTION_ARG_FILENAME, &file_report_name,
          "Report statistics to file", "FILENAME" },
        { NULL }
    };

    GOptionContext *context = NULL;
    GError *error = NULL;
    context = g_option_context_new("- helper for testing VideoEncoder");
    g_option_context_set_description(context, program_description);
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(1);
    }

    if (!input_pipeline_desc) {
        g_printerr("Input pipeline option missing\n");
        exit(1);
    }

    const EncoderInfo *encoder = encoder_info_mjpeg;
    if (encoder_name != NULL) {
        encoder = get_encoder_info(encoder_name);
        if (!encoder) {
            g_printerr("Encoder name unsupported: %s\n", encoder_name);
            exit(1);
        }
    }

    if (image_format != NULL) {
        bitmap_format = get_bitmap_format(image_format);
        if (bitmap_format == SPICE_BITMAP_FMT_INVALID) {
            g_printerr("Invalid image format: %s\n", image_format);
            exit(1);
        }
    }

    parse_clipping(clipping ? clipping : "(0,0)x(100%,100%)");

    if (minimum_psnr < 0) {
        g_printerr("Invalid PSNR specified %f\n", minimum_psnr);
        exit(1);
    }

    if (image_split_lines < 1) {
        g_printerr("Invalid --split-lines option: %d\n", image_split_lines);
        exit(1);
    }

    if (file_report_name) {
        file_report = fopen(file_report_name, "w");
        if (!file_report) {
            g_printerr("Error opening file %s for report\n", file_report_name);
            exit(1);
        }
    }

    gst_init(&argc, &argv);

    // TODO give particular error if pipeline fails to be created

    create_output_pipeline(encoder, output_frames, NULL);

    create_video_encoder(encoder);

    create_input_pipeline(input_pipeline_desc, input_frames, NULL);

    // run all input streaming
    pipeline_wait_eos(input_pipeline);

    video_encoder->destroy(video_encoder);

    // send EOS to output and wait
    // this assure we processed all frames sent from input pipeline
    if (gst_app_src_end_of_stream(output_pipeline->appsrc) != GST_FLOW_OK) {
        g_printerr("gst_app_src_end_of_stream failed\n");
        exit(1);
    }
    pipeline_wait_eos(output_pipeline);

    // check queue is now empty
    pthread_mutex_lock(&frame_queue_mtx);
    TestFrame *frame = g_queue_pop_head(&frame_queue);
    pthread_mutex_unlock(&frame_queue_mtx);
    if (frame) {
        g_printerr("Queue not empty at the end\n");
        exit(1);
    }

    pipeline_free(input_pipeline);
    pipeline_free(output_pipeline);

    g_free(encoder_name);
    g_free(image_format);
    g_free(input_pipeline_desc);
    g_free(clipping);
    g_option_context_free(context);

    return 0;
}

static void
parse_clipping(const char *clipping)
{
    spice_assert(clipping);

#define NUM_FMT "%31[^,)]"
#define NUM(n) coords[n]
    char coords[4][32];
    char clipping_type_sign[2];
    int i;

    if (sscanf(clipping, "(" NUM_FMT "," NUM_FMT ")%1[x-](" NUM_FMT "," NUM_FMT ")",
               NUM(0), NUM(1), clipping_type_sign, NUM(2), NUM(3)) < 5) {
        goto format_error;
    }
    for (i = 0; i < 4; ++i) {
        char *end = NULL;
        errno = 0;
        clipping_coords[i].unit = COORDS_NUMBER;
        clipping_coords[i].value = strtoul(coords[i], &end, 10);
        if (errno || !end || (strcmp(end, "") != 0 && strcmp(end, "%") != 0)) {
            goto format_error;
        }
        if (strcmp(end, "%") == 0) {
            clipping_coords[i].unit = COORDS_PERCENT;
            if (clipping_coords[i].value > 100) {
                goto format_error;
            }
        }
    }
    if (clipping_type_sign[0] == 'x') {
        clipping_type = COORDS_SIZE;
    } else {
        clipping_type = COORDS_BOX;
    }
    return;

format_error:
    g_printerr("Invalid clipping format: %s\n", clipping);
    exit(1);

}

static void
compute_clipping_rect(GstSample *sample)
{
    GstCaps *caps = gst_sample_get_caps(sample);
    spice_assert(caps);

    GstStructure *s = gst_caps_get_structure(caps, 0);
    spice_assert(s);

    gint width, height;
    spice_assert(gst_structure_get_int(s, "width", &width) &&
                 gst_structure_get_int(s, "height", &height));

    // transform from percent to pixel values
    int i;
    unsigned int coords[4];
    for (i = 0; i < 4; ++i) {
        unsigned int coord = coords[i] = clipping_coords[i].value;
        if (clipping_coords[i].unit != COORDS_PERCENT) {
            spice_assert(clipping_coords[i].unit == COORDS_NUMBER);
            continue;
        }
        coords[i] = coord * ((i&1) ? height : width) / 100;
    }

    // transform from sized to box
    if (clipping_type == COORDS_SIZE) {
        coords[2] += coords[0];
        coords[3] += coords[1];
    }

    // clip to sample
    coords[0] = MIN(coords[0], width);
    coords[1] = MIN(coords[1], height);
    coords[2] = MIN(coords[2], width);
    coords[3] = MIN(coords[3], height);

    // check coordinated are valid
    spice_assert(coords[0] < coords[2]);
    spice_assert(coords[1] < coords[3]);

    // set
    clipping_rect.left = coords[0];
    clipping_rect.top = coords[1];
    clipping_rect.right = coords[2];
    clipping_rect.bottom = coords[3];
    clipping_type_computed = TRUE;
}

static const EncoderInfo *
get_encoder_info(const char *encoder_name)
{
    const EncoderInfo *info;
    for (info = encoder_infos; info->name; ++info) {
        if (strcmp(info->name, encoder_name) == 0) {
            return info;
        }
    }
    return NULL;
}

static GstFlowReturn
new_sample(GstAppSink *gstappsink, gpointer test_pipeline)
{
    TestPipeline *pipeline = test_pipeline;

    GstSample *sample = gst_app_sink_pull_sample(pipeline->appsink);
    if (sample) {
        pipeline->sample_proc(sample, pipeline->sample_param);
        gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
}

static GstBusSyncReply
handle_pipeline_message(GstBus *bus, GstMessage *msg, gpointer test_pipeline)
{
    TestPipeline *pipeline = (TestPipeline *) test_pipeline;

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        pthread_mutex_lock(&eos_mtx);
        pipeline->got_eos = TRUE;
        pthread_cond_signal(&eos_cond);
        pthread_mutex_unlock(&eos_mtx);
    }
    return GST_BUS_PASS;
}

static TestPipeline*
create_pipeline(const char *desc, SampleProc sample_proc, void *param)
{
    TestPipeline *pipeline = g_new0(TestPipeline, 1);

    pipeline->sample_proc = sample_proc;
    pipeline->sample_param = param;

    GError *err = NULL;
    pipeline->gst_pipeline = gst_parse_launch_full(desc, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &err);
    if (!pipeline->gst_pipeline) {
        g_printerr("GStreamer error: %s\n", err->message);
        return NULL;
    }

    pipeline->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline->gst_pipeline), "src"));
    pipeline->appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline->gst_pipeline), "sink"));
    if (!pipeline->appsink) {
        g_printerr("Appsync not found in pipeline: %s\n", desc);
        return NULL;
    }

    static const GstAppSinkCallbacks appsink_cbs_template =
        { NULL, NULL, new_sample, ._gst_reserved={NULL} };
    GstAppSinkCallbacks appsink_cbs = appsink_cbs_template;
    gst_app_sink_set_callbacks(pipeline->appsink, &appsink_cbs, pipeline, NULL);

    GstBus *bus = gst_element_get_bus(pipeline->gst_pipeline);
    gst_bus_set_sync_handler(bus, handle_pipeline_message, pipeline, NULL);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline->gst_pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        g_printerr("GStreamer error: Unable to set the pipeline to the playing state.\n");
        exit(1);
    }

    return pipeline;
}

static void
pipeline_free(TestPipeline *pipeline)
{
    if (pipeline->appsrc) {
        gst_object_unref(pipeline->appsrc);
    }
    gst_object_unref(pipeline->appsink);
    gst_object_unref(pipeline->gst_pipeline);
    g_free(pipeline);
}

static void
create_output_pipeline(const EncoderInfo *encoder, SampleProc sample_proc, void *param)
{
    gchar *desc =
        g_strdup_printf("appsrc name=src is-live=true format=time max-bytes=0 block=true "
                        "%s ! %s ! " VIDEOCONVERT " ! appsink name=sink " BGRx_CAPS
                        " sync=false drop=false", encoder->caps, encoder->decoder);

    TestPipeline *pipeline = create_pipeline(desc, sample_proc, param);
    if (!pipeline) {
        g_printerr("Error creating output pipeline: %s\n", desc);
        exit(1);
    }
    g_free(desc);

    output_pipeline = pipeline;
}

static void
create_input_pipeline(const char *input_pipeline_desc, SampleProc sample_proc, void *param)
{
    gchar *desc =
        g_strdup_printf("%s ! appsink name=sink " BGRx_CAPS
                        " sync=false drop=false", input_pipeline_desc);

    TestPipeline *pipeline = create_pipeline(desc, sample_proc, param);
    g_free(desc);
    if (!pipeline) {
        // TODO specific error
        g_printerr("Error creating input pipeline\n");
        exit(1);
    }

    input_pipeline = pipeline;
}

static void
video_buffer_release(VideoBuffer *video_buffer)
{
    video_buffer->free(video_buffer);
}

static void
pipeline_send_raw_data(TestPipeline *pipeline, VideoBuffer *video_buffer)
{
    GstBuffer *buffer =
        gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_PHYSICALLY_CONTIGUOUS,
                                    video_buffer->data, video_buffer->size,
                                    0, video_buffer->size,
                                    video_buffer, (void (*)(void*)) video_buffer_release);

    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
#ifndef HAVE_GSTREAMER_0_10
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
#endif

    if (gst_app_src_push_buffer(pipeline->appsrc, buffer) != GST_FLOW_OK) {
        g_printerr("GStreamer error: unable to push frame of size %u\n", video_buffer->size);
        exit(1);
    }
}

static void
pipeline_wait_eos(TestPipeline *pipeline)
{
    pthread_mutex_lock(&eos_mtx);
    while (!pipeline->got_eos) {
        pthread_cond_wait(&eos_cond, &eos_mtx);
    }
    pthread_mutex_unlock(&eos_mtx);
}

static uint32_t
mock_get_roundtrip_ms(void *opaque)
{
    // TODO
    return 20;
}

static uint32_t
mock_get_source_fps(void *opaque)
{
    // TODO
    return 10;
}

static void
mock_update_client_playback_delay(void *opaque, uint32_t delay_ms)
{
    // TODO
}

static VideoEncoderRateControlCbs rate_control_cbs = {
    .get_roundtrip_ms = mock_get_roundtrip_ms,
    .get_source_fps = mock_get_source_fps,
    .update_client_playback_delay = mock_update_client_playback_delay,
};

static void
create_video_encoder(const EncoderInfo *encoder)
{
    spice_assert(encoder);

    video_encoder = encoder->new_encoder(encoder->coded_type, starting_bit_rate, &rate_control_cbs,
                                         (bitmap_ref_t) frame_ref, (bitmap_unref_t) frame_unref);
    // TODO return not supported error
    spice_assert(video_encoder);
}

static void
frame_ref(TestFrame *frame)
{
    g_atomic_int_inc(&frame->refs);
}

static void
frame_unref(TestFrame *frame)
{
    if (!g_atomic_int_dec_and_test(&frame->refs)) {
        return;
    }
    image_free(frame->image);
    g_free(frame);
}

static void
image_free(SpiceImage *image)
{
    if (!image) {
        return;
    }
    spice_assert(image->descriptor.type == SPICE_IMAGE_TYPE_BITMAP);
    SpiceBitmap *bitmap = &image->u.bitmap;
    spice_assert(!bitmap->palette);
    spice_assert(bitmap->data);
    spice_chunks_destroy(bitmap->data);
    g_free(image);
}

static SpiceChunks* chunks_alloc(uint32_t stride, uint32_t height, uint32_t split);
static uint8_t *bitmap_get_line(SpiceBitmap *bitmap, int y);
static uint32_t compute_stride(int width, SpiceBitmapFmt format);
typedef void convert_line_t(uint8_t *dest, const uint8_t *src, uint32_t width);
static convert_line_t convert_line16;
static convert_line_t convert_line24;
static convert_line_t convert_line32;
static convert_line_t *get_convert_line(SpiceBitmapFmt format);

static SpiceImage *
gst_to_spice_image(GstSample *sample)
{
    GstCaps *caps = gst_sample_get_caps(sample);
    spice_assert(caps);

    GstStructure *s = gst_caps_get_structure(caps, 0);
    spice_assert(s);

    gint width = 0, height = 0;
    spice_assert(gst_structure_get_int(s, "width", &width) &&
                 gst_structure_get_int(s, "height", &height));

    SpiceImage *image = g_new0(SpiceImage, 1);
    image->descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
    image->descriptor.width = width;
    image->descriptor.height = height;
    SpiceBitmap *bitmap = &image->u.bitmap;
    bitmap->format = bitmap_format;
    bitmap->flags = top_down ? SPICE_BITMAP_FLAGS_TOP_DOWN : 0;
    bitmap->x = width;
    bitmap->y = height;
    bitmap->stride = compute_stride(width, bitmap->format);
    bitmap->data = chunks_alloc(bitmap->stride, height, image_split_lines);

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo mapinfo;
    if (!gst_buffer_map(buffer, &mapinfo, GST_MAP_READ)) {
        spice_error("GStreamer error: could not map the buffer");
    }

    // convert image
    gint y;
    convert_line_t *convert_line = get_convert_line(bitmap->format);
    for (y = 0; y < height; ++y) {
        convert_line(bitmap_get_line(bitmap, y),
                     mapinfo.data + y * width * 4,
                     width);
    }
    gst_buffer_unmap(buffer, &mapinfo);
    // TODO should we unref buffer ??

    return image;
}

static uint32_t
compute_stride(int width, SpiceBitmapFmt format)
{
    spice_assert(width > 0);

    switch (format) {
    case SPICE_BITMAP_FMT_16BIT:
        return width * 2;
    case SPICE_BITMAP_FMT_24BIT:
        return width * 3;
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        return width * 4;
    default:
        break;
    }
    spice_assert(0);
    return 0;
}

static SpiceChunks*
chunks_alloc(uint32_t stride, uint32_t height, uint32_t split)
{
    spice_assert(stride && height && split);
    const uint32_t num_chunks = (height + split - 1u) / split;
    SpiceChunks *chunks = spice_malloc0(sizeof(SpiceChunks) + sizeof(SpiceChunk) * num_chunks);

    chunks->data_size = stride * height;
    chunks->num_chunks = num_chunks;
    chunks->flags = SPICE_CHUNKS_FLAGS_FREE;
    unsigned n;
    uint32_t allocated = 0;
    for (n = 0; n < num_chunks; ++n) {
        SpiceChunk *chunk = &chunks->chunk[n];
        uint32_t len = stride * split;
        spice_assert(chunks->data_size > allocated);
        len = MIN(len, chunks->data_size - allocated);
        chunk->data = spice_malloc0(len);
        chunk->len = len;
        allocated += len;
    }
    spice_assert(chunks->data_size == allocated);
    return chunks;
}

static uint8_t *
bitmap_get_line(SpiceBitmap *bitmap, int y)
{
    spice_assert(bitmap && y >= 0 && y < bitmap->y);
    if (!(bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        y = bitmap->y - y - 1;
    }
    const uint32_t stride = bitmap->stride;
    uint32_t pos = stride * y;
    SpiceChunk *chunk = &bitmap->data->chunk[0];
    while (pos >= chunk->len) {
        pos -= chunk->len;
        ++chunk;
        spice_assert(chunk < &bitmap->data->chunk[bitmap->data->num_chunks]);
    }
    spice_assert(pos + stride <= chunk->len);
    return chunk->data + pos;
}

static convert_line_t *
get_convert_line(SpiceBitmapFmt format)
{
    switch (format) {
    case SPICE_BITMAP_FMT_16BIT:
        return convert_line16;
    case SPICE_BITMAP_FMT_24BIT:
        return convert_line24;
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        return convert_line32;
    default:
        break;
    }
    spice_assert(0);
    return 0;
}

static void
convert_line16(uint8_t *dest, const uint8_t *src, uint32_t width)
{
    uint16_t *dest16 = SPICE_ALIGNED_CAST(uint16_t *, dest);
    for (; width; --width) {
        *dest16++ = (src[0] >> 3) | ((src[1] & 0xf8) << 2) | ((src[2] & 0xf8) << 7);
        src += 4;
    }
}

static void
convert_line24(uint8_t *dest, const uint8_t *src, uint32_t width)
{
    for (; width; --width) {
        *dest++ = *src++;
        *dest++ = *src++;
        *dest++ = *src++;
        ++src;
    }
}

static void
convert_line32(uint8_t *dest, const uint8_t *src, uint32_t width)
{
    for (; width; --width) {
        *dest++ = *src++;
        *dest++ = *src++;
        *dest++ = *src++;
        *dest++ = 0;
        ++src;
    }
}

static SpiceBitmapFmt
get_bitmap_format(const char *format)
{
    if (strcmp(format, "32BIT") == 0) {
        return SPICE_BITMAP_FMT_32BIT;
    } else if (strcmp(format, "24BIT") == 0) {
        return SPICE_BITMAP_FMT_24BIT;
    } else if (strcmp(format, "16BIT") == 0) {
        return SPICE_BITMAP_FMT_16BIT;
    } else if (strcmp(format, "RGBA") == 0) {
        return SPICE_BITMAP_FMT_RGBA;
    }
    return SPICE_BITMAP_FMT_INVALID;
}

static TestFrame *
gst_to_spice_frame(GstSample *sample)
{
    TestFrame *frame = g_new0(TestFrame, 1);
    frame->refs = 1;
    frame->image = gst_to_spice_image(sample);
    return frame;
}

static uint32_t
line_diff_rgb(const uint8_t *pixel1, const uint8_t *pixel2, uint32_t w)
{
    uint32_t diff_sum = 0;
    for (w *= 3; w; --w) {
        int diff = *pixel1 - *pixel2;
        diff_sum += diff * diff;
        ++pixel1;
        ++pixel2;
    }
    return diff_sum;
}

typedef uint8_t *bitmap_extract_rgb_line_t(SpiceBitmap *bitmap, uint8_t *buf,
                                           int32_t x, int32_t y, int32_t w);
static bitmap_extract_rgb_line_t *get_bitmap_extract(SpiceBitmapFmt format);
static bitmap_extract_rgb_line_t bitmap_extract16;
static bitmap_extract_rgb_line_t bitmap_extract24;
static bitmap_extract_rgb_line_t bitmap_extract32;

// compute PSNR
// see https://en.wikipedia.org/wiki/Peak_signal-to-noise_ratio
// higher is better (less data loosed)
// typical are 30-50
static double
compute_psnr(SpiceBitmap *bitmap1, int32_t x1, int32_t y1,
             SpiceBitmap *bitmap2, int32_t x2, int32_t y2,
             int32_t w, int32_t h)
{
    spice_assert(x1 >= 0 && y1 >= 0);
    spice_assert(x2 >= 0 && y2 >= 0);
    spice_assert(w > 0 && h > 0);
    spice_assert(x1 + w <= bitmap1->x);
    spice_assert(y1 + h <= bitmap1->y);
    spice_assert(x2 + w <= bitmap2->x);
    spice_assert(y2 + h <= bitmap2->y);

    int y;
    uint64_t diff_sum = 0;
    uint8_t pixels[2][w*3];
    bitmap_extract_rgb_line_t *extract1 = get_bitmap_extract(bitmap1->format);
    bitmap_extract_rgb_line_t *extract2 = get_bitmap_extract(bitmap2->format);
    for (y = 0; y < h; ++y) {
        uint8_t *line1 = extract1(bitmap1, pixels[0], x1, y1 + y, w);
        uint8_t *line2 = extract2(bitmap2, pixels[1], x2, y2 + y, w);
        diff_sum += line_diff_rgb(line1, line2, w);
    }

    double mse = (double) diff_sum / (w*h*3);
    double psnr = 10 * log10(255*255/mse);

    return psnr;
}

static bitmap_extract_rgb_line_t *
get_bitmap_extract(SpiceBitmapFmt format)
{
    switch (format) {
    case SPICE_BITMAP_FMT_16BIT:
        return bitmap_extract16;
    case SPICE_BITMAP_FMT_24BIT:
        return bitmap_extract24;
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        return bitmap_extract32;
    default:
        break;
    }
    spice_assert(0);
    return 0;
}

static uint8_t *
bitmap_extract24(SpiceBitmap *bitmap, uint8_t *buf, int32_t x, int32_t y, int32_t w)
{
    uint8_t *line = bitmap_get_line(bitmap, y) + x * 3;
    return line;
}

static uint8_t *
bitmap_extract32(SpiceBitmap *bitmap, uint8_t *buf, int32_t x, int32_t y, int32_t w)
{
    const uint8_t *line = bitmap_get_line(bitmap, y) + x * 4;
    uint8_t *dest = buf;
    for (; w; --w) {
        *dest++ = *line++;
        *dest++ = *line++;
        *dest++ = *line++;
        ++line;
    }
    return buf;
}

static uint8_t *
bitmap_extract16(SpiceBitmap *bitmap, uint8_t *buf, int32_t x, int32_t y, int32_t w)
{
    const uint16_t *line = SPICE_ALIGNED_CAST(const uint16_t *, bitmap_get_line(bitmap, y) + x * 2);
    uint8_t *dest = buf;
    for (; w; --w) {
        uint16_t pixel = *line++;
        uint8_t comp;
        comp = (pixel >> 0) & 0x1f;
        *dest++ = (comp << 3) | (comp >> 2);
        comp = (pixel >> 5) & 0x1f;
        *dest++ = (comp << 3) | (comp >> 2);
        comp = (pixel >> 10) & 0x1f;
        *dest++ = (comp << 3) | (comp >> 2);
    }
    return buf;
}
