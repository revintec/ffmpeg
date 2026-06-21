/*
 * ffhls: on-demand, in-memory HLS streaming server over a UNIX domain socket
 *
 * Copyright (c) 2026 FFmpeg contributors
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * ffhls binds to a local UNIX domain socket and speaks just enough HTTP to
 * serve an HLS (HTTP Live Streaming) presentation to a browser media engine
 * (Safari on iOS/iPadOS/macOS natively, Chrome/Firefox via an MSE player such
 * as hls.js). The input media is transcoded to H.264/AAC MPEG-TS segments
 * entirely in memory -- nothing is written to disk. Segments are produced on
 * demand when the client requests them and a small, adaptive read-ahead window
 * pre-buffers a few upcoming segments. The amount pre-buffered is derived from
 * the rate at which the client pulls segments, so a client playing back at 4x
 * (which drains its buffer four times faster and therefore requests segments
 * four times more often) is kept fed without wastefully transcoding the whole
 * file. Because the media playlist is a VOD playlist listing every segment and
 * each segment begins with a forced IDR frame, the client can seek to any
 * position; ffhls simply transcodes the requested segment.
 */

#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "cmdutils.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

const char program_name[] = "ffhls";
const int program_birth_year = 2026;

/* ---- cache entry states ------------------------------------------------- */
enum SegState {
    SEG_EMPTY = 0,  /* not transcoded, no one working on it       */
    SEG_BUSY,       /* a thread is currently transcoding it       */
    SEG_READY,      /* transcoded, data valid                     */
    SEG_ERROR,      /* transcoding failed                         */
};

typedef struct CacheEntry {
    enum SegState state;
    uint8_t      *data;
    int           size;
    int           refcnt;       /* readers currently sending this entry */
    int64_t       last_access;  /* av_gettime_relative() of last use    */
} CacheEntry;

/* ---- global application context ----------------------------------------- */
typedef struct AppContext {
    /* configuration */
    const char *input_url;
    const char *socket_path;
    double      seg_duration;       /* target segment duration (s)        */
    int         max_buffer_segs;    /* upper bound on read-ahead window   */
    int64_t     max_cache_bytes;    /* in-memory segment cache cap        */
    double      lookahead_seconds;  /* how far ahead (media time) to keep */
    int64_t     video_bitrate;
    int64_t     audio_bitrate;
    const char *vcodec_name;        /* optional encoder name override     */

    /* media metadata (probed once at startup) */
    double      duration;           /* total media duration (s)           */
    int         nb_segments;
    int         has_video;
    int         has_audio;
    int         width, height;

    /* segment cache */
    pthread_mutex_t lock;
    pthread_cond_t  cond;           /* broadcast on any state change      */
    CacheEntry     *segs;
    int64_t         cache_bytes;

    /* adaptive read-ahead bookkeeping */
    int             playhead;       /* highest segment index requested    */
    int64_t         last_req_time;  /* wall clock of last new request     */
    int             last_req_idx;
    double          segs_per_sec;   /* smoothed client consumption rate   */
    int             shutdown;
} AppContext;

static AppContext app;
static int listen_fd = -1;
static volatile sig_atomic_t got_signal;

/* =========================================================================
 * Single-segment transcoder
 *
 * Each call is fully self-contained: it opens its own demuxer, seeks to the
 * segment start, sets up decoders/encoders/filters, and muxes an MPEG-TS
 * segment into an in-memory dynamic buffer. Keeping all state local makes it
 * safe to transcode several segments concurrently from different threads.
 * ========================================================================= */

typedef struct StreamXcode {
    int             in_index;       /* stream index in the demuxer        */
    AVCodecContext *dec;
    AVCodecContext *enc;
    AVFilterContext *bufsrc;
    AVFilterContext *bufsink;
    AVFilterGraph  *graph;
    int             out_index;      /* stream index in the muxer          */
    int             started;        /* have we emitted output yet         */
    int             finished;       /* reached segment end                */
} StreamXcode;

static int build_filter(StreamXcode *sx, enum AVMediaType type)
{
    char args[512];
    int ret = 0;
    const AVFilter *bufsrc, *bufsink;
    AVFilterContext *src_ctx = NULL, *sink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *graph   = avfilter_graph_alloc();
    AVCodecContext *dec = sx->dec, *enc = sx->enc;

    if (!outputs || !inputs || !graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (type == AVMEDIA_TYPE_VIDEO) {
        bufsrc  = avfilter_get_by_name("buffer");
        bufsink = avfilter_get_by_name("buffersink");
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 dec->width, dec->height, dec->pix_fmt,
                 dec->pkt_timebase.num, dec->pkt_timebase.den,
                 dec->sample_aspect_ratio.num, dec->sample_aspect_ratio.den);
        ret = avfilter_graph_create_filter(&src_ctx, bufsrc, "in", args, NULL, graph);
        if (ret < 0)
            goto end;
        sink_ctx = avfilter_graph_alloc_filter(graph, bufsink, "out");
        if (!sink_ctx) { ret = AVERROR(ENOMEM); goto end; }
        ret = av_opt_set_bin(sink_ctx, "pix_fmts", (uint8_t*)&enc->pix_fmt,
                             sizeof(enc->pix_fmt), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
            goto end;
        ret = avfilter_init_dict(sink_ctx, NULL);
        if (ret < 0)
            goto end;
    } else {
        char layout[64];
        bufsrc  = avfilter_get_by_name("abuffer");
        bufsink = avfilter_get_by_name("abuffersink");
        if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&dec->ch_layout, dec->ch_layout.nb_channels);
        av_channel_layout_describe(&dec->ch_layout, layout, sizeof(layout));
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                 dec->pkt_timebase.num, dec->pkt_timebase.den, dec->sample_rate,
                 av_get_sample_fmt_name(dec->sample_fmt), layout);
        ret = avfilter_graph_create_filter(&src_ctx, bufsrc, "in", args, NULL, graph);
        if (ret < 0)
            goto end;
        sink_ctx = avfilter_graph_alloc_filter(graph, bufsink, "out");
        if (!sink_ctx) { ret = AVERROR(ENOMEM); goto end; }
        ret = av_opt_set_bin(sink_ctx, "sample_fmts", (uint8_t*)&enc->sample_fmt,
                             sizeof(enc->sample_fmt), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
            goto end;
        av_channel_layout_describe(&enc->ch_layout, layout, sizeof(layout));
        ret = av_opt_set(sink_ctx, "ch_layouts", layout, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
            goto end;
        ret = av_opt_set_bin(sink_ctx, "sample_rates", (uint8_t*)&enc->sample_rate,
                             sizeof(enc->sample_rate), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0)
            goto end;
        ret = avfilter_init_dict(sink_ctx, NULL);
        if (ret < 0)
            goto end;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = src_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = sink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avfilter_graph_parse_ptr(graph, type == AVMEDIA_TYPE_VIDEO ? "null" : "anull",
                                   &inputs, &outputs, NULL);
    if (ret < 0)
        goto end;
    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0)
        goto end;

    if (type == AVMEDIA_TYPE_AUDIO && enc->frame_size > 0)
        av_buffersink_set_frame_size(sink_ctx, enc->frame_size);

    sx->bufsrc  = src_ctx;
    sx->bufsink = sink_ctx;
    sx->graph   = graph;
    graph = NULL;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    return ret;
}

static const AVCodec *find_video_encoder(void)
{
    static const char *const names[] = {
        "libx264", "libopenh264", "h264_videotoolbox", "h264_nvenc", NULL
    };
    const AVCodec *c;

    if (app.vcodec_name) {
        c = avcodec_find_encoder_by_name(app.vcodec_name);
        return c;
    }
    for (int i = 0; names[i]; i++)
        if ((c = avcodec_find_encoder_by_name(names[i])))
            return c;
    return avcodec_find_encoder(AV_CODEC_ID_H264);
}

static int open_video_encoder(StreamXcode *sx, AVFormatContext *oc, int global_header)
{
    const AVCodec *enc = find_video_encoder();
    AVCodecContext *dec = sx->dec, *e;
    const enum AVPixelFormat *pix_fmts = NULL;
    AVDictionary *opts = NULL;
    int ret;

    if (!enc) {
        av_log(NULL, AV_LOG_ERROR, "No usable H.264 encoder found\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    e = avcodec_alloc_context3(enc);
    if (!e)
        return AVERROR(ENOMEM);

    e->width               = dec->width;
    e->height              = dec->height;
    e->sample_aspect_ratio = dec->sample_aspect_ratio;
    ret = avcodec_get_supported_config(e, NULL, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                       (const void**)&pix_fmts, NULL);
    e->pix_fmt   = (ret >= 0 && pix_fmts) ? pix_fmts[0] : AV_PIX_FMT_YUV420P;
    e->framerate = dec->framerate;
    if (!e->framerate.num)
        e->framerate = (AVRational){ 25, 1 };
    e->time_base = av_inv_q(e->framerate);
    e->bit_rate  = app.video_bitrate;
    /* large GOP: keyframes are forced explicitly at the segment boundary so
     * each segment is independently decodable and seekable. */
    e->gop_size     = 250;
    e->max_b_frames = 0;
    if (global_header)
        e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (!strcmp(enc->name, "libx264")) {
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(e, enc, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        avcodec_free_context(&e);
        return ret;
    }
    sx->enc = e;
    return 0;
}

static int open_audio_encoder(StreamXcode *sx, AVFormatContext *oc, int global_header)
{
    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext *dec = sx->dec, *e;
    const enum AVSampleFormat *sample_fmts = NULL;
    int ret;

    if (!enc)
        return AVERROR_ENCODER_NOT_FOUND;
    e = avcodec_alloc_context3(enc);
    if (!e)
        return AVERROR(ENOMEM);

    e->sample_rate = dec->sample_rate;
    ret = av_channel_layout_copy(&e->ch_layout, &dec->ch_layout);
    if (ret < 0) {
        avcodec_free_context(&e);
        return ret;
    }
    if (e->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&e->ch_layout, e->ch_layout.nb_channels);
    ret = avcodec_get_supported_config(e, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                       (const void**)&sample_fmts, NULL);
    e->sample_fmt = (ret >= 0 && sample_fmts) ? sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    e->time_base  = (AVRational){ 1, e->sample_rate };
    e->bit_rate   = app.audio_bitrate;
    if (global_header)
        e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(e, enc, NULL);
    if (ret < 0) {
        avcodec_free_context(&e);
        return ret;
    }
    sx->enc = e;
    return 0;
}

static int open_decoder(StreamXcode *sx, AVStream *st)
{
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *c;
    int ret;

    if (!dec)
        return AVERROR_DECODER_NOT_FOUND;
    c = avcodec_alloc_context3(dec);
    if (!c)
        return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(c, st->codecpar);
    if (ret < 0) {
        avcodec_free_context(&c);
        return ret;
    }
    c->pkt_timebase = st->time_base;
    if (c->codec_type == AVMEDIA_TYPE_VIDEO)
        c->framerate = av_guess_frame_rate(NULL, st, NULL);
    ret = avcodec_open2(c, dec, NULL);
    if (ret < 0) {
        avcodec_free_context(&c);
        return ret;
    }
    sx->dec      = c;
    sx->in_index = st->index;
    return 0;
}

/* encode filtered frames coming out of a stream's filtergraph */
static int encode_filtered(StreamXcode *sx, AVFormatContext *oc, AVFrame *frame,
                           AVPacket *pkt, int force_key)
{
    int ret;

    if (frame && frame->pts != AV_NOPTS_VALUE)
        frame->pts = av_rescale_q(frame->pts, frame->time_base, sx->enc->time_base);
    if (frame && force_key)
        frame->pict_type = AV_PICTURE_TYPE_I;
    else if (frame)
        frame->pict_type = AV_PICTURE_TYPE_NONE;

    ret = avcodec_send_frame(sx->enc, frame);
    if (ret < 0)
        return ret;

    while (ret >= 0) {
        ret = avcodec_receive_packet(sx->enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        pkt->stream_index = sx->out_index;
        av_packet_rescale_ts(pkt, sx->enc->time_base,
                             oc->streams[sx->out_index]->time_base);
        ret = av_interleaved_write_frame(oc, pkt);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* push one decoded frame (or NULL to flush) through filter + encoder */
static int filter_and_encode(StreamXcode *sx, AVFormatContext *oc, AVFrame *dec_frame,
                             AVFrame *filt_frame, AVPacket *pkt)
{
    int ret = av_buffersrc_add_frame_flags(sx->bufsrc, dec_frame, 0);
    if (ret < 0)
        return ret;
    while (1) {
        ret = av_buffersink_get_frame(sx->bufsink, filt_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        filt_frame->time_base = av_buffersink_get_time_base(sx->bufsink);
        ret = encode_filtered(sx, oc, filt_frame, pkt, !sx->started);
        sx->started = 1;
        av_frame_unref(filt_frame);
        if (ret < 0)
            return ret;
    }
}

/* Transcode segment [seg_start, seg_end) of the input into an MPEG-TS buffer. */
static int transcode_segment(int seg_index, uint8_t **out_buf, int *out_size)
{
    AVFormatContext *ic = NULL, *oc = NULL;
    AVIOContext     *dyn = NULL;
    StreamXcode      vx = { .in_index = -1, .out_index = -1 };
    StreamXcode      ax = { .in_index = -1, .out_index = -1 };
    AVPacket        *pkt = NULL, *enc_pkt = NULL;
    AVFrame         *dec_frame = NULL, *filt_frame = NULL;
    double seg_start = seg_index * app.seg_duration;
    double seg_end   = FFMIN((seg_index + 1) * app.seg_duration, app.duration);
    int global_header, ret, vstream = -1, astream = -1;

    *out_buf = NULL;
    *out_size = 0;

    ret = avformat_open_input(&ic, app.input_url, NULL, NULL);
    if (ret < 0)
        goto end;
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0)
        goto end;

    vstream = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    astream = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    ret = avformat_alloc_output_context2(&oc, NULL, "mpegts", NULL);
    if (!oc) {
        ret = AVERROR_UNKNOWN;
        goto end;
    }
    ret = avio_open_dyn_buf(&dyn);
    if (ret < 0)
        goto end;
    oc->pb = dyn;
    global_header = !!(oc->oformat->flags & AVFMT_GLOBALHEADER);

    if (vstream >= 0) {
        AVStream *out;
        if ((ret = open_decoder(&vx, ic->streams[vstream])) < 0)               goto end;
        if ((ret = open_video_encoder(&vx, oc, global_header)) < 0)            goto end;
        if ((ret = build_filter(&vx, AVMEDIA_TYPE_VIDEO)) < 0)                 goto end;
        if (!(out = avformat_new_stream(oc, NULL))) { ret = AVERROR(ENOMEM);   goto end; }
        avcodec_parameters_from_context(out->codecpar, vx.enc);
        out->time_base = vx.enc->time_base;
        vx.out_index   = out->index;
    }
    if (astream >= 0) {
        AVStream *out;
        if ((ret = open_decoder(&ax, ic->streams[astream])) < 0)               goto end;
        if ((ret = open_audio_encoder(&ax, oc, global_header)) < 0)            goto end;
        if ((ret = build_filter(&ax, AVMEDIA_TYPE_AUDIO)) < 0)                 goto end;
        if (!(out = avformat_new_stream(oc, NULL))) { ret = AVERROR(ENOMEM);   goto end; }
        avcodec_parameters_from_context(out->codecpar, ax.enc);
        out->time_base = ax.enc->time_base;
        ax.out_index   = out->index;
    }
    if (vstream < 0 && astream < 0) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    ret = avformat_write_header(oc, NULL);
    if (ret < 0)
        goto end;

    /* Seek to just before the segment start; AVSEEK_FLAG_BACKWARD lands on the
     * preceding keyframe so the decoder has the references it needs. */
    if (seg_index > 0) {
        int64_t ts = (int64_t)(seg_start * AV_TIME_BASE);
        av_seek_frame(ic, -1, ts, AVSEEK_FLAG_BACKWARD);
    }

    pkt        = av_packet_alloc();
    enc_pkt    = av_packet_alloc();
    dec_frame  = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!pkt || !enc_pkt || !dec_frame || !filt_frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    while (!(vx.finished || vstream < 0) || !(ax.finished || astream < 0)) {
        StreamXcode *sx;
        AVStream *ist;
        double t;

        ret = av_read_frame(ic, pkt);
        if (ret < 0)
            break;  /* EOF or error -> flush below */

        if (pkt->stream_index == vstream && vstream >= 0 && !vx.finished)
            sx = &vx;
        else if (pkt->stream_index == astream && astream >= 0 && !ax.finished)
            sx = &ax;
        else {
            av_packet_unref(pkt);
            continue;
        }
        ist = ic->streams[pkt->stream_index];

        ret = avcodec_send_packet(sx->dec, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;

        while (ret >= 0) {
            ret = avcodec_receive_frame(sx->dec, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { ret = 0; break; }
            if (ret < 0)
                goto end;

            dec_frame->pts = dec_frame->best_effort_timestamp;
            t = (dec_frame->pts == AV_NOPTS_VALUE) ? seg_start
                : dec_frame->pts * av_q2d(ist->time_base);

            if (t >= seg_end) {
                sx->finished = 1;
                av_frame_unref(dec_frame);
                break;
            }
            /* drop frames that belong to the previous segment (decoded only to
             * prime the decoder after the backward seek) */
            if (t + 1e-6 < seg_start) {
                av_frame_unref(dec_frame);
                continue;
            }
            ret = filter_and_encode(sx, oc, dec_frame, filt_frame, enc_pkt);
            av_frame_unref(dec_frame);
            if (ret < 0)
                goto end;
        }
    }

    /* flush decoders -> filters -> encoders */
    for (int pass = 0; pass < 2; pass++) {
        StreamXcode *sx = pass ? &ax : &vx;
        if (sx->in_index < 0)
            continue;
        avcodec_send_packet(sx->dec, NULL);
        while (avcodec_receive_frame(sx->dec, dec_frame) >= 0) {
            double t;
            dec_frame->pts = dec_frame->best_effort_timestamp;
            t = (dec_frame->pts == AV_NOPTS_VALUE) ? seg_start
                : dec_frame->pts * av_q2d(ic->streams[sx->in_index]->time_base);
            if (t >= seg_end || t + 1e-6 < seg_start) {
                av_frame_unref(dec_frame);
                continue;
            }
            filter_and_encode(sx, oc, dec_frame, filt_frame, enc_pkt);
            av_frame_unref(dec_frame);
        }
        filter_and_encode(sx, oc, NULL, filt_frame, enc_pkt);   /* flush filter  */
        encode_filtered(sx, oc, NULL, enc_pkt, 0);              /* flush encoder */
    }

    ret = av_write_trailer(oc);
    if (ret < 0)
        goto end;

    *out_size = avio_close_dyn_buf(dyn, out_buf);
    dyn = NULL;
    oc->pb = NULL;
    ret = (*out_size > 0) ? 0 : AVERROR_UNKNOWN;

end:
    if (dyn) {
        uint8_t *tmp = NULL;
        avio_close_dyn_buf(dyn, &tmp);
        av_free(tmp);
    }
    av_packet_free(&pkt);
    av_packet_free(&enc_pkt);
    av_frame_free(&dec_frame);
    av_frame_free(&filt_frame);
    avfilter_graph_free(&vx.graph);
    avfilter_graph_free(&ax.graph);
    avcodec_free_context(&vx.dec);
    avcodec_free_context(&vx.enc);
    avcodec_free_context(&ax.dec);
    avcodec_free_context(&ax.enc);
    if (oc)
        avformat_free_context(oc);
    avformat_close_input(&ic);
    if (ret < 0)
        av_log(NULL, AV_LOG_WARNING, "segment %d transcode failed: %s\n",
               seg_index, av_err2str(ret));
    return ret;
}

/* =========================================================================
 * Segment cache (shared, thread-safe)
 * ========================================================================= */

/* Evict ready, unreferenced entries (LRU) until a new entry of new_bytes fits.
 * Must be called with app.lock held. Never evicts entries near the playhead. */
static void cache_make_room(int64_t new_bytes, int keep_idx)
{
    while (app.cache_bytes + new_bytes > app.max_cache_bytes) {
        int victim = -1;
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < app.nb_segments; i++) {
            CacheEntry *e = &app.segs[i];
            if (e->state != SEG_READY || e->refcnt > 0)
                continue;
            if (i >= keep_idx && i <= keep_idx + app.max_buffer_segs)
                continue;   /* protect the active read-ahead window */
            if (e->last_access < oldest) {
                oldest = e->last_access;
                victim = i;
            }
        }
        if (victim < 0)
            break;          /* nothing evictable */
        av_freep(&app.segs[victim].data);
        app.cache_bytes -= app.segs[victim].size;
        app.segs[victim].size  = 0;
        app.segs[victim].state = SEG_EMPTY;
    }
}

/* Ensure segment idx is transcoded and cached. Returns 0 on success. If
 * acquire is non-zero the caller receives a reference (refcnt++) and must call
 * cache_release(). */
static int cache_ensure(int idx, int acquire, uint8_t **data, int *size)
{
    int ret = 0;

    pthread_mutex_lock(&app.lock);
    for (;;) {
        CacheEntry *e = &app.segs[idx];
        if (app.shutdown) { ret = AVERROR_EXIT; break; }
        if (e->state == SEG_READY) {
            e->last_access = av_gettime_relative();
            if (acquire) {
                e->refcnt++;
                *data = e->data;
                *size = e->size;
            }
            ret = 0;
            break;
        }
        if (e->state == SEG_ERROR) {
            e->state = SEG_EMPTY;   /* allow a later retry */
            ret = AVERROR_UNKNOWN;
            break;
        }
        if (e->state == SEG_BUSY) {
            pthread_cond_wait(&app.cond, &app.lock);
            continue;
        }
        /* SEG_EMPTY: claim it and transcode outside the lock */
        e->state = SEG_BUSY;
        pthread_mutex_unlock(&app.lock);

        {
            uint8_t *buf = NULL;
            int sz = 0;
            int terr = transcode_segment(idx, &buf, &sz);

            pthread_mutex_lock(&app.lock);
            if (terr < 0) {
                app.segs[idx].state = SEG_ERROR;
                av_free(buf);
            } else {
                cache_make_room(sz, app.playhead);
                app.segs[idx].data        = buf;
                app.segs[idx].size        = sz;
                app.segs[idx].state       = SEG_READY;
                app.segs[idx].last_access = av_gettime_relative();
                app.cache_bytes += sz;
            }
            pthread_cond_broadcast(&app.cond);
        }
        /* loop again: will now observe READY or ERROR */
    }
    pthread_mutex_unlock(&app.lock);
    return ret;
}

static void cache_release(int idx)
{
    pthread_mutex_lock(&app.lock);
    if (app.segs[idx].refcnt > 0)
        app.segs[idx].refcnt--;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
}

/* =========================================================================
 * Adaptive read-ahead worker
 *
 * Watches the playhead (highest segment the client has requested) and the rate
 * at which the client advances it, then pre-transcodes a window of upcoming
 * segments. The window grows with the consumption rate so that fast playback
 * (e.g. 4x) stays buffered while normal playback transcodes little ahead.
 * ========================================================================= */

static int readahead_target(void)
{
    /* segments needed to cover lookahead_seconds of media at the observed
     * consumption rate; clamp into [1, max_buffer_segs]. */
    double segs = app.segs_per_sec * app.lookahead_seconds;
    int n = (int)(segs + 0.999);
    if (n < 1)
        n = 1;
    if (n > app.max_buffer_segs)
        n = app.max_buffer_segs;
    return n;
}

static void *readahead_thread(void *arg)
{
    for (;;) {
        int base, target, next = -1;

        pthread_mutex_lock(&app.lock);
        while (!app.shutdown) {
            base   = app.playhead;
            target = readahead_target();
            next   = -1;
            for (int i = base + 1; i <= base + target && i < app.nb_segments; i++) {
                if (app.segs[i].state == SEG_EMPTY) { next = i; break; }
            }
            /* honour the memory cap: don't prefetch if the cache is full */
            if (next >= 0 && app.cache_bytes < app.max_cache_bytes)
                break;
            pthread_cond_wait(&app.cond, &app.lock);
        }
        if (app.shutdown) {
            pthread_mutex_unlock(&app.lock);
            break;
        }
        pthread_mutex_unlock(&app.lock);

        /* transcode (no reference acquired -- just warm the cache) */
        cache_ensure(next, 0, NULL, NULL);
    }
    return NULL;
}

/* Record that the client requested segment idx and update the consumption
 * rate estimate, then nudge the read-ahead worker. */
static void note_request(int idx)
{
    pthread_mutex_lock(&app.lock);
    if (idx > app.playhead)
        app.playhead = idx;
    if (app.last_req_time && idx != app.last_req_idx) {
        double dt = (av_gettime_relative() - app.last_req_time) / 1e6;
        int    di = idx - app.last_req_idx;
        if (dt > 0.0 && di > 0) {
            double inst = di / dt;                /* segments per second */
            /* exponential smoothing keeps the estimate responsive but stable */
            app.segs_per_sec = app.segs_per_sec > 0
                ? app.segs_per_sec * 0.5 + inst * 0.5 : inst;
        }
    }
    app.last_req_time = av_gettime_relative();
    app.last_req_idx  = idx;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
}

/* =========================================================================
 * Minimal HTTP/1.0 server over a UNIX domain socket
 * ========================================================================= */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return AVERROR(errno);
        }
        p   += n;
        len -= n;
    }
    return 0;
}

static void send_simple(int fd, int code, const char *status,
                        const char *ctype, const char *body)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n\r\n",
                     code, status, ctype, body ? strlen(body) : 0);
    write_all(fd, hdr, n);
    if (body)
        write_all(fd, body, strlen(body));
}

static void send_master_playlist(int fd)
{
    char body[512];
    int bw = (int)(app.video_bitrate + app.audio_bitrate);
    snprintf(body, sizeof(body),
             "#EXTM3U\n"
             "#EXT-X-VERSION:3\n"
             "#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"avc1.640028,mp4a.40.2\"\n"
             "stream.m3u8\n",
             bw ? bw : 2200000);
    send_simple(fd, 200, "OK", "application/vnd.apple.mpegurl", body);
}

static void send_media_playlist(int fd)
{
    AVBPrint bp;
    int target = (int)(app.seg_duration + 0.999);

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "#EXTM3U\n");
    av_bprintf(&bp, "#EXT-X-VERSION:3\n");
    av_bprintf(&bp, "#EXT-X-TARGETDURATION:%d\n", target > 0 ? target : 1);
    av_bprintf(&bp, "#EXT-X-MEDIA-SEQUENCE:0\n");
    av_bprintf(&bp, "#EXT-X-PLAYLIST-TYPE:VOD\n");
    for (int i = 0; i < app.nb_segments; i++) {
        double d = FFMIN((i + 1) * app.seg_duration, app.duration) - i * app.seg_duration;
        av_bprintf(&bp, "#EXTINF:%.3f,\n", d);
        av_bprintf(&bp, "seg-%d.ts\n", i);
    }
    av_bprintf(&bp, "#EXT-X-ENDLIST\n");

    if (!av_bprint_is_complete(&bp)) {
        send_simple(fd, 500, "Internal Server Error", "text/plain", "oom\n");
    } else {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 200 OK\r\n"
                         "Content-Type: application/vnd.apple.mpegurl\r\n"
                         "Content-Length: %u\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n\r\n", bp.len);
        write_all(fd, hdr, n);
        write_all(fd, bp.str, bp.len);
    }
    av_bprint_finalize(&bp, NULL);
}

static void send_index_page(int fd)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<title>ffhls</title></head><body style='margin:0;background:#000'>"
        "<video id=v controls autoplay playsinline style='width:100%;height:100%'></video>"
        "<script src='https://cdn.jsdelivr.net/npm/hls.js@1/dist/hls.min.js'></script>"
        "<script>var v=document.getElementById('v');var src='master.m3u8';"
        "if(v.canPlayType('application/vnd.apple.mpegurl')){v.src=src;}"
        "else if(window.Hls&&Hls.isSupported()){var h=new Hls();h.loadSource(src);"
        "h.attachMedia(v);}</script></body></html>";
    send_simple(fd, 200, "OK", "text/html", page);
}

static void send_segment(int fd, int idx, int64_t range_start, int64_t range_end)
{
    uint8_t *data = NULL;
    int size = 0, ret;
    char hdr[512];
    int64_t start, end, len;
    int code;

    note_request(idx);

    ret = cache_ensure(idx, 1, &data, &size);
    if (ret < 0 || !data) {
        send_simple(fd, 500, "Internal Server Error", "text/plain",
                    "segment transcode failed\n");
        return;
    }

    start = 0;
    end   = size - 1;
    code  = 200;
    if (range_start >= 0) {                 /* byte-range request (seeking) */
        start = range_start;
        end   = (range_end >= 0 && range_end < size) ? range_end : size - 1;
        if (start >= size || start > end) {
            int n = snprintf(hdr, sizeof(hdr),
                             "HTTP/1.0 416 Range Not Satisfiable\r\n"
                             "Content-Range: bytes */%d\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Connection: close\r\n\r\n", size);
            write_all(fd, hdr, n);
            cache_release(idx);
            return;
        }
        code = 206;
    }
    len = end - start + 1;

    if (code == 206) {
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 206 Partial Content\r\n"
                         "Content-Type: video/mp2t\r\n"
                         "Content-Length: %" PRId64 "\r\n"
                         "Content-Range: bytes %" PRId64 "-%" PRId64 "/%d\r\n"
                         "Accept-Ranges: bytes\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n\r\n",
                         len, start, end, size);
        write_all(fd, hdr, n);
    } else {
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 200 OK\r\n"
                         "Content-Type: video/mp2t\r\n"
                         "Content-Length: %" PRId64 "\r\n"
                         "Accept-Ranges: bytes\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n\r\n", len);
        write_all(fd, hdr, n);
    }
    write_all(fd, data + start, len);
    cache_release(idx);
}

/* Parse "Range: bytes=START-END". Returns 1 if a range was found. */
static int parse_range(const char *req, int64_t *start, int64_t *end)
{
    const char *p = av_stristr(req, "\nRange:");
    if (!p)
        p = av_stristr(req, "\nrange:");
    if (!p)
        return 0;
    p = av_stristr(p, "bytes=");
    if (!p)
        return 0;
    p += 6;
    *start = -1;
    *end   = -1;
    if (*p != '-')
        *start = strtoll(p, NULL, 10);
    p = strchr(p, '-');
    if (p && p[1] && p[1] != '\r' && p[1] != '\n')
        *end = strtoll(p + 1, NULL, 10);
    return 1;
}

static void *connection_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[4096];
    size_t off = 0;
    char method[8] = {0}, path[1024] = {0};
    int64_t rstart = -1, rend = -1;
    int seg;

    /* read request headers (until CRLFCRLF or buffer full) */
    while (off < sizeof(req) - 1) {
        ssize_t n = read(fd, req + off, sizeof(req) - 1 - off);
        if (n <= 0)
            break;
        off += n;
        req[off] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[off] = 0;

    if (sscanf(req, "%7s %1023s", method, path) != 2) {
        send_simple(fd, 400, "Bad Request", "text/plain", "bad request\n");
        goto done;
    }
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        send_simple(fd, 405, "Method Not Allowed", "text/plain", "no\n");
        goto done;
    }
    parse_range(req, &rstart, &rend);

    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
        send_index_page(fd);
    else if (!strcmp(path, "/master.m3u8"))
        send_master_playlist(fd);
    else if (!strcmp(path, "/stream.m3u8") || !strcmp(path, "/playlist.m3u8"))
        send_media_playlist(fd);
    else if (sscanf(path, "/seg-%d.ts", &seg) == 1) {
        if (seg < 0 || seg >= app.nb_segments)
            send_simple(fd, 404, "Not Found", "text/plain", "no such segment\n");
        else
            send_segment(fd, seg, rstart, rend);
    } else {
        send_simple(fd, 404, "Not Found", "text/plain", "not found\n");
    }

done:
    close(fd);
    return NULL;
}

/* =========================================================================
 * Startup
 * ========================================================================= */

static int probe_input(void)
{
    AVFormatContext *ic = NULL;
    int ret, v, a;

    ret = avformat_open_input(&ic, app.input_url, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input '%s': %s\n",
               app.input_url, av_err2str(ret));
        return ret;
    }
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) {
        avformat_close_input(&ic);
        return ret;
    }

    v = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    a = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    app.has_video = v >= 0;
    app.has_audio = a >= 0;
    if (v >= 0) {
        app.width  = ic->streams[v]->codecpar->width;
        app.height = ic->streams[v]->codecpar->height;
    }
    if (ic->duration == AV_NOPTS_VALUE || ic->duration <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Input has unknown duration; VOD HLS needs a "
               "seekable input of known length.\n");
        avformat_close_input(&ic);
        return AVERROR_INVALIDDATA;
    }
    app.duration    = ic->duration / (double)AV_TIME_BASE;
    app.nb_segments = (int)(app.duration / app.seg_duration);
    if (app.nb_segments * app.seg_duration < app.duration - 1e-6)
        app.nb_segments++;
    if (app.nb_segments < 1)
        app.nb_segments = 1;

    av_log(NULL, AV_LOG_INFO,
           "Input: %s | duration %.2fs | %s%s | %d segments of ~%.1fs\n",
           app.input_url, app.duration,
           app.has_video ? "video " : "", app.has_audio ? "audio" : "",
           app.nb_segments, app.seg_duration);

    avformat_close_input(&ic);
    return 0;
}

static int setup_socket(void)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(app.socket_path) >= sizeof(addr.sun_path)) {
        av_log(NULL, AV_LOG_ERROR, "Socket path too long\n");
        return AVERROR(EINVAL);
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return AVERROR(errno);

    unlink(app.socket_path);    /* clear a stale socket from a previous run */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    av_strlcpy(addr.sun_path, app.socket_path, sizeof(addr.sun_path));
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "bind(%s): %s\n", app.socket_path, strerror(errno));
        close(fd);
        return AVERROR(errno);
    }
    if (listen(fd, 64) < 0) {
        av_log(NULL, AV_LOG_ERROR, "listen: %s\n", strerror(errno));
        close(fd);
        return AVERROR(errno);
    }
    return fd;
}

static void handle_signal(int sig)
{
    got_signal = sig;
    if (listen_fd >= 0)
        close(listen_fd);
    listen_fd = -1;
}

static void usage(void)
{
    printf("ffhls: serve in-memory HLS over a UNIX domain socket.\n\n"
           "Usage: ffhls -i INPUT -unix_socket PATH [options]\n\n"
           "  -i INPUT             input media file or URL (required)\n"
           "  -unix_socket PATH    UNIX domain socket to bind/listen on (required)\n"
           "  -seg_duration SEC    target segment duration (default 4)\n"
           "  -buffer_segments N   max read-ahead window in segments (default 6)\n"
           "  -max_memory MB       in-memory segment cache cap (default 96)\n"
           "  -lookahead SEC       media time to keep buffered ahead (default 8)\n"
           "  -vb BPS              video bitrate (default 2000000)\n"
           "  -ab BPS              audio bitrate (default 128000)\n"
           "  -vcodec NAME         override video encoder (default libx264/H.264)\n"
           "  -h                   show this help\n\n"
           "Endpoints: /  /master.m3u8  /stream.m3u8  /seg-N.ts\n");
}

void show_help_default(const char *opt, const char *arg)
{
    usage();
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "-h") || !strcmp(a, "-help") || !strcmp(a, "--help")) {
            usage();
            exit(0);
        }
#define NEED_VAL() do { if (!val) { av_log(NULL, AV_LOG_ERROR, "%s needs an argument\n", a); return AVERROR(EINVAL); } } while (0)
        if (!strcmp(a, "-i")) {
            NEED_VAL(); app.input_url = val; i++;
        } else if (!strcmp(a, "-unix_socket") || !strcmp(a, "-unix") || !strcmp(a, "-listen")) {
            NEED_VAL(); app.socket_path = val; i++;
        } else if (!strcmp(a, "-seg_duration")) {
            NEED_VAL(); app.seg_duration = atof(val); i++;
        } else if (!strcmp(a, "-buffer_segments")) {
            NEED_VAL(); app.max_buffer_segs = atoi(val); i++;
        } else if (!strcmp(a, "-max_memory")) {
            NEED_VAL(); app.max_cache_bytes = (int64_t)atoll(val) * 1024 * 1024; i++;
        } else if (!strcmp(a, "-lookahead")) {
            NEED_VAL(); app.lookahead_seconds = atof(val); i++;
        } else if (!strcmp(a, "-vb")) {
            NEED_VAL(); app.video_bitrate = atoll(val); i++;
        } else if (!strcmp(a, "-ab")) {
            NEED_VAL(); app.audio_bitrate = atoll(val); i++;
        } else if (!strcmp(a, "-vcodec")) {
            NEED_VAL(); app.vcodec_name = val; i++;
        } else if (!strcmp(a, "-loglevel") || !strcmp(a, "-v")) {
            NEED_VAL(); i++;    /* handled by parse_loglevel below */
        } else {
            av_log(NULL, AV_LOG_ERROR, "Unknown option: %s\n", a);
            return AVERROR(EINVAL);
        }
#undef NEED_VAL
    }

    if (app.seg_duration <= 0)
        app.seg_duration = 4.0;
    if (app.max_buffer_segs <= 0)
        app.max_buffer_segs = 6;
    if (app.max_cache_bytes <= 0)
        app.max_cache_bytes = (int64_t)96 * 1024 * 1024;
    if (app.lookahead_seconds <= 0)
        app.lookahead_seconds = 8.0;
    if (app.video_bitrate <= 0)
        app.video_bitrate = 2000000;
    if (app.audio_bitrate <= 0)
        app.audio_bitrate = 128000;

    if (!app.input_url || !app.socket_path) {
        av_log(NULL, AV_LOG_ERROR, "Both -i and -unix_socket are required.\n");
        usage();
        return AVERROR(EINVAL);
    }
    return 0;
}

static const OptionDef options[] = { { NULL } };

int main(int argc, char **argv)
{
    pthread_t ra_thread;
    int ret, ra_started = 0;

    init_dynload();
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    if ((ret = parse_args(argc, argv)) < 0)
        return 1;

    avformat_network_init();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if ((ret = probe_input()) < 0)
        goto fail;

    app.segs = av_calloc(app.nb_segments, sizeof(*app.segs));
    if (!app.segs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    pthread_mutex_init(&app.lock, NULL);
    pthread_cond_init(&app.cond, NULL);

    listen_fd = setup_socket();
    if (listen_fd < 0) {
        ret = listen_fd;
        goto fail;
    }

    if (pthread_create(&ra_thread, NULL, readahead_thread, NULL) == 0)
        ra_started = 1;

    av_log(NULL, AV_LOG_INFO, "ffhls listening on unix:%s\n", app.socket_path);
    av_log(NULL, AV_LOG_INFO, "Connect a browser via an HTTP proxy/forwarder, "
           "or test with: curl --unix-socket %s http://localhost/master.m3u8\n",
           app.socket_path);

    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (got_signal)
                break;
            if (errno == EINTR)
                continue;
            av_log(NULL, AV_LOG_ERROR, "accept: %s\n", strerror(errno));
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, connection_thread, (void*)(intptr_t)cfd) == 0)
            pthread_detach(t);
        else
            close(cfd);
    }

    ret = 0;

fail:
    pthread_mutex_lock(&app.lock);
    app.shutdown = 1;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
    if (ra_started)
        pthread_join(ra_thread, NULL);

    if (listen_fd >= 0)
        close(listen_fd);
    if (app.socket_path)
        unlink(app.socket_path);
    if (app.segs) {
        for (int i = 0; i < app.nb_segments; i++)
            av_freep(&app.segs[i].data);
        av_freep(&app.segs);
    }
    avformat_network_deinit();
    return ret < 0 ? 1 : 0;
}
