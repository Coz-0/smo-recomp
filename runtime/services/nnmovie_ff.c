/* The movie reader on FFmpeg (nnmovie_src.h; Linux): what Media Foundation's source reader is to nnmovie_mf.c on Windows
 * and AVFoundation's asset reader on macOS (nnmovie_av.m). The file the game read is copied (the caller frees its buffer
 * once mv_open returns, as SHCreateMemStream copies it on Windows) and demuxed from that copy (libavformat over a memory
 * reader), its tracks are described as the Windows reader describes them, and libavcodec
 * decodes the selected ones: video to NV12 (libswscale; the format Media Foundation's H.264 decoder gives), audio to
 * interleaved 16-bit PCM at the track's own rate and channel count (libswresample).
 * One demuxer feeds every track: a packet of a selected track is decoded at once and its samples wait in that track's
 * queue until the game reads them; packets of other tracks are dropped. A seek goes to the key frame at or before the
 * time and drops what decodes before it. A track selected after reading began starts at the demuxer's position.
 * Audio comes in blocks of at most 1024 frames (an AAC frame, what Media Foundation hands out), with their own times. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include "nnmovie_src.h"

#define AUDIO_BLOCK 1024                              /* frames */
#define IO_BUF 65536

typedef struct Sample { struct Sample *next; int is_video; int64_t us; size_t len; unsigned char *data; } Sample;
typedef struct {
    int stream;                                       /* index in the container */
    AVCodecContext *dec;                              /* NULL: not selected */
    struct SwsContext *sws;
    SwrContext *swr;
    Sample *head, *tail;                              /* decoded, not yet read */
} Track;

struct MvSrc {
    unsigned char *data;                              /* our copy of the file, freed by mv_close */
    size_t size, pos;
    AVFormatContext *fmt;
    AVIOContext *io;
    AVPacket *pkt;
    AVFrame *frame;
    MvTrack info[MV_MAX_TRACKS];
    Track tr[MV_MAX_TRACKS];
    int ntracks, eof;
    int64_t skip_before_us;                           /* after a seek: samples before it are dropped */
};

static int io_read(void *opaque, uint8_t *buf, int n) {
    MvSrc *s = opaque;
    size_t left = s->size - s->pos, k = (size_t)n < left ? (size_t)n : left;
    if (!k) return AVERROR_EOF;
    memcpy(buf, s->data + s->pos, k);
    s->pos += k;
    return (int)k;
}
static int64_t io_seek(void *opaque, int64_t off, int whence) {
    MvSrc *s = opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)s->size;
    int64_t base = (whence & 3) == SEEK_CUR ? (int64_t)s->pos : (whence & 3) == SEEK_END ? (int64_t)s->size : 0;
    if (base + off < 0 || base + off > (int64_t)s->size) return -1;
    s->pos = (size_t)(base + off);
    return (int64_t)s->pos;
}

static const char *mime_of(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
    case AV_CODEC_ID_OPUS: return "audio/opus";
    case AV_CODEC_ID_MP3: return "audio/mpeg";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    case AV_CODEC_ID_PCM_S16LE: return "audio/raw";
    default: return NULL;
    }
}

MvSrc *mv_open(const unsigned char *data, size_t size, const char *path, int64_t *duration_us, MvTrack *tracks, int *ntracks) {
    MvSrc *s = calloc(1, sizeof *s);
    s->data = malloc(size ? size : 1); s->size = size;
    if (!s->data) { printf("movie: FFmpeg: out of memory for %s\n", path); free(s); return NULL; }
    memcpy(s->data, data, size);
    unsigned char *buf = av_malloc(IO_BUF);
    s->io = buf ? avio_alloc_context(buf, IO_BUF, 0, s, io_read, NULL, io_seek) : NULL;
    s->fmt = avformat_alloc_context();
    if (!s->io || !s->fmt) { printf("movie: FFmpeg: out of memory for %s\n", path); goto fail; }
    s->fmt->pb = s->io;
    int r = avformat_open_input(&s->fmt, NULL, NULL, NULL);
    if (r < 0 || (r = avformat_find_stream_info(s->fmt, NULL)) < 0) { printf("movie: FFmpeg cannot read %s: %s\n", path, av_err2str(r)); goto fail; }
    *duration_us = s->fmt->duration > 0 ? s->fmt->duration * 1000000 / AV_TIME_BASE : 0;
    *ntracks = 0;
    for (unsigned i = 0; i < s->fmt->nb_streams && *ntracks < MV_MAX_TRACKS; i++) {
        const AVStream *st = s->fmt->streams[i];
        const AVCodecParameters *cp = st->codecpar;
        MvTrack *t = &tracks[*ntracks];
        const char *mime = mime_of(cp->codec_id);
        memset(t, 0, sizeof *t);
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            t->kind = 0; t->w = cp->width; t->h = cp->height;
            t->fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0;
            snprintf(t->mime, sizeof t->mime, "%s", mime ? mime : "video/unknown");
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            t->kind = 1; t->rate = cp->sample_rate; t->ch = cp->ch_layout.nb_channels;
            snprintf(t->mime, sizeof t->mime, "%s", mime ? mime : "audio/unknown");
        } else {
            t->kind = 2;
            snprintf(t->mime, sizeof t->mime, "application/octet-stream");
        }
        s->tr[*ntracks].stream = (int)i;
        (*ntracks)++;
    }
    s->ntracks = *ntracks;
    memcpy(s->info, tracks, sizeof(MvTrack) * (size_t)s->ntracks);
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    s->skip_before_us = INT64_MIN;
    return s;
fail:
    if (s->fmt) avformat_close_input(&s->fmt);
    if (s->io) { av_freep(&s->io->buffer); avio_context_free(&s->io); } else av_free(buf);
    free(s->data);
    free(s);
    return NULL;
}

static void queue_clear(Track *t) {
    for (Sample *x = t->head, *n; x; x = n) { n = x->next; free(x->data); free(x); }
    t->head = t->tail = NULL;
}
static void queue_push(Track *t, Sample *x) {
    x->next = NULL;
    if (t->tail) t->tail->next = x; else t->head = x;
    t->tail = x;
}
static void track_close(Track *t) {
    queue_clear(t);
    avcodec_free_context(&t->dec);
    sws_freeContext(t->sws); t->sws = NULL;
    swr_free(&t->swr);
}

int mv_select(MvSrc *s, int track, int on, MvTrack *t) {
    Track *k = &s->tr[track];
    if (on && t->kind == 2) return -2;                /* no decoder for data tracks (Media Foundation refuses them too) */
    if (!on) { track_close(k); return 0; }
    if (k->dec) return 0;
    const AVStream *st = s->fmt->streams[k->stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec || !(k->dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(k->dec, st->codecpar) < 0 ||
        avcodec_open2(k->dec, codec, NULL) < 0) {
        printf("movie: track %d (%s): no FFmpeg decoder; track left unselected\n", track, t->mime);
        avcodec_free_context(&k->dec);
        return -1;
    }
    k->dec->pkt_timebase = st->time_base;
    if (t->kind == 1) {                               /* the output: 16-bit interleaved at the track's own rate and channels */
        t->rate = k->dec->sample_rate; t->ch = k->dec->ch_layout.nb_channels;
        if (swr_alloc_set_opts2(&k->swr, &k->dec->ch_layout, AV_SAMPLE_FMT_S16, t->rate, &k->dec->ch_layout, k->dec->sample_fmt, t->rate, 0, NULL) < 0 ||
            swr_init(k->swr) < 0) { printf("movie: track %d: no audio converter\n", track); track_close(k); return -1; }
    }
    s->info[track] = *t;
    return 0;
}

static int64_t us_of(const MvSrc *s, const Track *k, int64_t ts) {
    return ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts, s->fmt->streams[k->stream]->time_base, (AVRational){1, 1000000});
}

/* every frame the decoder has ready, into the track's queue */
static int drain(MvSrc *s, int track) {
    Track *k = &s->tr[track];
    MvTrack *t = &s->info[track];
    for (;;) {
        int r = avcodec_receive_frame(k->dec, s->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
        if (r < 0) { printf("movie: track %d: decoding failed: %s\n", track, av_err2str(r)); return -1; }
        int64_t us = us_of(s, k, s->frame->best_effort_timestamp);
        if (t->kind == 0) {
            if (us >= s->skip_before_us) {
                int w = t->w, h = t->h;
                size_t ysize = (size_t)w * (size_t)h, len = ysize + (size_t)w * (size_t)((h + 1) / 2);
                k->sws = sws_getCachedContext(k->sws, s->frame->width, s->frame->height, s->frame->format, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
                Sample *x = calloc(1, sizeof *x);
                x->data = malloc(len);
                if (!k->sws || !x->data) { free(x->data); free(x); av_frame_unref(s->frame); return -1; }
                uint8_t *planes[2] = {x->data, x->data + ysize};
                int strides[2] = {w, w};
                sws_scale(k->sws, (const uint8_t *const *)s->frame->data, s->frame->linesize, 0, s->frame->height, planes, strides);
                x->is_video = 1; x->us = us; x->len = len;
                queue_push(k, x);
            }
        } else {
            size_t bpf = (size_t)t->ch * 2;
            int frames = swr_get_out_samples(k->swr, s->frame->nb_samples);
            unsigned char *pcm = frames > 0 ? malloc((size_t)frames * bpf) : NULL;
            uint8_t *out[1] = {pcm};
            int got = pcm ? swr_convert(k->swr, out, frames, (const uint8_t **)s->frame->extended_data, s->frame->nb_samples) : 0;
            for (int off = 0; got > 0 && off < got; off += AUDIO_BLOCK) {
                int n = got - off < AUDIO_BLOCK ? got - off : AUDIO_BLOCK;
                int64_t bus = us + (int64_t)off * 1000000 / (t->rate ? t->rate : 48000);
                if (bus < s->skip_before_us) continue;
                Sample *x = calloc(1, sizeof *x);
                x->len = (size_t)n * bpf;
                x->data = malloc(x->len);
                memcpy(x->data, pcm + (size_t)off * bpf, x->len);
                x->us = bus;
                queue_push(k, x);
            }
            free(pcm);
        }
        av_frame_unref(s->frame);
    }
}

/* one more packet through its track's decoder (or, at the end, every decoder flushed): 0 when nothing is left */
static int feed(MvSrc *s) {
    if (s->eof) return 0;
    int r = av_read_frame(s->fmt, s->pkt);
    if (r < 0) {
        s->eof = 1;
        for (int i = 0; i < s->ntracks; i++) if (s->tr[i].dec) { avcodec_send_packet(s->tr[i].dec, NULL); drain(s, i); }
        return 1;
    }
    for (int i = 0; i < s->ntracks; i++) {
        if (s->tr[i].stream != s->pkt->stream_index || !s->tr[i].dec) continue;
        if (avcodec_send_packet(s->tr[i].dec, s->pkt) == AVERROR(EAGAIN)) {   /* the decoder is full: empty it, then send again */
            drain(s, i);
            avcodec_send_packet(s->tr[i].dec, s->pkt);
        }
        drain(s, i);
    }
    av_packet_unref(s->pkt);
    return 1;
}

int mv_read(MvSrc *s, int track, void **sample, int64_t *us) {
    Track *k = &s->tr[track];
    if (!k->dec) return 0;
    while (!k->head) if (!feed(s)) return 0;
    Sample *x = k->head;
    k->head = x->next;
    if (!k->head) k->tail = NULL;
    *sample = x; *us = x->us;
    return 1;
}

int mv_seek(MvSrc *s, int64_t us) {
    if (us < 0) us = 0;
    int r = av_seek_frame(s->fmt, -1, av_rescale(us, AV_TIME_BASE, 1000000), AVSEEK_FLAG_BACKWARD);
    if (r < 0) { printf("movie: seek to %lld us: %s\n", (long long)us, av_err2str(r)); return -1; }
    for (int i = 0; i < s->ntracks; i++) { queue_clear(&s->tr[i]); if (s->tr[i].dec) avcodec_flush_buffers(s->tr[i].dec); }
    s->eof = 0;
    s->skip_before_us = us;
    return 0;
}

void mv_release(void *sample) {
    Sample *x = sample;
    if (!x) return;
    free(x->data);
    free(x);
}

size_t mv_copy(void *sample, int is_video, int w, int h, unsigned char *dst, size_t cap, size_t *need) {
    Sample *x = sample;
    (void)w; (void)h;                                 /* a video sample is already the track's size */
    *need = x->len;
    if (is_video && x->is_video && cap < x->len) return 0;
    size_t n = x->len < cap ? x->len : cap;
    memcpy(dst, x->data, n);
    return n;
}

void mv_close(MvSrc *s) {
    for (int i = 0; i < s->ntracks; i++) track_close(&s->tr[i]);
    av_packet_free(&s->pkt);
    av_frame_free(&s->frame);
    avformat_close_input(&s->fmt);
    if (s->io) { av_freep(&s->io->buffer); avio_context_free(&s->io); }
    free(s->data);
    free(s);
}
