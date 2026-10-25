/* The movie reader on AVFoundation (nnmovie_src.h; macOS): what Media Foundation's source reader is to nnmovie_mf.c on
 * Windows. The file the game read into memory is written to a temporary file (an AVURLAsset reads a URL), its tracks are
 * described as the Windows reader describes them, and an AVAssetReader demuxes AND decodes the selected ones: video to
 * NV12 (420YpCbCr8BiPlanarVideoRange, the format Media Foundation's H.264 decoder gives), audio to interleaved 16-bit PCM
 * at the track's own rate and channel count.
 * An AVAssetReader's outputs are fixed once it reads, and it reads forward only: a seek, or a track selected or
 * unselected after reading began, makes a new reader. After a selection it starts at the earliest time any track got
 * to, and each track drops what it already delivered (no sample twice, none lost); after a seek every track starts
 * again from the seek time.
 * Audio comes in blocks of at most 1024 frames (an AAC frame, what Media Foundation hands out), with their own times. */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nnmovie_src.h"
#if !__has_feature(objc_arc)
#error "nnmovie_av.m needs ARC (-fobjc-arc): the reader keeps AVFoundation objects in a C structure"
#endif
#pragma clang diagnostic ignored "-Wdeprecated-declarations"     /* the asset's tracks and duration are read after loading them */

#define AUDIO_BLOCK 1024                              /* frames */

typedef struct { int is_video; CVPixelBufferRef px; unsigned char *pcm; size_t len; } Sample;

struct MvSrc {
    char file[1024];                                  /* the temporary copy */
    AVURLAsset *asset;
    NSArray<AVAssetTrack *> *tracks;
    MvTrack info[MV_MAX_TRACKS];
    int ntracks, selected[MV_MAX_TRACKS];
    AVAssetReader *reader;                            /* NULL: made again at the next read */
    AVAssetReaderTrackOutput *out[MV_MAX_TRACKS];
    int64_t start_us;                                 /* where the next reader starts */
    int64_t last_us[MV_MAX_TRACKS];                   /* the time of the last sample each track delivered (INT64_MIN: none) */
    int reading;                                      /* a reader has delivered samples since the last seek */
    /* audio: the decoded buffer being cut into blocks */
    CMSampleBufferRef abuf[MV_MAX_TRACKS];
    size_t aoff[MV_MAX_TRACKS];                       /* frames of abuf already handed out */
};

static int64_t us_of(CMTime t) { return CMTIME_IS_VALID(t) ? (int64_t)(CMTimeGetSeconds(t) * 1e6 + (t.value >= 0 ? 0.5 : -0.5)) : 0; }

static void wait_for(void (^start)(dispatch_semaphore_t done)) {    /* runs an asynchronous load and waits for it */
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    start(done);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

static void video_mime(FourCharCode c, char *mime) {
    strcpy(mime, c == kCMVideoCodecType_H264 ? "video/avc" : c == kCMVideoCodecType_HEVC ? "video/hevc" : c == kCMVideoCodecType_VP9 ? "video/x-vnd.on2.vp9" :
                 c == 'vp08' ? "video/x-vnd.on2.vp8" : "video/unknown");
}
static void audio_mime(AudioFormatID f, char *mime) {
    strcpy(mime, f == kAudioFormatMPEG4AAC || f == kAudioFormatMPEG4AAC_HE || f == kAudioFormatMPEG4AAC_HE_V2 ? "audio/mp4a-latm" : f == kAudioFormatOpus ? "audio/opus" :
                 f == kAudioFormatMPEGLayer3 ? "audio/mpeg" : f == kAudioFormatLinearPCM ? "audio/raw" : f == 'vorb' ? "audio/vorbis" : "audio/unknown");
}

MvSrc *mv_open(const unsigned char *data, size_t size, const char *path, int64_t *duration_us, MvTrack *tracks, int *ntracks) {
    @autoreleasepool {
        MvSrc *s = calloc(1, sizeof *s);
        const char *ext = strrchr(path, '.');
        const char *tmp = getenv("TMPDIR");
        if (!ext || strlen(ext) >= 8) ext = ".mp4";
        snprintf(s->file, sizeof s->file, "%s/smo-movie-XXXXXX%s", tmp && *tmp ? tmp : "/tmp", ext);
        int fd = mkstemps(s->file, (int)strlen(ext));
        if (fd < 0 || write(fd, data, size) != (ssize_t)size) {
            printf("movie: cannot write the temporary copy %s\n", s->file);
            if (fd >= 0) { close(fd); unlink(s->file); }
            free(s);
            return NULL;
        }
        close(fd);
        s->asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:s->file]] options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
        __block NSArray<AVAssetTrack *> *all = nil;
        __block CMTime dur = kCMTimeInvalid;
        __block NSError *err = nil;
        wait_for(^(dispatch_semaphore_t done) {
            [s->asset loadValuesAsynchronouslyForKeys:@[@"tracks", @"duration"] completionHandler:^{ dispatch_semaphore_signal(done); }];
        });
        if ([s->asset statusOfValueForKey:@"tracks" error:&err] != AVKeyValueStatusLoaded) {
            printf("movie: AVFoundation cannot read %s: %s\n", path, err ? err.localizedDescription.UTF8String : "?");
            unlink(s->file); free(s);
            return NULL;
        }
        all = s->asset.tracks;                        /* loaded above: no wait */
        dur = s->asset.duration;
        s->tracks = all;
        *duration_us = us_of(dur);
        *ntracks = 0;
        for (AVAssetTrack *t in all) {
            if (*ntracks == MV_MAX_TRACKS) break;
            wait_for(^(dispatch_semaphore_t done) {
                [t loadValuesAsynchronouslyForKeys:@[@"formatDescriptions", @"naturalSize", @"nominalFrameRate", @"mediaType"] completionHandler:^{ dispatch_semaphore_signal(done); }];
            });
            MvTrack *tr = &tracks[(*ntracks)++];
            memset(tr, 0, sizeof *tr);
            CMFormatDescriptionRef fd0 = t.formatDescriptions.count ? (__bridge CMFormatDescriptionRef)t.formatDescriptions[0] : NULL;
            if ([t.mediaType isEqualToString:AVMediaTypeVideo]) {
                tr->kind = 0;
                CMVideoDimensions dim = fd0 ? CMVideoFormatDescriptionGetDimensions(fd0) : (CMVideoDimensions){(int32_t)t.naturalSize.width, (int32_t)t.naturalSize.height};
                tr->w = dim.width; tr->h = dim.height;
                tr->fps = t.nominalFrameRate;
                video_mime(fd0 ? CMFormatDescriptionGetMediaSubType(fd0) : 0, tr->mime);
            } else if ([t.mediaType isEqualToString:AVMediaTypeAudio]) {
                tr->kind = 1;
                const AudioStreamBasicDescription *a = fd0 ? CMAudioFormatDescriptionGetStreamBasicDescription(fd0) : NULL;
                if (a) { tr->rate = (int)a->mSampleRate; tr->ch = (int)a->mChannelsPerFrame; }
                audio_mime(a ? a->mFormatID : 0, tr->mime);
            } else {
                tr->kind = 2;
                strcpy(tr->mime, "application/octet-stream");
            }
        }
        s->ntracks = *ntracks;
        memcpy(s->info, tracks, sizeof(MvTrack) * (size_t)s->ntracks);
        for (int i = 0; i < MV_MAX_TRACKS; i++) s->last_us[i] = INT64_MIN;
        return s;
    }
}

static void drop_reader(MvSrc *s) {
    for (int i = 0; i < MV_MAX_TRACKS; i++) {
        s->out[i] = nil;
        if (s->abuf[i]) { CFRelease(s->abuf[i]); s->abuf[i] = NULL; }
        s->aoff[i] = 0;
    }
    if (s->reader) [s->reader cancelReading];
    s->reader = nil;
}

/* a new reader for the selected tracks, from start_us (caller: none exists) */
static int make_reader(MvSrc *s) {
    NSError *err = nil;
    s->reader = [AVAssetReader assetReaderWithAsset:s->asset error:&err];
    if (!s->reader) { printf("movie: AVAssetReader: %s\n", err ? err.localizedDescription.UTF8String : "?"); return 0; }
    if (s->start_us > 0) s->reader.timeRange = CMTimeRangeMake(CMTimeMake(s->start_us, 1000000), kCMTimePositiveInfinity);
    for (int i = 0; i < s->ntracks; i++) {
        if (!s->selected[i]) continue;
        NSMutableDictionary *set = [NSMutableDictionary dictionary];
        if (s->info[i].kind == 0) set[(id)kCVPixelBufferPixelFormatTypeKey] = @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        else {
            [set addEntriesFromDictionary:@{AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO,
                                            AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO}];
            if (s->info[i].rate > 0) set[AVSampleRateKey] = @(s->info[i].rate);
            if (s->info[i].ch > 0) set[AVNumberOfChannelsKey] = @(s->info[i].ch);
        }
        AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:s->tracks[(NSUInteger)i] outputSettings:set];
        o.alwaysCopiesSampleData = NO;
        if (![s->reader canAddOutput:o]) { printf("movie: track %d: the asset reader takes no output for it\n", i); s->reader = nil; return 0; }
        [s->reader addOutput:o];
        s->out[i] = o;
    }
    if (![s->reader startReading]) { printf("movie: startReading: %s\n", s->reader.error ? s->reader.error.localizedDescription.UTF8String : "?"); s->reader = nil; return 0; }
    return 1;
}

int mv_select(MvSrc *s, int track, int on, MvTrack *t) {
    @autoreleasepool {
        if (on && t->kind == 2) return -2;            /* no decoder for data tracks (Media Foundation refuses them too) */
        if (on && t->kind == 0 && strcmp(t->mime, "video/unknown") == 0) { printf("movie: track %d (%s): no AVFoundation decoder; track left unselected\n", track, t->mime); return -1; }
        if (s->selected[track] == on) return 0;
        if (s->reading) {                             /* outputs are fixed while reading: a new reader where the tracks got to */
            int64_t at = INT64_MAX;
            for (int i = 0; i < s->ntracks; i++) if (s->selected[i] && s->last_us[i] != INT64_MIN && s->last_us[i] < at) at = s->last_us[i];
            s->start_us = at == INT64_MAX ? s->start_us : at;
        }
        drop_reader(s);
        s->selected[track] = on;
        if (!on) s->last_us[track] = INT64_MIN;
        return 0;
    }
}

/* the next decoded buffer of a track, or NULL (end, or error: *failed) */
static CMSampleBufferRef next_buffer(MvSrc *s, int track, int *failed) {
    *failed = 0;
    for (;;) {
        CMSampleBufferRef b = [s->out[track] copyNextSampleBuffer];
        if (!b) { *failed = s->reader.status == AVAssetReaderStatusFailed; if (*failed) printf("movie: track %d: %s\n", track, s->reader.error.localizedDescription.UTF8String); return NULL; }
        if (CMSampleBufferGetNumSamples(b) > 0 && (CMSampleBufferGetImageBuffer(b) || CMSampleBufferGetDataBuffer(b))) return b;
        CFRelease(b);                                 /* a marker without data */
    }
}

int mv_read(MvSrc *s, int track, void **sample, int64_t *us) {
    @autoreleasepool {
        if (!s->selected[track]) return 0;
        if (!s->reader && !make_reader(s)) return -1;
        if (!s->out[track]) return -1;
        for (;;) {
            int failed = 0;
            if (s->info[track].kind == 0) {
                CMSampleBufferRef b = next_buffer(s, track, &failed);
                if (!b) return failed ? -1 : 0;
                int64_t t = us_of(CMSampleBufferGetPresentationTimeStamp(b));
                if (s->last_us[track] != INT64_MIN && t <= s->last_us[track]) { CFRelease(b); continue; }   /* delivered before the new reader */
                Sample *x = calloc(1, sizeof *x);
                x->is_video = 1; x->px = CVPixelBufferRetain(CMSampleBufferGetImageBuffer(b));
                CFRelease(b);
                s->last_us[track] = t; s->reading = 1;
                *sample = x; *us = t;
                return 1;
            }
            /* audio: blocks of at most AUDIO_BLOCK frames out of the decoded buffer */
            if (!s->abuf[track]) {
                s->abuf[track] = next_buffer(s, track, &failed);
                s->aoff[track] = 0;
                if (!s->abuf[track]) return failed ? -1 : 0;
            }
            CMSampleBufferRef b = s->abuf[track];
            size_t frames = (size_t)CMSampleBufferGetNumSamples(b), bpf = (size_t)s->info[track].ch * 2;
            size_t n = frames - s->aoff[track] < AUDIO_BLOCK ? frames - s->aoff[track] : AUDIO_BLOCK;
            CMTime pts = CMSampleBufferGetPresentationTimeStamp(b);
            int64_t t = us_of(CMTimeAdd(pts, CMTimeMake((int64_t)s->aoff[track], s->info[track].rate)));
            Sample *x = NULL;
            if (!(s->last_us[track] != INT64_MIN && t <= s->last_us[track]) && n) {
                x = calloc(1, sizeof *x);
                x->len = n * bpf; x->pcm = malloc(x->len);
                CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(b);
                if (CMBlockBufferCopyDataBytes(bb, s->aoff[track] * bpf, x->len, x->pcm) != kCMBlockBufferNoErr) { free(x->pcm); free(x); x = NULL; failed = 1; }
            }
            s->aoff[track] += n;
            if (s->aoff[track] >= frames || !n) { CFRelease(b); s->abuf[track] = NULL; }
            if (failed) { printf("movie: track %d: the decoded audio cannot be read\n", track); return -1; }
            if (!x) continue;                         /* delivered before the new reader */
            s->last_us[track] = t; s->reading = 1;
            *sample = x; *us = t;
            return 1;
        }
    }
}

int mv_seek(MvSrc *s, int64_t us) {
    @autoreleasepool {
        drop_reader(s);
        s->start_us = us > 0 ? us : 0;
        s->reading = 0;
        for (int i = 0; i < MV_MAX_TRACKS; i++) s->last_us[i] = INT64_MIN;
        return 0;
    }
}

void mv_release(void *sample) {
    Sample *x = sample;
    if (!x) return;
    if (x->px) CVPixelBufferRelease(x->px);
    free(x->pcm);
    free(x);
}

size_t mv_copy(void *sample, int is_video, int w, int h, unsigned char *dst, size_t cap, size_t *need) {
    Sample *x = sample;
    if (!is_video || !x->is_video) {
        *need = x->len;
        size_t n = x->len < cap ? x->len : cap;
        if (x->pcm) memcpy(dst, x->pcm, n);
        return n;
    }
    size_t ysize = (size_t)w * (size_t)h, uvrows = (size_t)((h + 1) / 2);
    *need = ysize + (size_t)w * uvrows;
    if (cap < *need || CVPixelBufferGetPlaneCount(x->px) != 2) return 0;
    if (CVPixelBufferLockBaseAddress(x->px, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
    const unsigned char *yp = CVPixelBufferGetBaseAddressOfPlane(x->px, 0), *uvp = CVPixelBufferGetBaseAddressOfPlane(x->px, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(x->px, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(x->px, 1);
    size_t pw = CVPixelBufferGetWidthOfPlane(x->px, 0), ph = CVPixelBufferGetHeightOfPlane(x->px, 0);
    size_t cw = (size_t)w < pw ? (size_t)w : pw, chh = (size_t)h < ph ? (size_t)h : ph, cuv = uvrows < CVPixelBufferGetHeightOfPlane(x->px, 1) ? uvrows : CVPixelBufferGetHeightOfPlane(x->px, 1);
    if (cw < (size_t)w || chh < (size_t)h) memset(dst, 0, *need);    /* a frame smaller than the track says: the rest stays black-ish */
    for (size_t y = 0; y < chh; y++) memcpy(dst + y * (size_t)w, yp + y * ys, cw);
    for (size_t y = 0; y < cuv; y++) memcpy(dst + ysize + y * (size_t)w, uvp + y * uvs, cw);
    CVPixelBufferUnlockBaseAddress(x->px, kCVPixelBufferLock_ReadOnly);
    return *need;
}

void mv_close(MvSrc *s) {
    @autoreleasepool {
        drop_reader(s);
        s->asset = nil; s->tracks = nil;
        unlink(s->file);
        free(s);
    }
}
