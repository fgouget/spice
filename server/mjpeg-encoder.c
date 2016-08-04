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

#include <stdio.h>
#include <inttypes.h>
#include <jerror.h>
#include <jpeglib.h>

#include "red-common.h"
#include "video-encoder.h"
#include "utils.h"

#define MJPEG_MAX_FPS 25
#define MJPEG_MIN_FPS 1

#define MJPEG_QUALITY_SAMPLE_NUM 7
static const int mjpeg_quality_samples[MJPEG_QUALITY_SAMPLE_NUM] = {20, 30, 40, 50, 60, 70, 80};

#define MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH 10
#define MJPEG_IMPROVE_QUALITY_FPS_PERMISSIVE_TH 5

#define MJPEG_AVERAGE_SIZE_WINDOW 3

#define MJPEG_BIT_RATE_EVAL_MIN_NUM_FRAMES 3
#define MJPEG_LOW_FPS_RATE_TH 3

#define MJPEG_SERVER_STATUS_EVAL_FPS_INTERVAL 1
#define MJPEG_SERVER_STATUS_DOWNGRADE_DROP_FACTOR_TH 0.1

/*
 * acting on positive client reports only if enough frame mm time
 * has passed since the last bit rate change and the report.
 * time
 */
#define MJPEG_CLIENT_POSITIVE_REPORT_TIMEOUT 2000
#define MJPEG_CLIENT_POSITIVE_REPORT_STRICT_TIMEOUT 3000

#define MJPEG_ADJUST_FPS_TIMEOUT 500

/*
 * avoid interrupting the playback when there are temporary
 * incidents of instability (with respect to server and client drops)
 */
#define MJPEG_MAX_CLIENT_PLAYBACK_DELAY (MSEC_PER_SEC * 5)

/*
 * The stream starts after lossless frames were sent to the client,
 * and without rate control (except for pipe congestion). Thus, on the beginning
 * of the stream, we might observe frame drops on the client and server side which
 * are not necessarily related to mis-estimation of the bit rate, and we would
 * like to wait till the stream stabilizes.
 */
#define MJPEG_WARMUP_TIME (NSEC_PER_SEC * 3)

/* The compressed buffer initial size. */
#define MJPEG_INITIAL_BUFFER_SIZE (32 * 1024)

enum {
    MJPEG_QUALITY_EVAL_TYPE_SET,
    MJPEG_QUALITY_EVAL_TYPE_UPGRADE,
    MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE,
};

enum {
    MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
    MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE,
};

typedef struct MJpegEncoderQualityEval {
    int type;
    int reason;

    uint64_t encoded_size_by_quality[MJPEG_QUALITY_SAMPLE_NUM];
    /* lower limit for the current evaluation round */
    int min_quality_id;
    int min_quality_fps; // min fps for the given quality
    /* upper limit for the current evaluation round */
    int max_quality_id;
    int max_quality_fps; // max fps for the given quality
    /* tracking the best sampled fps so far */
    int max_sampled_fps;
    int max_sampled_fps_quality_id;
} MJpegEncoderQualityEval;

typedef struct MJpegEncoderClientState {
    int max_video_latency;
    uint32_t max_audio_latency;
} MJpegEncoderClientState;

typedef struct MJpegEncoderServerState {
    uint32_t num_frames_encoded;
    uint32_t num_frames_dropped;
} MJpegEncoderServerState;

typedef struct MJpegEncoderBitRateInfo {
    uint64_t change_start_time;
    uint64_t last_frame_time;
    uint32_t change_start_mm_time;
    int was_upgraded;

    /* gathering data about the frames that
     * were encoded since the last bit rate change*/
    uint32_t num_enc_frames;
    uint64_t sum_enc_size;
} MJpegEncoderBitRateInfo;

/*
 * Adjusting the stream jpeg quality and frame rate (fps):
 * When during_quality_eval=TRUE, we compress different frames with different
 * jpeg quality. By considering (1) the resulting compression ratio, and (2) the available
 * bit rate, we evaluate the max frame frequency for the stream with the given quality,
 * and we choose the highest quality that will allow a reasonable frame rate.
 * during_quality_eval is set for new streams and can also be set any time we want
 * to re-evaluate the stream parameters (e.g., when the bit rate and/or
 * compressed frame size significantly change).
 */
typedef struct MJpegEncoderRateControl {
    int during_quality_eval;
    MJpegEncoderQualityEval quality_eval_data;
    MJpegEncoderBitRateInfo bit_rate_info;
    MJpegEncoderClientState client_state;
    MJpegEncoderServerState server_state;

    uint64_t byte_rate;
    int quality_id;
    uint32_t fps;
    double adjusted_fps;
    uint64_t adjusted_fps_start_time;
    uint64_t adjusted_fps_num_frames;

    /* the encoded frame size which the quality and the fps evaluation was based upon */
    uint64_t base_enc_size;

    uint64_t last_enc_size;

    uint64_t sum_recent_enc_size;
    uint32_t num_recent_enc_frames;

    uint64_t warmup_start_time;
} MJpegEncoderRateControl;

typedef struct MJpegVideoBuffer {
    VideoBuffer base;
    size_t maxsize;
} MJpegVideoBuffer;

typedef struct MJpegEncoder {
    VideoEncoder base;
    uint8_t *row;
    uint32_t row_size;
    int first_frame;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    unsigned int bytes_per_pixel; /* bytes per pixel of the input buffer */
    void (*pixel_converter)(void *src, uint8_t *dest);

    MJpegEncoderRateControl rate_control;
    VideoEncoderRateControlCbs cbs;

    /* stats */
    uint64_t starting_bit_rate;
    uint64_t avg_quality;
    uint32_t num_frames;
} MJpegEncoder;

static void mjpeg_encoder_process_server_drops(MJpegEncoder *encoder);
static uint32_t get_min_required_playback_delay(uint64_t frame_enc_size,
                                                uint64_t byte_rate,
                                                uint32_t latency);

static void mjpeg_video_buffer_free(VideoBuffer *video_buffer)
{
    MJpegVideoBuffer *buffer = (MJpegVideoBuffer*)video_buffer;
    g_free(buffer->base.data);
    g_free(buffer);
}

static MJpegVideoBuffer* create_mjpeg_video_buffer(void)
{
    MJpegVideoBuffer *buffer = g_new0(MJpegVideoBuffer, 1);
    buffer->base.free = mjpeg_video_buffer_free;
    buffer->maxsize = MJPEG_INITIAL_BUFFER_SIZE;
    buffer->base.data = g_try_malloc(buffer->maxsize);
    if (!buffer->base.data) {
        g_free(buffer);
        buffer = NULL;
    }
    return buffer;
}

static void mjpeg_encoder_destroy(VideoEncoder *video_encoder)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    g_free(encoder->cinfo.dest);
    jpeg_destroy_compress(&encoder->cinfo);
    g_free(encoder->row);
    g_free(encoder);
}

static uint8_t mjpeg_encoder_get_bytes_per_pixel(MJpegEncoder *encoder)
{
    return encoder->bytes_per_pixel;
}

#ifndef JCS_EXTENSIONS
/* Pixel conversion routines */
static void pixel_rgb24bpp_to_24(void *src_ptr, uint8_t *dest)
{
    uint8_t *src = src_ptr;
    /* libjpegs stores rgb, spice/win32 stores bgr */
    *dest++ = src[2]; /* red */
    *dest++ = src[1]; /* green */
    *dest++ = src[0]; /* blue */
}

static void pixel_rgb32bpp_to_24(void *src, uint8_t *dest)
{
    uint32_t pixel = *(uint32_t *)src;
    *dest++ = (pixel >> 16) & 0xff;
    *dest++ = (pixel >>  8) & 0xff;
    *dest++ = (pixel >>  0) & 0xff;
}
#endif

static void pixel_rgb16bpp_to_24(void *src, uint8_t *dest)
{
    uint16_t pixel = *(uint16_t *)src;
    *dest++ = ((pixel >> 7) & 0xf8) | ((pixel >> 12) & 0x7);
    *dest++ = ((pixel >> 2) & 0xf8) | ((pixel >> 7) & 0x7);
    *dest++ = ((pixel << 3) & 0xf8) | ((pixel >> 2) & 0x7);
}


/* code from libjpeg 8 to handle compression to a memory buffer
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * Modified 2009 by Guido Vollbeding.
 * This file is part of the Independent JPEG Group's software.
 */
typedef struct {
  struct jpeg_destination_mgr pub; /* public fields */

  unsigned char ** outbuffer;      /* target buffer */
  size_t * outsize;
  uint8_t * buffer;                /* start of buffer */
  size_t bufsize;
} mem_destination_mgr;

static void init_mem_destination(j_compress_ptr cinfo)
{
}

static boolean empty_mem_output_buffer(j_compress_ptr cinfo)
{
  size_t nextsize;
  uint8_t * nextbuffer;
  mem_destination_mgr *dest = (mem_destination_mgr *) cinfo->dest;

  /* Try to allocate new buffer with double size */
  nextsize = dest->bufsize * 2;
  nextbuffer = g_try_realloc(dest->buffer, nextsize);

  if (nextbuffer == NULL)
    ERREXIT1(cinfo, JERR_OUT_OF_MEMORY, 10);

  dest->pub.next_output_byte = nextbuffer + dest->bufsize;
  dest->pub.free_in_buffer = dest->bufsize;

  dest->buffer = nextbuffer;
  dest->bufsize = nextsize;

  return TRUE;
}

static void term_mem_destination(j_compress_ptr cinfo)
{
  mem_destination_mgr *dest = (mem_destination_mgr *) cinfo->dest;

  *dest->outbuffer = dest->buffer;
  *dest->outsize = dest->bufsize;
}

/*
 * Prepare for output to a memory buffer.
 * The caller must supply its own initial buffer and size.
 * When the actual data output exceeds the given size, the library
 * will adapt the buffer size as necessary using the g_malloc()/g_free()
 * functions. The buffer is available to the application after the
 * compression and the application is then responsible for freeing it.
 */
static void
spice_jpeg_mem_dest(j_compress_ptr cinfo,
                    unsigned char ** outbuffer, size_t * outsize)
{
  mem_destination_mgr *dest;

  if (outbuffer == NULL || *outbuffer == NULL ||
      outsize == NULL || *outsize == 0) { /* sanity check */
    ERREXIT(cinfo, JERR_BUFFER_SIZE);
  }

  /* The destination object is made permanent so that multiple JPEG images
   * can be written to the same buffer without re-executing jpeg_mem_dest.
   */
  if (cinfo->dest == NULL) { /* first time for this JPEG object? */
    cinfo->dest = g_malloc(sizeof(mem_destination_mgr));
  }

  dest = (mem_destination_mgr *) cinfo->dest;
  dest->pub.init_destination = init_mem_destination;
  dest->pub.empty_output_buffer = empty_mem_output_buffer;
  dest->pub.term_destination = term_mem_destination;
  dest->outbuffer = outbuffer;
  dest->outsize = outsize;

  dest->pub.next_output_byte = dest->buffer = *outbuffer;
  dest->pub.free_in_buffer = dest->bufsize = *outsize;
}
/* end of code from libjpeg */

static inline uint32_t mjpeg_encoder_get_source_fps(MJpegEncoder *encoder)
{
    return encoder->cbs.get_source_fps ?
        encoder->cbs.get_source_fps(encoder->cbs.opaque) : MJPEG_MAX_FPS;
}

static inline uint32_t mjpeg_encoder_get_latency(MJpegEncoder *encoder)
{
    return encoder->cbs.get_roundtrip_ms ?
        encoder->cbs.get_roundtrip_ms(encoder->cbs.opaque) / 2 : 0;
}

static uint32_t get_max_fps(uint64_t frame_size, uint64_t bytes_per_sec)
{
    return frame_size ? bytes_per_sec / frame_size : MJPEG_MAX_FPS;
}

static inline void mjpeg_encoder_reset_quality(MJpegEncoder *encoder,
                                               int quality_id,
                                               uint32_t fps,
                                               uint64_t frame_enc_size)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    double fps_ratio;

    rate_control->during_quality_eval = FALSE;

    if (rate_control->quality_id != quality_id) {
        rate_control->last_enc_size = 0;
    }

    if (rate_control->quality_eval_data.reason == MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE) {
        memset(&rate_control->server_state, 0, sizeof(MJpegEncoderServerState));
    }
    rate_control->quality_id = quality_id;
    memset(&rate_control->quality_eval_data, 0, sizeof(MJpegEncoderQualityEval));
    rate_control->quality_eval_data.max_quality_id = MJPEG_QUALITY_SAMPLE_NUM - 1;
    rate_control->quality_eval_data.max_quality_fps = MJPEG_MAX_FPS;

    if (rate_control->adjusted_fps) {
        fps_ratio = rate_control->adjusted_fps / rate_control->fps;
    } else {
        fps_ratio = 1.5;
    }
    rate_control->fps = MAX(MJPEG_MIN_FPS, fps);
    rate_control->fps = MIN(MJPEG_MAX_FPS, rate_control->fps);
    rate_control->adjusted_fps = rate_control->fps*fps_ratio;
    spice_debug("adjusted-fps-ratio=%.2f adjusted-fps=%.2f", fps_ratio, rate_control->adjusted_fps);
    rate_control->adjusted_fps_start_time = 0;
    rate_control->adjusted_fps_num_frames = 0;
    rate_control->base_enc_size = frame_enc_size;

    rate_control->sum_recent_enc_size = 0;
    rate_control->num_recent_enc_frames = 0;
}

#define QUALITY_WAS_EVALUATED(encoder, quality) \
    ((encoder)->rate_control.quality_eval_data.encoded_size_by_quality[(quality)] != 0)

/*
 * Adjust the stream's jpeg quality and frame rate.
 * We evaluate the compression ratio of different jpeg qualities;
 * We compress successive frames with different qualities,
 * and then we estimate the stream frame rate according to the currently
 * evaluated jpeg quality and available bit rate.
 *
 * During quality evaluation, mjpeg_encoder_eval_quality is called before a new
 * frame is encoded. mjpeg_encoder_eval_quality examines the encoding size of
 * the previously encoded frame, and determines whether to continue evaluation
 * (and chnages the quality for the frame that is going to be encoded),
 * or stop evaluation (and sets the quality and frame rate for the stream).
 * When qualities are scanned, we assume monotonicity of compression ratio
 * as a function of jpeg quality. When we reach a quality with too small, or
 * big enough compression ratio, we stop the evaluation and set the stream parameters.
*/
static inline void mjpeg_encoder_eval_quality(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control;
    MJpegEncoderQualityEval *quality_eval;
    uint32_t fps, src_fps;
    uint64_t enc_size;
    uint32_t final_quality_id;
    uint32_t final_fps;
    uint64_t final_quality_enc_size;

    rate_control = &encoder->rate_control;
    quality_eval = &rate_control->quality_eval_data;

    spice_assert(rate_control->during_quality_eval);

    /* retrieving the encoded size of the last encoded frame */
    enc_size = quality_eval->encoded_size_by_quality[rate_control->quality_id];
    if (enc_size == 0) {
        spice_debug("size info missing");
        return;
    }

    src_fps = mjpeg_encoder_get_source_fps(encoder);

    fps = get_max_fps(enc_size, rate_control->byte_rate);
    spice_debug("mjpeg %p: jpeg %d: %.2f (KB) fps %d src-fps %u",
                encoder,
                mjpeg_quality_samples[rate_control->quality_id],
                enc_size / 1024.0,
                fps,
                src_fps);

    if (fps > quality_eval->max_sampled_fps ||
        ((fps == quality_eval->max_sampled_fps || fps >= src_fps) &&
         rate_control->quality_id > quality_eval->max_sampled_fps_quality_id)) {
        quality_eval->max_sampled_fps = fps;
        quality_eval->max_sampled_fps_quality_id = rate_control->quality_id;
    }

    /*
     * Choosing whether to evaluate another quality, or to complete evaluation
     * and set the stream parameters according to one of the qualities that
     * were already sampled.
     */

    if (rate_control->quality_id > MJPEG_QUALITY_SAMPLE_NUM / 2 &&
        fps < MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH &&
        fps < src_fps) {
        /*
         * When the jpeg quality is bigger than the median quality, prefer a reasonable
         * frame rate over improving the quality
         */
        spice_debug("fps < %d && (fps < src_fps), quality %d",
                MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH,
                mjpeg_quality_samples[rate_control->quality_id]);
        if (QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id - 1)) {
            /* the next worse quality was already evaluated and it passed the frame
             * rate thresholds (we know that, because we continued evaluating a better
             * quality) */
            rate_control->quality_id--;
            goto complete_sample;
        } else {
            /* evaluate the next worse quality */
            rate_control->quality_id--;
        }
    } else if ((fps > MJPEG_IMPROVE_QUALITY_FPS_PERMISSIVE_TH &&
                fps >= 0.66 * quality_eval->min_quality_fps) || fps >= src_fps) {
        /* When the jpeg quality is worse than the median one (see first condition), we allow a less
           strict threshold for fps, in order to improve the jpeg quality */
        if (rate_control->quality_id + 1 == MJPEG_QUALITY_SAMPLE_NUM ||
            rate_control->quality_id >= quality_eval->max_quality_id ||
            QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id + 1)) {
            /* best quality has been reached, or the next (better) quality was
             * already evaluated and didn't pass the fps thresholds */
            goto complete_sample;
        } else {
            if (rate_control->quality_id == MJPEG_QUALITY_SAMPLE_NUM / 2 &&
                fps < MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH &&
                fps < src_fps) {
                goto complete_sample;
            }
            /* evaluate the next quality as well*/
            rate_control->quality_id++;
        }
    } else { // very small frame rate, try to improve by downgrading the quality
        if (rate_control->quality_id == 0 ||
            rate_control->quality_id <= quality_eval->min_quality_id) {
            goto complete_sample;
        } else if (QUALITY_WAS_EVALUATED(encoder, rate_control->quality_id - 1)) {
            rate_control->quality_id--;
            goto complete_sample;
        } else {
            /* evaluate the next worse quality */
            rate_control->quality_id--;
        }
    }
    return;

complete_sample:
    if (quality_eval->max_sampled_fps != 0) {
        /* covering a case were monotonicity was violated and we sampled
           a better jepg quality, with better frame rate. */
        final_quality_id = MAX(rate_control->quality_id,
                               quality_eval->max_sampled_fps_quality_id);
    } else {
        final_quality_id = rate_control->quality_id;
    }
    final_quality_enc_size = quality_eval->encoded_size_by_quality[final_quality_id];
    final_fps = get_max_fps(final_quality_enc_size,
                            rate_control->byte_rate);

    if (final_quality_id == quality_eval->min_quality_id) {
        final_fps = MAX(final_fps, quality_eval->min_quality_fps);
    }
    if (final_quality_id == quality_eval->max_quality_id) {
        final_fps = MIN(final_fps, quality_eval->max_quality_fps);
    }
    mjpeg_encoder_reset_quality(encoder, final_quality_id, final_fps, final_quality_enc_size);
    rate_control->sum_recent_enc_size = final_quality_enc_size;
    rate_control->num_recent_enc_frames = 1;

    spice_debug("MJpeg quality sample end %p: quality %d fps %d",
                encoder, mjpeg_quality_samples[rate_control->quality_id], rate_control->fps);
    if (encoder->cbs.update_client_playback_delay) {
        uint32_t latency = mjpeg_encoder_get_latency(encoder);
        uint32_t min_delay = get_min_required_playback_delay(final_quality_enc_size,
                                                             rate_control->byte_rate,
                                                             latency);

        encoder->cbs.update_client_playback_delay(encoder->cbs.opaque, min_delay);
    }
}

static void mjpeg_encoder_quality_eval_set_upgrade(MJpegEncoder *encoder,
                                                   int reason,
                                                   uint32_t min_quality_id,
                                                   uint32_t min_quality_fps)
{
    MJpegEncoderQualityEval *quality_eval = &encoder->rate_control.quality_eval_data;

    encoder->rate_control.during_quality_eval = TRUE;
    quality_eval->type = MJPEG_QUALITY_EVAL_TYPE_UPGRADE;
    quality_eval->reason = reason;
    quality_eval->min_quality_id = min_quality_id;
    quality_eval->min_quality_fps = min_quality_fps;
}

static void mjpeg_encoder_quality_eval_set_downgrade(MJpegEncoder *encoder,
                                                     int reason,
                                                     uint32_t max_quality_id,
                                                     uint32_t max_quality_fps)
{
    MJpegEncoderQualityEval *quality_eval = &encoder->rate_control.quality_eval_data;

    encoder->rate_control.during_quality_eval = TRUE;
    quality_eval->type = MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE;
    quality_eval->reason = reason;
    quality_eval->max_quality_id = max_quality_id;
    quality_eval->max_quality_fps = max_quality_fps;
}

static void mjpeg_encoder_adjust_params_to_bit_rate(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control;
    MJpegEncoderQualityEval *quality_eval;
    uint64_t new_avg_enc_size = 0;
    uint32_t new_fps;
    uint32_t latency = 0;
    uint32_t src_fps;

    rate_control = &encoder->rate_control;
    quality_eval = &rate_control->quality_eval_data;

    if (!rate_control->last_enc_size) {
        spice_debug("missing sample size");
        return;
    }

    if (rate_control->during_quality_eval) {
        quality_eval->encoded_size_by_quality[rate_control->quality_id] = rate_control->last_enc_size;
        mjpeg_encoder_eval_quality(encoder);
        return;
    }

    if (!rate_control->num_recent_enc_frames) {
        spice_debug("No recent encoded frames");
        return;
    }

    if (rate_control->num_recent_enc_frames < MJPEG_AVERAGE_SIZE_WINDOW &&
        rate_control->num_recent_enc_frames < rate_control->fps) {
        goto end;
    }

    latency = mjpeg_encoder_get_latency(encoder);
    new_avg_enc_size = rate_control->sum_recent_enc_size /
                       rate_control->num_recent_enc_frames;
    new_fps = get_max_fps(new_avg_enc_size, rate_control->byte_rate);

    spice_debug("cur-fps=%u new-fps=%u (new/old=%.2f) |"
                "bit-rate=%.2f (Mbps) latency=%u (ms) quality=%d |"
                " new-size-avg %"PRIu64" , base-size %"PRIu64", (new/old=%.2f) ",
                rate_control->fps, new_fps, ((double)new_fps)/rate_control->fps,
                ((double)rate_control->byte_rate*8)/1024/1024,
                latency,
                mjpeg_quality_samples[rate_control->quality_id],
                new_avg_enc_size, rate_control->base_enc_size,
                rate_control->base_enc_size ?
                    ((double)new_avg_enc_size) / rate_control->base_enc_size :
                    1);

     src_fps = mjpeg_encoder_get_source_fps(encoder);

    /*
     * The ratio between the new_fps and the current fps reflects the changes
     * in latency and frame size. When the change passes a threshold,
     * we re-evaluate the quality and frame rate.
     */
    if (new_fps > rate_control->fps &&
        (rate_control->fps < src_fps || rate_control->quality_id < MJPEG_QUALITY_SAMPLE_NUM - 1)) {
        spice_debug("mjpeg %p FPS CHANGE >> :  re-evaluating params", encoder);
        mjpeg_encoder_quality_eval_set_upgrade(encoder, MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
                                               rate_control->quality_id, /* fps has improved -->
                                                                            don't allow stream quality
                                                                            to deteriorate */
                                               rate_control->fps);
    } else if (new_fps < rate_control->fps && new_fps < src_fps) {
        spice_debug("mjpeg %p FPS CHANGE << : re-evaluating params", encoder);
        mjpeg_encoder_quality_eval_set_downgrade(encoder, MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE,
                                                 rate_control->quality_id,
                                                 rate_control->fps);
    }
end:
    if (rate_control->during_quality_eval) {
        quality_eval->encoded_size_by_quality[rate_control->quality_id] = new_avg_enc_size;
        mjpeg_encoder_eval_quality(encoder);
    } else {
        mjpeg_encoder_process_server_drops(encoder);
    }
}

/*
 * The actual frames distribution does not necessarily fit the condition "at least
 * one frame every (1000/rate_contorl->fps) milliseconds".
 * For keeping the average fps close to the defined fps, we periodically
 * measure the current average fps, and modify rate_control->adjusted_fps accordingly.
 * Then, we use (1000/rate_control->adjusted_fps) as the interval between frames.
 */
static void mjpeg_encoder_adjust_fps(MJpegEncoder *encoder, uint64_t now)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    uint64_t adjusted_fps_time_passed;

    adjusted_fps_time_passed = (now - rate_control->adjusted_fps_start_time) / NSEC_PER_MILLISEC;

    if (!rate_control->during_quality_eval &&
        adjusted_fps_time_passed > MJPEG_ADJUST_FPS_TIMEOUT &&
        adjusted_fps_time_passed > MSEC_PER_SEC / rate_control->adjusted_fps) {
        double avg_fps;
        double fps_ratio;

        avg_fps = ((double)rate_control->adjusted_fps_num_frames * MSEC_PER_SEC) /
                  adjusted_fps_time_passed;
        spice_debug("#frames-adjust=%"PRIu64" #adjust-time=%"PRIu64" avg-fps=%.2f",
                    rate_control->adjusted_fps_num_frames, adjusted_fps_time_passed, avg_fps);
        spice_debug("defined=%u old-adjusted=%.2f", rate_control->fps, rate_control->adjusted_fps);
        fps_ratio = avg_fps / rate_control->fps;
        if (avg_fps + 0.5 < rate_control->fps &&
            mjpeg_encoder_get_source_fps(encoder) > avg_fps) {
            double new_adjusted_fps = avg_fps ?
                                               (rate_control->adjusted_fps/fps_ratio) :
                                               rate_control->adjusted_fps * 2;

            rate_control->adjusted_fps = MIN(rate_control->fps*2, new_adjusted_fps);
            spice_debug("new-adjusted-fps=%.2f", rate_control->adjusted_fps);
        } else if (rate_control->fps + 0.5 < avg_fps) {
            double new_adjusted_fps = rate_control->adjusted_fps / fps_ratio;

            rate_control->adjusted_fps = MAX(rate_control->fps, new_adjusted_fps);
            spice_debug("new-adjusted-fps=%.2f", rate_control->adjusted_fps);
        }
        rate_control->adjusted_fps_start_time = now;
        rate_control->adjusted_fps_num_frames = 0;
    }
}

/*
 * dest must be either NULL or allocated by g_malloc, since it might be freed
 * during the encoding, if its size is too small.
 *
 * return:
 *  MJPEG_ENCODER_FRAME_UNSUPPORTED : frame cannot be encoded
 *  MJPEG_ENCODER_FRAME_DROP        : frame should be dropped. This value can only be returned
 *                                    if mjpeg rate control is active.
 *  MJPEG_ENCODER_FRAME_ENCODE_DONE : frame encoding started. Continue with
 *                                    mjpeg_encoder_encode_scanline.
 */
static int mjpeg_encoder_start_frame(MJpegEncoder *encoder,
                                     SpiceBitmapFmt format,
                                     const SpiceRect *src,
                                     MJpegVideoBuffer *buffer,
                                     uint32_t frame_mm_time)
{
    uint32_t quality;

    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    uint64_t now;
    uint64_t interval;

    now = spice_get_monotonic_time_ns();

    if (!rate_control->adjusted_fps_start_time) {
        rate_control->adjusted_fps_start_time = now;
    }
    mjpeg_encoder_adjust_fps(encoder, now);
    interval = (now - rate_control->bit_rate_info.last_frame_time);

    if (interval < NSEC_PER_SEC / rate_control->adjusted_fps) {
        return VIDEO_ENCODER_FRAME_DROP;
    }

    mjpeg_encoder_adjust_params_to_bit_rate(encoder);

    if (!rate_control->during_quality_eval ||
        rate_control->quality_eval_data.reason == MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE) {
        MJpegEncoderBitRateInfo *bit_rate_info;

        bit_rate_info = &encoder->rate_control.bit_rate_info;

        if (!bit_rate_info->change_start_time) {
            bit_rate_info->change_start_time = now;
            bit_rate_info->change_start_mm_time = frame_mm_time;
        }
        bit_rate_info->last_frame_time = now;
    }

    encoder->cinfo.in_color_space   = JCS_RGB;
    encoder->cinfo.input_components = 3;
    encoder->pixel_converter = NULL;

    switch (format) {
    case SPICE_BITMAP_FMT_32BIT:
    case SPICE_BITMAP_FMT_RGBA:
        encoder->bytes_per_pixel = 4;
#ifdef JCS_EXTENSIONS
        encoder->cinfo.in_color_space   = JCS_EXT_BGRX;
        encoder->cinfo.input_components = 4;
#else
        encoder->pixel_converter = pixel_rgb32bpp_to_24;
#endif
        break;
    case SPICE_BITMAP_FMT_16BIT:
        encoder->bytes_per_pixel = 2;
        encoder->pixel_converter = pixel_rgb16bpp_to_24;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        encoder->bytes_per_pixel = 3;
#ifdef JCS_EXTENSIONS
        encoder->cinfo.in_color_space = JCS_EXT_BGR;
#else
        encoder->pixel_converter = pixel_rgb24bpp_to_24;
#endif
        break;
    default:
        spice_debug("unsupported format %d", format);
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    encoder->cinfo.image_width = src->right - src->left;
    encoder->cinfo.image_height = src->bottom - src->top;
    if (encoder->pixel_converter != NULL) {
        JDIMENSION stride = encoder->cinfo.image_width * 3;
        /* check for integer overflow */
        if (stride < encoder->cinfo.image_width) {
            return VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
        if (encoder->row_size < stride) {
            encoder->row = g_realloc(encoder->row, stride);
            encoder->row_size = stride;
        }
    }

    spice_jpeg_mem_dest(&encoder->cinfo, &buffer->base.data, &buffer->maxsize);

    jpeg_set_defaults(&encoder->cinfo);
    encoder->cinfo.dct_method       = JDCT_IFAST;
    quality = mjpeg_quality_samples[encoder->rate_control.quality_id];
    jpeg_set_quality(&encoder->cinfo, quality, TRUE);
    jpeg_start_compress(&encoder->cinfo, encoder->first_frame);

    encoder->num_frames++;
    encoder->avg_quality += quality;
    return VIDEO_ENCODER_FRAME_ENCODE_DONE;
}

static int mjpeg_encoder_encode_scanline(MJpegEncoder *encoder,
                                         uint8_t *src_pixels,
                                         size_t image_width)
{
    unsigned int scanlines_written;
    uint8_t *row;

    row = encoder->row;
    if (encoder->pixel_converter) {
        unsigned int x;
        for (x = 0; x < image_width; x++) {
            /* src_pixels is expected to be 4 bytes aligned */
            encoder->pixel_converter(src_pixels, row);
            row += 3;
            src_pixels += encoder->bytes_per_pixel;
        }
        scanlines_written = jpeg_write_scanlines(&encoder->cinfo, &encoder->row, 1);
    } else {
        scanlines_written = jpeg_write_scanlines(&encoder->cinfo, &src_pixels, 1);
    }
    if (scanlines_written == 0) { /* Not enough space */
        jpeg_abort_compress(&encoder->cinfo);
        encoder->rate_control.last_enc_size = 0;
        return 0;
    }

    return scanlines_written;
}

static size_t mjpeg_encoder_end_frame(MJpegEncoder *encoder)
{
    mem_destination_mgr *dest = (mem_destination_mgr *) encoder->cinfo.dest;
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;

    jpeg_finish_compress(&encoder->cinfo);

    encoder->first_frame = FALSE;
    rate_control->last_enc_size = dest->pub.next_output_byte - dest->buffer;
    rate_control->server_state.num_frames_encoded++;

    if (!rate_control->during_quality_eval ||
        rate_control->quality_eval_data.reason == MJPEG_QUALITY_EVAL_REASON_SIZE_CHANGE) {

        if (!rate_control->during_quality_eval) {
            if (rate_control->num_recent_enc_frames >= MJPEG_AVERAGE_SIZE_WINDOW) {
                rate_control->num_recent_enc_frames = 0;
                rate_control->sum_recent_enc_size = 0;
            }
            rate_control->sum_recent_enc_size += rate_control->last_enc_size;
            rate_control->num_recent_enc_frames++;
            rate_control->adjusted_fps_num_frames++;
        }
        rate_control->bit_rate_info.sum_enc_size += encoder->rate_control.last_enc_size;
        rate_control->bit_rate_info.num_enc_frames++;
    }
    return encoder->rate_control.last_enc_size;
}

static inline uint8_t *get_image_line(SpiceChunks *chunks, size_t *offset,
                                      int *chunk_nr, int stride)
{
    uint8_t *ret;
    SpiceChunk *chunk;

    chunk = &chunks->chunk[*chunk_nr];

    if (*offset == chunk->len) {
        if (*chunk_nr == chunks->num_chunks - 1) {
            return NULL; /* Last chunk */
        }
        *offset = 0;
        (*chunk_nr)++;
        chunk = &chunks->chunk[*chunk_nr];
    }

    if (chunk->len - *offset < stride) {
        spice_warning("bad chunk alignment");
        return NULL;
    }
    ret = chunk->data + *offset;
    *offset += stride;
    return ret;
}

static bool encode_frame(MJpegEncoder *encoder, const SpiceRect *src,
                         const SpiceBitmap *image, int top_down)
{
    SpiceChunks *chunks;
    uint32_t image_stride;
    size_t offset;
    int i, chunk;

    chunks = image->data;
    offset = 0;
    chunk = 0;
    image_stride = image->stride;

    const int skip_lines = top_down ? src->top : image->y - (src->bottom - 0);
    for (i = 0; i < skip_lines; i++) {
        get_image_line(chunks, &offset, &chunk, image_stride);
    }

    const unsigned int stream_height = src->bottom - src->top;
    const unsigned int stream_width = src->right - src->left;

    for (i = 0; i < stream_height; i++) {
        uint8_t *src_line = get_image_line(chunks, &offset, &chunk, image_stride);

        if (!src_line) {
            return FALSE;
        }

        src_line += src->left * mjpeg_encoder_get_bytes_per_pixel(encoder);
        if (mjpeg_encoder_encode_scanline(encoder, src_line, stream_width) == 0) {
            return FALSE;
        }
    }

    return TRUE;
}

static int mjpeg_encoder_encode_frame(VideoEncoder *video_encoder,
                                      uint32_t frame_mm_time,
                                      SpiceImage *image,
                                      const SpiceRect *src, int top_down,
                                      gpointer bitmap_opaque,
                                      VideoBuffer **outbuf)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    MJpegVideoBuffer *buffer = create_mjpeg_video_buffer();
    if (!buffer) {
        return VIDEO_ENCODER_FRAME_UNSUPPORTED;
    }

    const SpiceBitmap *bitmap = &image->u.bitmap;
    int ret = mjpeg_encoder_start_frame(encoder, bitmap->format, src,
                                        buffer, frame_mm_time);
    if (ret == VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        if (encode_frame(encoder, src, bitmap, top_down)) {
            buffer->base.size = mjpeg_encoder_end_frame(encoder);
            *outbuf = (VideoBuffer*)buffer;
        } else {
            ret = VIDEO_ENCODER_FRAME_UNSUPPORTED;
        }
    }

    if (ret != VIDEO_ENCODER_FRAME_ENCODE_DONE) {
        buffer->base.free(&buffer->base);
    }
    return ret;
}


static void mjpeg_encoder_quality_eval_stop(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    uint32_t quality_id;
    uint32_t fps;

    if (!rate_control->during_quality_eval) {
        return;
    }
    switch (rate_control->quality_eval_data.type) {
    case MJPEG_QUALITY_EVAL_TYPE_UPGRADE:
        quality_id = rate_control->quality_eval_data.min_quality_id;
        fps = rate_control->quality_eval_data.min_quality_fps;
        break;
    case MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE:
        quality_id = rate_control->quality_eval_data.max_quality_id;
        fps = rate_control->quality_eval_data.max_quality_fps;
        break;
    case MJPEG_QUALITY_EVAL_TYPE_SET:
        quality_id = MJPEG_QUALITY_SAMPLE_NUM / 2;
        fps = MJPEG_MAX_FPS / 2;
        break;
    default:
        spice_warning("unexpected");
        return;
    }
    mjpeg_encoder_reset_quality(encoder, quality_id, fps, 0);
    spice_debug("during quality evaluation: canceling."
                "reset quality to %d fps %d",
                mjpeg_quality_samples[rate_control->quality_id], rate_control->fps);
}

static void mjpeg_encoder_decrease_bit_rate(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    MJpegEncoderBitRateInfo *bit_rate_info = &rate_control->bit_rate_info;
    uint64_t measured_byte_rate;
    uint32_t measured_fps;
    uint64_t decrease_size;

    mjpeg_encoder_quality_eval_stop(encoder);

    rate_control->client_state.max_video_latency = 0;
    rate_control->client_state.max_audio_latency = 0;
    if (rate_control->warmup_start_time) {
        uint64_t now;

        now = spice_get_monotonic_time_ns();
        if (now - rate_control->warmup_start_time < MJPEG_WARMUP_TIME) {
            spice_debug("during warmup. ignoring");
            return;
        } else {
            rate_control->warmup_start_time = 0;
        }
    }

    if (bit_rate_info->num_enc_frames > MJPEG_BIT_RATE_EVAL_MIN_NUM_FRAMES ||
        bit_rate_info->num_enc_frames > rate_control->fps) {
        double duration_sec;

        duration_sec = (bit_rate_info->last_frame_time - bit_rate_info->change_start_time);
        duration_sec /= NSEC_PER_SEC;
        measured_byte_rate = bit_rate_info->sum_enc_size / duration_sec;
        measured_fps = bit_rate_info->num_enc_frames / duration_sec;
        decrease_size = bit_rate_info->sum_enc_size / bit_rate_info->num_enc_frames;
        spice_debug("bit rate estimation %.2f (Mbps) fps %u",
                    measured_byte_rate*8/1024.0/1024,
                    measured_fps);
    } else {
        measured_byte_rate = rate_control->byte_rate;
        measured_fps = rate_control->fps;
        decrease_size = measured_byte_rate/measured_fps;
        spice_debug("bit rate not re-estimated %.2f (Mbps) fps %u",
                    measured_byte_rate*8/1024.0/1024,
                    measured_fps);
    }

    measured_byte_rate = MIN(rate_control->byte_rate, measured_byte_rate);

    if (decrease_size >=  measured_byte_rate) {
        decrease_size = measured_byte_rate / 2;
    }

    rate_control->byte_rate = measured_byte_rate - decrease_size;
    bit_rate_info->change_start_time = 0;
    bit_rate_info->change_start_mm_time = 0;
    bit_rate_info->last_frame_time = 0;
    bit_rate_info->num_enc_frames = 0;
    bit_rate_info->sum_enc_size = 0;
    bit_rate_info->was_upgraded = FALSE;

    spice_debug("decrease bit rate %.2f (Mbps)", rate_control->byte_rate * 8 / 1024.0/1024.0);
    mjpeg_encoder_quality_eval_set_downgrade(encoder,
                                             MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE,
                                             rate_control->quality_id,
                                             rate_control->fps);
}

static void mjpeg_encoder_handle_negative_client_stream_report(MJpegEncoder *encoder,
                                                               uint32_t report_end_frame_mm_time)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;

    spice_debug("trace");

    if ((rate_control->bit_rate_info.change_start_mm_time > report_end_frame_mm_time ||
        !rate_control->bit_rate_info.change_start_mm_time) &&
         !rate_control->bit_rate_info.was_upgraded) {
        spice_debug("ignoring, a downgrade has already occurred later to the report time");
        return;
    }

    mjpeg_encoder_decrease_bit_rate(encoder);
}

static void mjpeg_encoder_increase_bit_rate(MJpegEncoder *encoder)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    MJpegEncoderBitRateInfo *bit_rate_info = &rate_control->bit_rate_info;
    uint64_t measured_byte_rate;
    uint32_t measured_fps;
    uint64_t increase_size;


    if (bit_rate_info->num_enc_frames > MJPEG_BIT_RATE_EVAL_MIN_NUM_FRAMES ||
        bit_rate_info->num_enc_frames > rate_control->fps) {
        uint64_t avg_frame_size;
        double duration_sec;

        duration_sec = (bit_rate_info->last_frame_time - bit_rate_info->change_start_time);
        duration_sec /= NSEC_PER_SEC;
        measured_byte_rate = bit_rate_info->sum_enc_size / duration_sec;
        measured_fps = bit_rate_info->num_enc_frames / duration_sec;
        avg_frame_size = bit_rate_info->sum_enc_size / bit_rate_info->num_enc_frames;
        spice_debug("bit rate estimation %.2f (Mbps) defined %.2f"
                    " fps %u avg-frame-size=%.2f (KB)",
                    measured_byte_rate*8/1024.0/1024,
                    rate_control->byte_rate*8/1024.0/1024,
                    measured_fps,
                    avg_frame_size/1024.0);
        increase_size = avg_frame_size;
    } else {
        spice_debug("not enough samples for measuring the bit rate. no change");
        return;
    }


    mjpeg_encoder_quality_eval_stop(encoder);

    if (measured_byte_rate + increase_size < rate_control->byte_rate) {
        spice_debug("measured byte rate is small: not upgrading, just re-evaluating");
    } else {
        rate_control->byte_rate = MIN(measured_byte_rate, rate_control->byte_rate) + increase_size;
    }

    bit_rate_info->change_start_time = 0;
    bit_rate_info->change_start_mm_time = 0;
    bit_rate_info->last_frame_time = 0;
    bit_rate_info->num_enc_frames = 0;
    bit_rate_info->sum_enc_size = 0;
    bit_rate_info->was_upgraded = TRUE;

    spice_debug("increase bit rate %.2f (Mbps)", rate_control->byte_rate * 8 / 1024.0/1024.0);
    mjpeg_encoder_quality_eval_set_upgrade(encoder,
                                           MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE,
                                           rate_control->quality_id,
                                           rate_control->fps);
}

static void mjpeg_encoder_handle_positive_client_stream_report(MJpegEncoder *encoder,
                                                               uint32_t report_start_frame_mm_time)
{
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    MJpegEncoderBitRateInfo *bit_rate_info = &rate_control->bit_rate_info;
    int stable_client_mm_time;
    int timeout;

    if (rate_control->during_quality_eval &&
        rate_control->quality_eval_data.reason == MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE) {
        spice_debug("during quality evaluation (rate change). ignoring report");
        return;
    }

    if ((rate_control->fps > MJPEG_IMPROVE_QUALITY_FPS_STRICT_TH ||
         rate_control->fps >= mjpeg_encoder_get_source_fps(encoder)) &&
         rate_control->quality_id > MJPEG_QUALITY_SAMPLE_NUM / 2) {
        timeout = MJPEG_CLIENT_POSITIVE_REPORT_STRICT_TIMEOUT;
    } else {
        timeout = MJPEG_CLIENT_POSITIVE_REPORT_TIMEOUT;
    }

    stable_client_mm_time = (int)report_start_frame_mm_time - bit_rate_info->change_start_mm_time;

    if (!bit_rate_info->change_start_mm_time || stable_client_mm_time < timeout) {
        /* assessing the stability of the current setting and only then
         * respond to the report */
        spice_debug("no drops, but not enough time has passed for assessing "
                    "the playback stability since the last bit rate change");
        return;
    }
    mjpeg_encoder_increase_bit_rate(encoder);
}

/*
 * the video playback jitter buffer should be at least (send_time*2 + net_latency) for
 * preventing underflow
 */
static uint32_t get_min_required_playback_delay(uint64_t frame_enc_size,
                                                uint64_t byte_rate,
                                                uint32_t latency)
{
    uint32_t one_frame_time;
    uint32_t min_delay;

    if (!frame_enc_size || !byte_rate) {
        return latency;
    }
    one_frame_time = (frame_enc_size * MSEC_PER_SEC) / byte_rate;

    min_delay = MIN(one_frame_time*2 + latency, MJPEG_MAX_CLIENT_PLAYBACK_DELAY);
    return min_delay;
}

#define MJPEG_PLAYBACK_LATENCY_DECREASE_FACTOR 0.5
#define MJPEG_VIDEO_VS_AUDIO_LATENCY_FACTOR 1.25
#define MJPEG_VIDEO_DELAY_TH -15

static void mjpeg_encoder_client_stream_report(VideoEncoder *video_encoder,
                                               uint32_t num_frames,
                                               uint32_t num_drops,
                                               uint32_t start_frame_mm_time,
                                               uint32_t end_frame_mm_time,
                                               int32_t end_frame_delay,
                                               uint32_t audio_delay)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    MJpegEncoderRateControl *rate_control = &encoder->rate_control;
    MJpegEncoderClientState *client_state = &rate_control->client_state;
    uint64_t avg_enc_size = 0;
    uint32_t min_playback_delay;
    int is_video_delay_small = FALSE;

    spice_debug("client report: #frames %u, #drops %d, duration %u video-delay %d audio-delay %u",
                num_frames, num_drops,
                end_frame_mm_time - start_frame_mm_time,
                end_frame_delay, audio_delay);

    if (rate_control->during_quality_eval) {
        if (rate_control->quality_eval_data.type == MJPEG_QUALITY_EVAL_TYPE_DOWNGRADE &&
            rate_control->quality_eval_data.reason == MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE) {
            spice_debug("during rate downgrade evaluation");
            return;
        }
    }

    if (rate_control->num_recent_enc_frames) {
        avg_enc_size = rate_control->sum_recent_enc_size /
                       rate_control->num_recent_enc_frames;
    }
    spice_debug("recent size avg %.2f (KB)", avg_enc_size / 1024.0);
    min_playback_delay = get_min_required_playback_delay(avg_enc_size, rate_control->byte_rate,
                                                         mjpeg_encoder_get_latency(encoder));
    spice_debug("min-delay %u client-delay %d", min_playback_delay, end_frame_delay);

    if (min_playback_delay > end_frame_delay) {
        uint32_t src_fps = mjpeg_encoder_get_source_fps(encoder);
        /*
        * if the stream is at its highest rate, we can't estimate the "real"
        * network bit rate and the min_playback_delay
        */
        if (rate_control->quality_id != MJPEG_QUALITY_SAMPLE_NUM - 1 ||
            rate_control->fps < MIN(src_fps, MJPEG_MAX_FPS) || end_frame_delay < 0) {
            is_video_delay_small = TRUE;
            if (encoder->cbs.update_client_playback_delay) {
                encoder->cbs.update_client_playback_delay(encoder->cbs.opaque,
                                                          min_playback_delay);
            }
        }
    }


    /*
     * If the audio latency has decreased (since the start of the current
     * sequence of positive reports), and the video latency is bigger, slow down
     * the video rate
     */
    if (end_frame_delay > 0 &&
        audio_delay < MJPEG_PLAYBACK_LATENCY_DECREASE_FACTOR*client_state->max_audio_latency &&
        end_frame_delay > MJPEG_VIDEO_VS_AUDIO_LATENCY_FACTOR*audio_delay) {
        spice_debug("video_latency >> audio_latency && audio_latency << max (%u)",
                    client_state->max_audio_latency);
        mjpeg_encoder_handle_negative_client_stream_report(encoder,
                                                           end_frame_mm_time);
        return;
    }

    if (end_frame_delay < MJPEG_VIDEO_DELAY_TH) {
        mjpeg_encoder_handle_negative_client_stream_report(encoder,
                                                           end_frame_mm_time);
    } else {
        double major_delay_decrease_thresh;
        double medium_delay_decrease_thresh;

        client_state->max_video_latency = MAX(end_frame_delay, client_state->max_video_latency);
        client_state->max_audio_latency = MAX(audio_delay, client_state->max_audio_latency);

        medium_delay_decrease_thresh = client_state->max_video_latency;
        medium_delay_decrease_thresh *= MJPEG_PLAYBACK_LATENCY_DECREASE_FACTOR;

        major_delay_decrease_thresh = medium_delay_decrease_thresh;
        major_delay_decrease_thresh *= MJPEG_PLAYBACK_LATENCY_DECREASE_FACTOR;
        /*
         * since the bit rate and the required latency are only evaluation based on the
         * reports we got till now, we assume that the latency is too low only if it
         * was higher during the time that passed since the last report that resulted
         * in a bit rate decrement. If we find that the latency has decreased, it might
         * suggest that the stream bit rate is too high.
         */
        if ((end_frame_delay < medium_delay_decrease_thresh &&
            is_video_delay_small) || end_frame_delay < major_delay_decrease_thresh) {
            spice_debug("downgrade due to short video delay (last=%u, past-max=%u",
                end_frame_delay, client_state->max_video_latency);
            mjpeg_encoder_handle_negative_client_stream_report(encoder,
                                                               end_frame_mm_time);
        } else if (!num_drops) {
            mjpeg_encoder_handle_positive_client_stream_report(encoder,
                                                               start_frame_mm_time);

        }
    }
}

static void mjpeg_encoder_notify_server_frame_drop(VideoEncoder *video_encoder)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    encoder->rate_control.server_state.num_frames_dropped++;
    mjpeg_encoder_process_server_drops(encoder);
}

/*
 * decrease the bit rate if the drop rate on the sever side exceeds a pre defined
 * threshold.
 */
static void mjpeg_encoder_process_server_drops(MJpegEncoder *encoder)
{
    MJpegEncoderServerState *server_state = &encoder->rate_control.server_state;
    uint32_t num_frames_total;
    double drop_factor;
    uint32_t fps;

    fps = MIN(encoder->rate_control.fps, mjpeg_encoder_get_source_fps(encoder));
    if (server_state->num_frames_encoded < fps * MJPEG_SERVER_STATUS_EVAL_FPS_INTERVAL) {
        return;
    }

    num_frames_total = server_state->num_frames_dropped + server_state->num_frames_encoded;
    drop_factor = ((double)server_state->num_frames_dropped) / num_frames_total;

    spice_debug("#drops %u total %u fps %u src-fps %u",
                server_state->num_frames_dropped,
                num_frames_total,
                encoder->rate_control.fps,
                mjpeg_encoder_get_source_fps(encoder));

    if (drop_factor > MJPEG_SERVER_STATUS_DOWNGRADE_DROP_FACTOR_TH) {
        mjpeg_encoder_decrease_bit_rate(encoder);
    }
    server_state->num_frames_encoded = 0;
    server_state->num_frames_dropped = 0;
}

static uint64_t mjpeg_encoder_get_bit_rate(VideoEncoder *video_encoder)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    return encoder->rate_control.byte_rate * 8;
}

static void mjpeg_encoder_get_stats(VideoEncoder *video_encoder,
                                    VideoEncoderStats *stats)
{
    MJpegEncoder *encoder = (MJpegEncoder*)video_encoder;
    spice_assert(encoder != NULL && stats != NULL);
    stats->starting_bit_rate = encoder->starting_bit_rate;
    stats->cur_bit_rate = mjpeg_encoder_get_bit_rate(video_encoder);
    stats->avg_quality = (double)encoder->avg_quality / encoder->num_frames;
}

VideoEncoder *mjpeg_encoder_new(SpiceVideoCodecType codec_type,
                                uint64_t starting_bit_rate,
                                VideoEncoderRateControlCbs *cbs,
                                bitmap_ref_t bitmap_ref,
                                bitmap_unref_t bitmap_unref)
{
    MJpegEncoder *encoder;

    spice_return_val_if_fail(codec_type == SPICE_VIDEO_CODEC_TYPE_MJPEG, NULL);

    encoder = g_new0(MJpegEncoder, 1);
    encoder->base.destroy = mjpeg_encoder_destroy;
    encoder->base.encode_frame = mjpeg_encoder_encode_frame;
    encoder->base.client_stream_report = mjpeg_encoder_client_stream_report;
    encoder->base.notify_server_frame_drop = mjpeg_encoder_notify_server_frame_drop;
    encoder->base.get_bit_rate = mjpeg_encoder_get_bit_rate;
    encoder->base.get_stats = mjpeg_encoder_get_stats;
    encoder->base.codec_type = codec_type;
    encoder->first_frame = TRUE;
    encoder->rate_control.byte_rate = starting_bit_rate / 8;
    encoder->starting_bit_rate = starting_bit_rate;

    encoder->cbs = *cbs;
    mjpeg_encoder_reset_quality(encoder, MJPEG_QUALITY_SAMPLE_NUM / 2, 5, 0);
    encoder->rate_control.during_quality_eval = TRUE;
    encoder->rate_control.quality_eval_data.type = MJPEG_QUALITY_EVAL_TYPE_SET;
    encoder->rate_control.quality_eval_data.reason = MJPEG_QUALITY_EVAL_REASON_RATE_CHANGE;
    encoder->rate_control.warmup_start_time = spice_get_monotonic_time_ns();

    encoder->cinfo.err = jpeg_std_error(&encoder->jerr);
    jpeg_create_compress(&encoder->cinfo);

    return (VideoEncoder*)encoder;
}
