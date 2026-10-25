/* services/nnmovie_av.m, the movie reader on AVFoundation (nnmovie_src.h), without the game: the test writes an MP4 of
 * its own with AVAssetWriter (H.264 video whose frame k is a flat grey of level 40 + 2k, so every frame says which one it
 * is; AAC stereo sound at 48 kHz), then reads it as nnmovie_mf.c does: tracks described, both selected, samples taken in
 * time order, copied out as packed NV12 / 16-bit PCM; a seek; a track selected and one unselected while reading (no frame
 * twice, none lost); a file that is not a movie. Then the same movie through nnmovie_mf.c's movie:: protocol, the real
 * Extractor and Decoder objects driven as the game's feed and render threads drive them (Acquire/Get/Read/Send/Advance,
 * AcquireOutput/GetOutputBuffer/Release, events). Exit code = failures. */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "nnmovie_src.h"
#include "runtime.h"
#include "hostrt.h"

/* ---- stand-ins for the rest of the runtime (nnmovie_mf.c's needs) */
void call_guest(Ctx *c, uint64_t fn) { (void)c; printf("unexpected guest call %llx\n", (unsigned long long)fn); exit(98); }
int fs_host_path(const char *guest, char *out, size_t n) { (void)guest; (void)out; (void)n; return 0; }
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; return (uint64_t)(uintptr_t)fn; }
void input_mark(const char *what) { (void)what; }
static int g_events[4];
void os_signal_event(uint64_t key) { if (key >= 1 && key <= 4) g_events[key - 1]++; }
static const ImportDef *g_defs; static int g_ndefs;
void register_imports(const ImportDef *defs, int count) { g_defs = defs; g_ndefs = count; }
void register_movie_mf_imports(void);
static ImportFn imp(const char *name) { for (int i = 0; i < g_ndefs; i++) if (!strcmp(g_defs[i].name, name)) return g_defs[i].fn; printf("no import %s\n", name); exit(97); }
static uint64_t vcall(uint64_t obj, int slot, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    Ctx c; memset(&c, 0, sizeof c);
    c.x[0] = obj; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[4] = a4; c.x[5] = a5;
    ((ImportFn)(uintptr_t)((uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)obj)[slot])(&c);
    return c.x[0];
}
static void fcall(const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    Ctx c; memset(&c, 0, sizeof c);
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[4] = a4;
    imp(name)(&c);
}
enum { E_SETPATH = 2, E_COUNT = 4, E_CONF, E_SELECT, E_UNSELECT, E_SEEK, E_AVAIL, E_READ, E_ADVANCE, E_TIME };
enum { D_CONFIGURE = 2, D_EVENTS, D_OUTFMT, D_START, D_STOP, D_RESET, D_FLUSH, D_ACQ_IN, D_GET_IN, D_SEND, D_ACQ_OUT, D_GET_OUT, D_OUTSIZE, D_RELEASE };

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 320
#define H 240
#define FPS 30
#define FRAMES 64
#define RATE 48000
static int level_of(int k) { return 40 + 2 * k; }

/* ---- the test movie */
static int write_movie(const char *path) {
    unlink(path);
    NSError *err = nil;
    AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:@(path)] fileType:AVFileTypeMPEG4 error:&err];
    if (!w) return 0;
    AVAssetWriterInput *vi = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @W, AVVideoHeightKey: @H,
        AVVideoCompressionPropertiesKey: @{AVVideoAverageBitRateKey: @4000000, AVVideoMaxKeyFrameIntervalKey: @15, AVVideoAllowFrameReorderingKey: @NO}}];
    AudioChannelLayout lay = {.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo};
    AVAssetWriterInput *ai = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
        AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @RATE, AVNumberOfChannelsKey: @2, AVEncoderBitRateKey: @128000,
        AVChannelLayoutKey: [NSData dataWithBytes:&lay length:sizeof lay]}];
    vi.expectsMediaDataInRealTime = NO; ai.expectsMediaDataInRealTime = NO;
    AVAssetWriterInputPixelBufferAdaptor *ad = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:vi sourcePixelBufferAttributes:@{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (id)kCVPixelBufferWidthKey: @W, (id)kCVPixelBufferHeightKey: @H}];
    [w addInput:vi]; [w addInput:ai];
    if (![w startWriting]) return 0;
    [w startSessionAtSourceTime:kCMTimeZero];
    int k = 0;
    long long af = 0, total_af = (long long)FRAMES * RATE / FPS;
    CMAudioFormatDescriptionRef afd = NULL;
    AudioStreamBasicDescription asbd = {RATE, kAudioFormatLinearPCM, kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    CMAudioFormatDescriptionCreate(NULL, &asbd, sizeof lay, &lay, 0, NULL, NULL, &afd);
    while (k < FRAMES || af < total_af) {
        if (k < FRAMES && vi.readyForMoreMediaData) {
            CVPixelBufferRef px = NULL;
            CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &px);
            CVPixelBufferLockBaseAddress(px, 0);
            uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(px, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(px, 1);
            for (int r = 0; r < H; r++) memset(y + r * CVPixelBufferGetBytesPerRowOfPlane(px, 0), level_of(k), W);
            for (int r = 0; r < H / 2; r++) memset(uv + r * CVPixelBufferGetBytesPerRowOfPlane(px, 1), 128, W);
            CVPixelBufferUnlockBaseAddress(px, 0);
            [ad appendPixelBuffer:px withPresentationTime:CMTimeMake(k, FPS)];
            CVPixelBufferRelease(px);
            k++;
        } else if (k >= FRAMES) [vi markAsFinished];
        if (af < total_af && ai.readyForMoreMediaData) {
            int n = (int)(total_af - af < 4800 ? total_af - af : 4800);
            int16_t *pcm = malloc((size_t)n * 4);
            for (int i = 0; i < n; i++) { int16_t v = (int16_t)(8000 * sin(2 * M_PI * 440.0 * (double)(af + i) / RATE)); pcm[2 * i] = v; pcm[2 * i + 1] = (int16_t)-v; }
            CMBlockBufferRef bb = NULL;
            CMBlockBufferCreateWithMemoryBlock(NULL, pcm, (size_t)n * 4, kCFAllocatorMalloc, NULL, 0, (size_t)n * 4, 0, &bb);
            CMSampleBufferRef sb = NULL;
            CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, bb, afd, n, CMTimeMake(af, RATE), NULL, &sb);
            [ai appendSampleBuffer:sb];
            CFRelease(sb); CFRelease(bb);
            af += n;
            if (af >= total_af) [ai markAsFinished];
        }
        usleep(200);
    }
    if (k >= FRAMES) [vi markAsFinished];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CFRelease(afd);
    return w.status == AVAssetWriterStatusCompleted;
}

/* ---- reading as nnmovie_mf.c's ext_fill does: the next sample of the selected tracks with the smallest time */
static unsigned char *g_frame;
static int frame_level(void *s, int *uv_ok) {           /* the mean Y of a copied frame; UV must be 128 everywhere */
    size_t need = 0, n = mv_copy(s, 1, W, H, g_frame, (size_t)W * H * 2, &need);
    if (n != need || need != (size_t)W * H + W * (H / 2)) return -1;
    long sum = 0; int uvbad = 0;
    for (int i = 0; i < W * H; i++) sum += g_frame[i];
    for (int i = 0; i < W * (H / 2); i++) uvbad += abs(g_frame[W * H + i] - 128) > 3;
    *uv_ok = uvbad == 0;
    return (int)(sum / (W * H));
}
static int index_of_level(int lv) { int k = (lv - 40 + 1) / 2; return abs(level_of(k) - lv) <= 1 ? k : -1000; }

int main(void) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        char path[512];
        snprintf(path, sizeof path, "%s/smo-test-movie.mp4", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        CHECK(write_movie(path), "an H.264 + AAC MP4 is written with AVAssetWriter (%s)", path);
        FILE *f = fopen(path, "rb");
        fseek(f, 0, SEEK_END); size_t size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc(size);
        fread(data, 1, size, f); fclose(f);
        g_frame = malloc((size_t)W * H * 2);

        /* 1. the tracks */
        MvTrack t[MV_MAX_TRACKS];
        int n = 0;
        int64_t dur = 0;
        MvSrc *s = mv_open(data, size, "content:/MovieData/test.mp4", &dur, t, &n);
        int v = -1, a = -1;
        for (int i = 0; s && i < n; i++) { if (t[i].kind == 0) v = i; if (t[i].kind == 1) a = i; }
        CHECK(s && n == 2 && v >= 0 && a >= 0, "two tracks, one video and one audio (%d)", n);
        if (!s || v < 0 || a < 0) { printf("movie test: %d of %d checks passed\n", g_pass, g_pass + g_fail); return 1; }
        printf("  tracks: [%d] %s %dx%d %.2f fps, [%d] %s %d Hz %d ch, %.3f s\n", v, t[v].mime, t[v].w, t[v].h, t[v].fps, a, t[a].mime, t[a].rate, t[a].ch, dur / 1e6);
        CHECK(!strcmp(t[v].mime, "video/avc") && t[v].w == W && t[v].h == H && fabs(t[v].fps - FPS) < 0.5, "video/avc %dx%d at %d fps", W, H, FPS);
        CHECK(!strcmp(t[a].mime, "audio/mp4a-latm") && t[a].rate == RATE && t[a].ch == 2, "audio/mp4a-latm, %d Hz, 2 channels", RATE);
        CHECK(llabs(dur - (int64_t)FRAMES * 1000000 / FPS) < 100000, "duration %.3f s (%d frames at %d fps)", dur / 1e6, FRAMES, FPS);

        /* 2. the whole movie, both tracks, in time order */
        CHECK(mv_select(s, v, 1, &t[v]) == 0 && mv_select(s, a, 1, &t[a]) == 0, "both tracks selected");
        void *ahead[2] = {NULL, NULL}; int64_t aus[2] = {0, 0}; int eos[2] = {0, 0}, tr2[2] = {v, a};
        int frames = 0, order_ok = 1, content_ok = 1, uv_all = 1, blocks = 0, big = 0, gap_bad = 0;
        long long pcm_frames = 0; int64_t last_t = INT64_MIN, last_vt = -1, next_at = INT64_MIN;
        for (;;) {
            for (int j = 0; j < 2; j++) if (!ahead[j] && !eos[j]) { int r = mv_read(s, tr2[j], &ahead[j], &aus[j]); if (r != 1) { eos[j] = 1; ahead[j] = NULL; } }
            int b = !ahead[0] ? (ahead[1] ? 1 : -1) : !ahead[1] ? 0 : aus[0] <= aus[1] ? 0 : 1;
            if (b < 0) break;
            if (aus[b] < last_t) order_ok = 0;
            last_t = aus[b];
            if (b == 0) {
                int uv = 0, lv = frame_level(ahead[0], &uv);
                int k = index_of_level(lv);
                if (k != frames || llabs(aus[0] - (int64_t)k * 1000000 / FPS) > 1000) { if (content_ok) printf("  frame %d: level %d (frame %d), t=%.4f\n", frames, lv, k, aus[0] / 1e6); content_ok = 0; }
                uv_all &= uv; last_vt = aus[0];
                frames++;
            } else {
                size_t need = 0; static unsigned char pcm[1 << 16];
                size_t got = mv_copy(ahead[1], 0, 0, 0, pcm, sizeof pcm, &need);
                if (got != need || got % 4) gap_bad++;
                if (got / 4 > 1024) big++;
                if (next_at != INT64_MIN && llabs(aus[1] - next_at) > 50) gap_bad++;
                next_at = aus[1] + (int64_t)(got / 4) * 1000000 / RATE;
                pcm_frames += (long long)(got / 4); blocks++;
            }
            mv_release(ahead[b]); ahead[b] = NULL;
        }
        printf("  read: %d video frames (last t=%.3f), %d audio blocks = %lld PCM frames (%.3f s)\n", frames, last_vt / 1e6, blocks, pcm_frames, pcm_frames / (double)RATE);
        CHECK(frames == FRAMES && content_ok, "every video frame, once, in order, at its time, with its own picture (%d of %d)", frames, FRAMES);
        CHECK(uv_all, "the chroma plane comes out flat (128) as written");
        CHECK(order_ok, "samples come in time order across the two tracks");
        CHECK(!big && !gap_bad, "audio in blocks of at most 1024 frames, back to back in time (%d too big, %d gaps or bad sizes)", big, gap_bad);
        CHECK(llabs(pcm_frames - (long long)FRAMES * RATE / FPS) <= 2048, "audio length %.3f s (the movie is %.3f s; AAC priming aside)", pcm_frames / (double)RATE, FRAMES / (double)FPS);

        /* 3. seek to 1.0 s: video starts there, with the picture of that time */
        CHECK(mv_seek(s, 1000000) == 0, "SeekTo(1.0 s)");
        void *x = NULL; int64_t us = 0, ausk = 0; void *y = NULL;
        int r = mv_read(s, v, &x, &us);
        int uv = 0, lv = r == 1 ? frame_level(x, &uv) : -1;
        int r2 = mv_read(s, a, &y, &ausk);
        printf("  after the seek: first video frame t=%.4f (level %d = frame %d), first audio t=%.4f\n", us / 1e6, lv, index_of_level(lv), ausk / 1e6);
        CHECK(r == 1 && us >= 1000000 - 34000 && us <= 1000000 + 34000 && index_of_level(lv) == (int)((us * FPS + 500000) / 1000000), "the first frame after the seek is the one at 1.0 s, with its own picture");
        CHECK(r2 == 1 && ausk >= 1000000 - 25000 && ausk <= 1000000 + 25000, "audio after the seek starts at 1.0 s (%.4f)", ausk / 1e6);
        if (x) mv_release(x);
        if (y) mv_release(y);

        /* 4. audio unselected, then selected again, while reading: video goes on without a frame twice or one lost */
        mv_seek(s, 0);
        int next = 0, seq_ok = 1;
        for (int i = 0; i < 40; i++) {
            if (i == 10) mv_select(s, a, 0, &t[a]);
            if (i == 25) mv_select(s, a, 1, &t[a]);
            if (mv_read(s, v, &x, &us) != 1) { seq_ok = 0; break; }
            int k = index_of_level(frame_level(x, &uv));
            mv_release(x);
            if (k != next) { if (seq_ok) printf("  frame %d came as %d\n", next, k); seq_ok = 0; }
            next = k + 1;
            if (i >= 25 && i % 3 == 0 && mv_read(s, a, &y, &ausk) == 1) mv_release(y);
        }
        CHECK(seq_ok, "selection changes while reading: video frames 0..39 each once, in order");
        r2 = mv_read(s, a, &y, &ausk);
        CHECK(r2 == 1 && ausk >= 25 * 1000000 / FPS - 40000, "audio selected again goes on from where the video was (t=%.3f)", ausk / 1e6);
        if (y) mv_release(y);
        mv_close(s);

        /* 5. not a movie */
        unsigned char junk[4096];
        for (int i = 0; i < 4096; i++) junk[i] = (unsigned char)(i * 37);
        CHECK(mv_open(junk, sizeof junk, "content:/MovieData/junk.mp4", &dur, t, &n) == NULL, "data that is not a movie is refused");

        /* 6. the movie:: protocol of nnmovie_mf.c on this reader, as the game drives it */
        {
            register_movie_mf_imports();
            uint64_t ex[2] = {0, 0}, md[2][1] = {{0}, {0}}, dv[2] = {0, 0}, dd[2][2] = {{0, 0}, {0, 0}};
            uint64_t events[2][4] = {{1, 2, 3, 4}, {1, 2, 3, 4}};
            fcall("_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", (uint64_t)(uintptr_t)ex, 1, 0, 0, 0);
            uint64_t E = (uint64_t)(uintptr_t)ex;
            CHECK(vcall(E, E_SETPATH, (uint64_t)(uintptr_t)path, 0, 0, 0, 0) == 0, "Extractor::SetDataSource(%s)", path);
            uint32_t count = 0;
            vcall(E, E_COUNT, (uint64_t)(uintptr_t)&count, 0, 0, 0, 0);
            CHECK(count == 2, "Extractor::GetTrackCount: %u", count);
            int vt = -1, at = -1;
            for (int i = 0; i < 2; i++) {
                fcall("_ZN5movie9MediaDataC1Ev", (uint64_t)(uintptr_t)md[i], 0, 0, 0, 0);
                vcall(E, E_CONF, (uint64_t)i, (uint64_t)(uintptr_t)md[i], 0, 0, 0);
                Ctx c; memset(&c, 0, sizeof c); const char *mime = NULL;
                c.x[0] = (uint64_t)(uintptr_t)md[i]; c.x[1] = (uint64_t)(uintptr_t)"mime"; c.x[2] = (uint64_t)(uintptr_t)&mime;
                imp("_ZNK5movie9MediaData10FindStringEPKcPS2_")(&c);
                if (mime && !strncmp(mime, "video/", 6)) vt = i; else if (mime && !strncmp(mime, "audio/", 6)) at = i;
                vcall(E, E_SELECT, (uint64_t)i, 0, 0, 0, 0);
            }
            CHECK(vt >= 0 && at >= 0, "GetTrackConfiguration: a video and an audio track");
            int trk[2] = {vt, at};
            for (int j = 0; j < 2; j++) {           /* Decoder(type: 1 H.264 / 4 AAC, output format: 4 NV12 / 1, mode 1) */
                fcall("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", (uint64_t)(uintptr_t)dd[j], j ? 4 : 1, j ? 1 : 4, 1, 0);
                dv[j] = (uint64_t)(uintptr_t)dd[j];
                vcall(dv[j], D_CONFIGURE, (uint64_t)(uintptr_t)md[trk[j]], 0, 0, 0, 0);
                vcall(dv[j], D_EVENTS, (uint64_t)(uintptr_t)events[j], 0, 0, 0, 0);
                vcall(dv[j], D_START, 0, 0, 0, 0, 0);
            }
            uint32_t outsize = 0;
            vcall(dv[0], D_OUTSIZE, (uint64_t)(uintptr_t)&outsize, 0, 0, 0, 0);
            unsigned char *frame = malloc(outsize), *pcm = malloc(0x8000);
            uint8_t inbuf[0x40], outbuf[0x40];
            int vframes = 0, vok = 1, eos[2] = {0, 0}, end = 0;
            long long afr = 0;
            for (int guard = 0; guard < 100000 && !(eos[0] && eos[1]); guard++) {
                /* the feed thread: the track whose sample is next gets one of its decoder's input buffers */
                uint64_t track = 0;
                int r = !end ? (int)vcall(E, E_AVAIL, (uint64_t)(uintptr_t)&track, 0, 0, 0, 0) : 0x2000;
                if (r == 0x2000 && !end) {          /* end of data: an end-of-stream input to each decoder */
                    end = 1;
                    for (int j = 0; j < 2; j++) { int32_t idx = -1, more = 0;
                        if (vcall(dv[j], D_ACQ_IN, (uint64_t)(uintptr_t)&idx, (uint64_t)(uintptr_t)&more, 0, 0, 0) == 0) vcall(dv[j], D_SEND, (uint64_t)idx, 0, 0, 0, 4); }
                } else if (r == 0) {
                    int j = (int)track == vt ? 0 : 1;
                    int32_t idx = -1, more = 0;
                    if (vcall(dv[j], D_ACQ_IN, (uint64_t)(uintptr_t)&idx, (uint64_t)(uintptr_t)&more, 0, 0, 0) == 0) {
                        memset(inbuf, 0, sizeof inbuf);
                        vcall(dv[j], D_GET_IN, (uint64_t)idx, (uint64_t)(uintptr_t)inbuf, 0, 0, 0);
                        vcall(E, E_READ, (uint64_t)(uintptr_t)inbuf, 0, 0, 0, 0);
                        int64_t ts = 0;
                        vcall(E, E_TIME, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
                        uint32_t off, sz; memcpy(&off, inbuf, 4); memcpy(&sz, inbuf + 4, 4);
                        vcall(dv[j], D_SEND, (uint64_t)idx, off, sz, (uint64_t)ts, 0);
                        vcall(E, E_ADVANCE, 0, 0, 0, 0, 0);
                    }
                }
                /* the render side: every decoded frame / block, copied and released */
                for (int j = 0; j < 2; j++) {
                    int32_t idx = -1, more = 0; int64_t ts = 0; uint32_t fl = 0;
                    while (vcall(dv[j], D_ACQ_OUT, (uint64_t)(uintptr_t)&idx, (uint64_t)(uintptr_t)&ts, (uint64_t)(uintptr_t)&fl, (uint64_t)(uintptr_t)&more, 0) == 0) {
                        memset(outbuf, 0, sizeof outbuf);
                        uint64_t base = (uint64_t)(uintptr_t)(j ? pcm : frame), cap = j ? 0x8000 : outsize;
                        memcpy(outbuf + 0x28, &base, 8); memcpy(outbuf + 0x18, &cap, 8);
                        vcall(dv[j], D_GET_OUT, (uint64_t)idx, (uint64_t)(uintptr_t)outbuf, 0, 0, 0);
                        uint32_t got; memcpy(&got, outbuf + 4, 4);
                        if (fl & 4) eos[j] = 1;
                        else if (j == 0) {
                            long sum = 0; for (int i = 0; i < W * H; i++) sum += frame[i];
                            int k = index_of_level((int)(sum / (W * H)));
                            if (k != vframes || got != outsize) { if (vok) printf("  protocol: frame %d came as %d (%u bytes)\n", vframes, k, got); vok = 0; }
                            vframes++;
                        } else afr += got / 4;
                        vcall(dv[j], D_RELEASE, (uint64_t)idx, 0, 0, 0, 0);
                    }
                }
            }
            printf("  protocol: %d video frames, %lld PCM frames, end of stream on both: %s; events: input %d, output %d, format %d, error %d\n",
                   vframes, afr, eos[0] && eos[1] ? "yes" : "no", g_events[0], g_events[1], g_events[2], g_events[3]);
            CHECK(vframes == FRAMES && vok && outsize == (uint32_t)(W * H + W * (H / 2)), "through movie::Decoder: every video frame once, in order, as packed NV12 of %u bytes", outsize);
            CHECK(llabs(afr - (long long)FRAMES * RATE / FPS) <= 2048 && eos[0] && eos[1], "through movie::Decoder: the whole sound, and the end of stream on both decoders");
            CHECK(g_events[0] > 0 && g_events[1] > 0 && g_events[2] >= 2 && g_events[3] == 0, "the decoders signal input, output and format events, no error");
            for (int j = 0; j < 2; j++) vcall(dv[j], 0, 0, 0, 0, 0, 0);
            vcall(E, 0, 0, 0, 0, 0, 0);
            free(frame); free(pcm);
        }
        unlink(path);

        /* 7. no temporary file is left */
        char cmd[600];
        snprintf(cmd, sizeof cmd, "ls %s/smo-movie-* >/dev/null 2>&1", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        CHECK(system(cmd) != 0, "the temporary copies are gone after close");

        printf("movie test: %d of %d checks passed\n", g_pass, g_pass + g_fail);
        return g_fail != 0;
    }
}
