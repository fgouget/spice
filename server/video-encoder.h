/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.
   Copyright (C) 2015 Jeremy White
   Copyright (C) 2015 Francois Gouget

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

#ifndef VIDEO_ENCODER_H_
#define VIDEO_ENCODER_H_

#include <inttypes.h>
#include <glib.h>
#include <common/draw.h>


/* A structure containing the data for a compressed frame. See encode_frame(). */
typedef struct VideoBuffer VideoBuffer;
struct VideoBuffer {
    /* A pointer to the compressed frame data. */
    uint8_t *data;

    /* The size of the compressed frame in bytes. */
    uint32_t size;

    /* Releases the video buffer resources and deallocates it.
     *
     * @buffer:   The video buffer.
     */
    void (*free)(VideoBuffer *buffer);
};

enum {
    VIDEO_ENCODER_FRAME_UNSUPPORTED = -1,
    VIDEO_ENCODER_FRAME_DROP,
    VIDEO_ENCODER_FRAME_ENCODE_DONE,
};

typedef struct VideoEncoderStats {
    uint64_t starting_bit_rate;
    uint64_t cur_bit_rate;
    double avg_quality;
} VideoEncoderStats;

typedef struct VideoEncoder VideoEncoder;
struct VideoEncoder {
    /* Releases the video encoder's resources */
    void (*destroy)(VideoEncoder *encoder);

    /* Compresses the specified src image area into the outbuf buffer.
     *
     * @encoder:       The video encoder.
     * @frame_mm_time: The frame's mm-time timestamp in milliseconds.
     * @bitmap:        A bitmap containing the source video frame.
     * @src:           A rectangle specifying the area occupied by the video.
     * @top_down:      If true the first video line is specified by src.top.
     * @bitmap_opaque: The parameter for the bitmap_ref() and bitmap_unref()
     *                 callbacks.
     * @outbuf:        A pointer to a VideoBuffer structure containing the
     *                 compressed frame if successful. Call the buffer's
     *                 free() method as soon as it is no longer needed.
     * @return:
     *     VIDEO_ENCODER_FRAME_ENCODE_DONE if successful.
     *     VIDEO_ENCODER_FRAME_UNSUPPORTED if the frame cannot be encoded.
     *     VIDEO_ENCODER_FRAME_DROP if the frame was dropped. This value can
     *                              only happen if rate control is active.
     */
    int (*encode_frame)(VideoEncoder *encoder, uint32_t frame_mm_time,
                        SpiceImage *image,
                        const SpiceRect *src, int top_down,
                        gpointer bitmap_opaque, VideoBuffer** outbuf);

    /*
     * Bit rate control methods.
     */

    /* When rate control is active statistics are periodically obtained from
     * the client and sent to the video encoder through this method.
     *
     * @encoder:       The video encoder.
     * @num_frames:    The number of frames that reached the client during
     *                 the time period the report is referring to.
     * @num_drops:     The part of the above frames that was dropped by the
     *                 client due to late arrival time.
     * @start_frame_mm_time: The mm_time of the first frame included in the
     *                 report.
     * @end_frame_mm_time: The mm_time of the last frame included in the
     *                 report.
     * @end_frame_delay: This indicates how long in advance the client
     *                 received the last frame before having to display it.
     * @audio delay:   The latency of the audio playback or MAX_UINT if it
     *                 is not tracked.
     */
    void (*client_stream_report)(VideoEncoder *encoder,
                                 uint32_t num_frames, uint32_t num_drops,
                                 uint32_t start_frame_mm_time,
                                 uint32_t end_frame_mm_time,
                                 int32_t end_frame_delay, uint32_t audio_delay);

    /* This notifies the video encoder each time a frame is dropped due to
     * pipe congestion.
     *
     * Note that frames are being dropped before they are encoded and that
     * there may be any number of encoded frames in the network queue.
     * The client reports provide richer and typically more reactive
     * information for fine tuning the playback parameters but this function
     * provides a fallback when client reports are getting delayed or are not
     * supported by the client.
     *
     * @encoder:    The video encoder.
     */
    void (*notify_server_frame_drop)(VideoEncoder *encoder);

    /* This queries the video encoder's current bit rate.
     *
     * @encoder:    The video encoder.
     * @return:     The current bit rate in bits per second.
     */
    uint64_t (*get_bit_rate)(VideoEncoder *encoder);

    /* Collects video statistics.
     *
     * @encoder:    The video encoder.
     * @stats:      A VideoEncoderStats structure to fill with the collected
     *              statistics.
     */
    void (*get_stats)(VideoEncoder *encoder, VideoEncoderStats *stats);

    /* The codec being used by the video encoder */
    SpiceVideoCodecType codec_type;
};


/* When rate control is active the video encoder can use these callbacks to
 * figure out how to adjust the stream bit rate and adjust some stream
 * parameters.
 */
typedef struct VideoEncoderRateControlCbs {
    /* The opaque parameter for the callbacks */
    void *opaque;

    /* Returns the stream's estimated roundtrip time in milliseconds. */
    uint32_t (*get_roundtrip_ms)(void *opaque);

    /* Returns the estimated input frame rate.
     *
     * This is the number of frames per second arriving from the guest to
     * spice-server, before any drops.
     */
    uint32_t (*get_source_fps)(void *opaque);

    /* Informs the client of the minimum playback delay.
     *
     * @delay_ms:   The minimum number of milliseconds required for the
     *              frames to reach the client.
     */
    void (*update_client_playback_delay)(void *opaque, uint32_t delay_ms);
} VideoEncoderRateControlCbs;

typedef void (*bitmap_ref_t)(gpointer data);
typedef void (*bitmap_unref_t)(gpointer data);

/* Instantiates the video encoder.
 *
 * @codec_type:        The codec to use.
 * @starting_bit_rate: An initial estimate of the available stream bit rate
 *                     or zero if the client does not support rate control.
 * @cbs:               A set of callback methods to be used for rate control.
 * @bitmap_ref:        A callback that the encoder can use to increase the
 *                     bitmap refcount.
 *                     This must be called from the main context.
 * @bitmap_unref:      A callback that the encoder can use to decrease the
 *                     bitmap refcount.
 *                     This must be called from the main context.
 * @return:            A pointer to a structure implementing the VideoEncoder
 *                     methods.
 */
typedef VideoEncoder* (*new_video_encoder_t)(SpiceVideoCodecType codec_type,
                                             uint64_t starting_bit_rate,
                                             VideoEncoderRateControlCbs *cbs,
                                             bitmap_ref_t bitmap_ref,
                                             bitmap_unref_t bitmap_unref);

VideoEncoder* mjpeg_encoder_new(SpiceVideoCodecType codec_type,
                                uint64_t starting_bit_rate,
                                VideoEncoderRateControlCbs *cbs,
                                bitmap_ref_t bitmap_ref,
                                bitmap_unref_t bitmap_unref);
#if defined(HAVE_GSTREAMER_1_0) || defined(HAVE_GSTREAMER_0_10)
VideoEncoder* gstreamer_encoder_new(SpiceVideoCodecType codec_type,
                                    uint64_t starting_bit_rate,
                                    VideoEncoderRateControlCbs *cbs,
                                    bitmap_ref_t bitmap_ref,
                                    bitmap_unref_t bitmap_unref);
#endif


typedef struct RedVideoCodec {
    new_video_encoder_t create;
    SpiceVideoCodecType type;
    uint32_t cap;
} RedVideoCodec;

#endif /* VIDEO_ENCODER_H_ */
