/* smo_audio.c - SMO port: the host side of the AudioRenderer.
 *
 * SMO's sdk (NintendoSDK 3.5.1) contains the whole AudioRenderer: the client API (nn::audio::OpenAudioRenderer,
 * RequestUpdateAudioRenderer, ...), the server (nn::audio::server::AudioRendererManagerImpl and below) and the DSP
 * command processing (nn::audio::dsp). tools/lifter/sdklift.py lifts all of it (rules: sdklift_rules.txt), and the
 * game's nn::audio imports are answered by that lifted code (nnsdk2.c install_lifted). The host supplies only what is
 * below the SDK on a console:
 *
 *   CreateAudioRendererManagerByHipc   [sdk] nn::audio::OpenAudioRenderer / GetAudioRendererWorkBufferSize use it when
 *       AudioRendererParameter +0x2c != 1, i.e. when the renderer is to run in the audio system module (over IPC)
 *       rather than in the application (+0x2c == 1: CreateAudioRendererManagerByDfc, "direct function call"). The
 *       system module runs the same AudioRendererManagerImpl the sdk contains, so the host answers with the in-process
 *       manager: the lifted CreateAudioRendererManagerByDfc, called with the same registers (the sf::SharedPointer
 *       result goes through x8, the indirect-result register, untouched).
 *
 *   THE RENDERING CLOCK (the audio system module's job on a console). The SDK's in-process build has no frame loop:
 *       nothing in it calls AudioRenderSystem::SendCommandToDsp, and nn::audio::dsp::SendCommandBuffer / Start / Wait /
 *       Signal are empty stubs [sdk 0x196514 .. 0x196554]. The host therefore keeps the list of started renderers
 *       (AudioRenderSystem::Start / Stop are cut here: record, then run the lifted function) and, on its own thread,
 *       once per audio frame (sample count / sample rate: 240 / 48000 = 5 ms) calls the lifted SendCommandToDsp of each.
 *       That SDK code generates the frame's command list, hands it to the "DSP" (below) and signals the renderer's
 *       system event, which wakes the game's audio thread (nn::atk) to send its next update.
 *   THE DSP: dsp::SendCommandBuffer(session, address, size, ...) is cut here and runs the SDK's own command list
 *       processor on the CPU, as the SDK's manual-execution path does [sdk 0x188a00]: CommandListProcessor
 *       SetProcessTimeMax(-1), Setup(list, size), Process(), one processor object per session. DSP addresses are CPU
 *       addresses in the in-process build (dsp::MapUserPointer returns its argument [sdk 0x196530]).
 *   NOT YET: the sound itself. The final mix goes to the device sink; where that lands and the host audio output for it
 *       come next, so for now nothing is heard (the frames are processed and timed).
 * Lifted code reached from the frame thread stops with the name of anything it needs (sdklift_rt.c). */
#include <stdio.h>
#include <windows.h>
#include "hostrt.h"
#include "sdklift.h"

static unsigned long long g_mgr_by_hipc;

static void audio_manager_by_hipc(Ctx *c) {
    static ImportFn dfc;
    if (!dfc) dfc = sdk_lifted("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if (!dfc) guest_trap("smo_audio: CreateAudioRendererManagerByDfc was not lifted (sdklift_rules.txt)", c->x[30] - 4);
    if (g_mgr_by_hipc++ == 0)
        printf("smo_audio: the AudioRenderer manager is the in-process one (CreateAudioRendererManagerByDfc) in place of the audio system module's (IPC)\n");
    dfc(c);
}

#define RS_START "_ZN2nn5audio6server17AudioRenderSystem5StartEv"
#define RS_STOP "_ZN2nn5audio6server17AudioRenderSystem4StopEv"
#define RS_SEND "_ZN2nn5audio6server17AudioRenderSystem16SendCommandToDspEv"
#define RS_RATE "_ZNK2nn5audio6server17AudioRenderSystem13GetSampleRateEv"
#define RS_COUNT "_ZNK2nn5audio6server17AudioRenderSystem14GetSampleCountEv"
#define CLP_TIME "_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"
#define CLP_SETUP "_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm"
#define CLP_PROCESS "_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv"

#define MAX_SYS 8
static SRWLOCK g_sys_lock = SRWLOCK_INIT;
static uint64_t g_sys[MAX_SYS];                 /* started AudioRenderSystem objects */
static int g_nsys;
static HANDLE g_frame_thread;
static volatile LONG64 g_frames, g_lists, g_commands_bytes;
static uint8_t *g_proc[16];                     /* CommandListProcessor per session (host memory, zeroed) */

static ImportFn lifted(const char *name) {
    ImportFn f = sdk_lifted(name);
    if (!f) { printf("smo_audio: %s was not lifted (sdklift_rules.txt)\n", name); guest_trap("smo_audio: SDK function not lifted", 0); }
    return f;
}
static uint64_t call1(Ctx *c, ImportFn f, uint64_t a0) { c->x[0] = a0; c->x[30] = 0; f(c); return c->x[0]; }

static DWORD WINAPI frame_thread(LPVOID arg) {
    (void)arg;
    static Ctx c;                                /* one frame thread: its own guest context, stack and thread block */
    uint8_t *stack = VirtualAlloc(NULL, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *tls = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    static uint8_t thread_type[0x400];
    c.x[31] = (uint64_t)(uintptr_t)(stack + 0x40000);
    c.tpidr = (uint64_t)(uintptr_t)tls;
    wr64(c.tpidr + 0x1f8, (uint64_t)(uintptr_t)thread_type);   /* Horizon thread block: ThreadType pointer at +0x1f8 */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ImportFn send = lifted(RS_SEND), rate = lifted(RS_RATE), count = lifted(RS_COUNT);
    LARGE_INTEGER fq, t0, now;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    /* The wait for the next frame boundary sleeps on a high-resolution timer. It used to Sleep(1) until 2 ms before the
     * boundary and then call SwitchToThread in a loop: 2 of every 5 ms busy on a time-critical thread (69% of a core in
     * all, measured), taken from the game's threads when the PC is busy. The output has 120 ms queued, so a frame a
     * fraction of a millisecond late changes nothing; the schedule stays on the performance counter. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    double period = 0;
    long long tick = 0;                          /* frames since t0: the schedule (g_frames only counts) */
    for (;;) {
        uint64_t sys[MAX_SYS];
        int n;
        AcquireSRWLockShared(&g_sys_lock);
        n = g_nsys;
        memcpy(sys, g_sys, sizeof sys);
        ReleaseSRWLockShared(&g_sys_lock);
        if (n && period == 0) {
            uint32_t r = (uint32_t)call1(&c, rate, sys[0]), k = (uint32_t)call1(&c, count, sys[0]);
            period = r ? (double)k / r : 0.005;
            printf("smo_audio: rendering clock: %u samples at %u Hz = %.3f ms per frame\n", k, r, period * 1000.0);
        }
        for (int i = 0; i < n; i++) call1(&c, send, sys[i]);
        if (n) InterlockedIncrement64(&g_frames);
        /* the next frame boundary, on the performance counter (no drift); after a stall of more than 10 frames the
         * schedule restarts from now instead of rendering the missed frames in a burst */
        double p = period ? period : 0.005;
        long long frame = ++tick;
        for (;;) {
            QueryPerformanceCounter(&now);
            double t = (double)(now.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
            double due = frame * p;
            if (!n || t >= due) { if (t > due + 10 * p) { t0 = now; tick = 0; } break; }
            double left = due - t;
            LARGE_INTEGER rel;
            rel.QuadPart = -(LONGLONG)(left * 1e7) - 1;              /* relative, 100 ns units */
            if (timer && SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100);
            else if (left > 0.002) Sleep(1); else SwitchToThread();
        }
        if (!n) Sleep(1);
    }
    return 0;
}

static void rs_Start(Ctx *c) {
    static ImportFn start;
    if (!start) start = lifted(RS_START);
    uint64_t sys = c->x[0];
    start(c);
    AcquireSRWLockExclusive(&g_sys_lock);
    int known = 0;
    for (int i = 0; i < g_nsys; i++) known |= g_sys[i] == sys;
    if (!known && g_nsys < MAX_SYS) g_sys[g_nsys++] = sys;
    ReleaseSRWLockExclusive(&g_sys_lock);
    printf("smo_audio: renderer %llx started (%d running)\n", (unsigned long long)sys, g_nsys);
    if (!g_frame_thread) g_frame_thread = CreateThread(NULL, 1 << 20, frame_thread, NULL, 0, NULL);
}
static void rs_Stop(Ctx *c) {
    static ImportFn stop;
    if (!stop) stop = lifted(RS_STOP);
    uint64_t sys = c->x[0];
    AcquireSRWLockExclusive(&g_sys_lock);
    for (int i = 0; i < g_nsys; i++) if (g_sys[i] == sys) { g_sys[i] = g_sys[--g_nsys]; break; }
    ReleaseSRWLockExclusive(&g_sys_lock);
    stop(c);
}
/* ---- THE SOUND: the device sink's channels go to the PC through waveOut (48 kHz, 16-bit, stereo).
 * DeviceSinkCommand [sdk 0x18aab8 GenerateDeviceSinkCommand]: magic 0xcafebabe, u16 size 0x130, u16 type 0xf, +0x10 enabled,
 * +0x14 device name (256), +0x114 session, +0x118 sample buffer (the upsampler's output when the renderer does not run at
 * 48 kHz; 0 otherwise), +0x120 input count, +0x124 mix buffer index per channel (u8, already offset). Mix buffers are int32
 * samples, buffer i at GetMixBuffers() + i * sample count; the device takes them as 16-bit (clamped). Frames of 5 ms are
 * gathered into blocks of 40 ms; 16 blocks in flight at most (the oldest pending audio is dropped and counted if the
 * device falls behind; a block the device finished before the next one was ready is counted as an underrun). */
#include <mmsystem.h>
static float g_dev_volume[3];                    /* defined with its values below (the device layer) */
#define SINK_BLOCK 1920                           /* frames per waveOut block (40 ms at 48 kHz) */
#define SINK_NBLK 16
static HWAVEOUT g_wo;
static WAVEHDR g_wh[SINK_NBLK];
static int16_t g_wbuf[SINK_NBLK][SINK_BLOCK * 2];
static int g_wcur, g_wfill, g_wopen = -1, g_wstarted;
static LONG64 g_sink_frames, g_sink_blocks, g_sink_dropped, g_sink_underruns;
static uint32_t g_sink_peak;
static FILE *g_wav;                               /* SMO_AUDIO_WAV=<file>: what goes to the device, as a WAV file (header completed at exit) */
static uint32_t g_wav_frames;
static void wav_header(void) {
    uint32_t data = g_wav_frames * 4, riff = 36 + data, fmt = 16, rate = 48000, bps = 48000 * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav); fwrite(&fmt, 4, 1, g_wav);
    fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav); fwrite(&rate, 4, 1, g_wav); fwrite(&bps, 4, 1, g_wav); fwrite(&align, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&data, 4, 1, g_wav);
    fseek(g_wav, 0, SEEK_END); fflush(g_wav);
}
static void sink_open(void) {
    if (getenv("SMO_AUDIO_WAV")) { g_wav = fopen(getenv("SMO_AUDIO_WAV"), "wb"); if (g_wav) wav_header(); }
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    if (getenv("SMO_AUDIO") && !atoi(getenv("SMO_AUDIO"))) { g_wopen = 0; printf("smo_audio: sound output off (SMO_AUDIO=0)\n"); return; }
    MMRESULT r = waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL);
    g_wopen = r == MMSYSERR_NOERROR;
    printf("smo_audio: sound output: %s\n", g_wopen ? "waveOut, 48000 Hz, 16-bit stereo, 40 ms blocks" : "waveOutOpen FAILED: no sound");
    if (g_wopen) waveOutPause(g_wo);
}
static void sink_submit(void) {                   /* the block g_wcur is full */
    WAVEHDR *h = &g_wh[g_wcur];
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, sizeof *h);
    memset(h, 0, sizeof *h);
    h->lpData = (LPSTR)g_wbuf[g_wcur]; h->dwBufferLength = SINK_BLOCK * 4;
    waveOutPrepareHeader(g_wo, h, sizeof *h);
    waveOutWrite(g_wo, h, sizeof *h);
    g_sink_blocks++;
    if (!g_wstarted && g_sink_blocks >= 3) { waveOutRestart(g_wo); g_wstarted = 1; }      /* 120 ms queued before playing */
    else if (g_wstarted) {                       /* everything before this one already played: the device waited for us */
        int busy = 0;
        for (int i = 0; i < SINK_NBLK; i++) if (i != g_wcur && (g_wh[i].dwFlags & WHDR_PREPARED) && !(g_wh[i].dwFlags & WHDR_DONE)) busy = 1;
        if (!busy) g_sink_underruns++;
    }
    g_wcur = (g_wcur + 1) % SINK_NBLK; g_wfill = 0;
}
/* The command header and the device sink command, as each SDK writes them:
 *   3.5.1 [sdk 0x18aab8 GenerateDeviceSinkCommand]: {u32 magic 0xcafebabe, u16 size, u16 type (0xf: device sink), u32
 *         estimated time, u32 node}, +0x10 enabled, +0x14 device name, +0x118 sample buffer, +0x120 input count, +0x124
 *         the mix buffer of each channel (u8, already offset).
 *   7.3.2 [sdk 7.3.2 0x9f950 CommandBuffer::GenerateDeviceSinkCommand]: {u32 magic, u8 enabled, u8 type (0x10: device
 *         sink), u16 size, u32 estimated time, u32 node}, +0x20 device name (256), +0x120 session, +0x128 sample buffer,
 *         +0x130 input count, +0x134 the mix buffer of each channel (u16, already offset).
 * SDK_VERSION (image.h, the program NCA's SDK) says which; the SDKs between those two are not known here. */
typedef struct { const char *name; uint32_t inputs; uint64_t sample_buffer; unsigned index[6]; } Sink;
#if SDK_VERSION >= 0x07000000u
#define SINK_TYPE 0x10u
static void cmd_header(const uint8_t *c, unsigned *size, unsigned *type, int *enabled) { *enabled = c[4]; *type = c[5]; *size = c[6] | (unsigned)c[7] << 8; }
static void sink_fields(const uint8_t *c, Sink *s) {
    s->name = (const char *)(c + 0x20);
    memcpy(&s->sample_buffer, c + 0x128, 8); memcpy(&s->inputs, c + 0x130, 4);
    for (int i = 0; i < 6; i++) s->index[i] = c[0x134 + 2 * i] | (unsigned)c[0x135 + 2 * i] << 8;
}
#else
#define SINK_TYPE 0xfu
static void cmd_header(const uint8_t *c, unsigned *size, unsigned *type, int *enabled) { *size = c[4] | (unsigned)c[5] << 8; *type = c[6] | (unsigned)c[7] << 8; *enabled = c[0x10]; }
static void sink_fields(const uint8_t *c, Sink *s) {
    s->name = (const char *)(c + 0x14);
    memcpy(&s->sample_buffer, c + 0x118, 8); memcpy(&s->inputs, c + 0x120, 4);
    for (int i = 0; i < 6; i++) s->index[i] = c[0x124 + i];
}
#endif
static void audio_sink_out(const Sink *s, uint64_t mb, uint32_t ns) {
    uint32_t nin = s->inputs;
    if (g_wopen < 0) {
        sink_open();
        printf("smo_audio: device sink \"%.32s\": %u channels, mix buffers %u %u %u %u %u %u, %u samples per frame, sample buffer %llx\n",
               s->name, nin, s->index[0], s->index[1], s->index[2], s->index[3], s->index[4], s->index[5], ns, (unsigned long long)s->sample_buffer);
    }
    if (!g_wopen || !mb || !ns || nin == 0 || nin > 6) return;
    if (ns != 240) { static int said; if (!said++) printf("smo_audio: the renderer does not run at 48 kHz (%u samples per 5 ms frame): its upsampler output is NOT IMPLEMENTED, no sound\n", ns); return; }
    /* the sample buffer is always filled ([sdk 0x18ad4c]: the translated upsampler buffer, or the caller's argument): at 48 kHz the sink reads the mix buffers */
    const int32_t *L = (const int32_t *)(uintptr_t)(mb + (uint64_t)s->index[0] * ns * 4), *R = (const int32_t *)(uintptr_t)(mb + (uint64_t)s->index[nin > 1] * ns * 4);
    const int32_t *C = nin >= 6 ? (const int32_t *)(uintptr_t)(mb + (uint64_t)s->index[2] * ns * 4) : NULL;      /* 6 channels: FL FR FC LFE BL BR; FC and the backs folded in */
    const int32_t *BL = nin >= 6 ? (const int32_t *)(uintptr_t)(mb + (uint64_t)s->index[4] * ns * 4) : NULL, *BR = nin >= 6 ? (const int32_t *)(uintptr_t)(mb + (uint64_t)s->index[5] * ns * 4) : NULL;
    float vol = g_dev_volume[0];
    for (uint32_t i = 0; i < ns; i++) {
        float l = (float)L[i], r = (float)R[i];
        if (C) { l += 0.707f * (float)C[i] + 0.707f * (float)BL[i]; r += 0.707f * (float)C[i] + 0.707f * (float)BR[i]; }
        l *= vol; r *= vol;
        int32_t li = (int32_t)(l < -32768.f ? -32768.f : l > 32767.f ? 32767.f : l), ri = (int32_t)(r < -32768.f ? -32768.f : r > 32767.f ? 32767.f : r);
        uint32_t a = (uint32_t)(li < 0 ? -li : li); if (a > g_sink_peak) g_sink_peak = a;
        if (g_wfill == 0 && (g_wh[g_wcur].dwFlags & WHDR_PREPARED) && !(g_wh[g_wcur].dwFlags & WHDR_DONE)) { g_sink_dropped += ns - i; return; }    /* all 16 blocks still queued */
        g_wbuf[g_wcur][g_wfill * 2] = (int16_t)li; g_wbuf[g_wcur][g_wfill * 2 + 1] = (int16_t)ri;
        if (g_wav) { int16_t fr[2] = {(int16_t)li, (int16_t)ri}; fwrite(fr, 4, 1, g_wav); g_wav_frames++; }
        if (++g_wfill == SINK_BLOCK) sink_submit();
    }
    g_sink_frames++;
}

/* dsp::SendCommandBuffer(int session, DspAddr list, size_t size, int ?, bool ?, u64 ?) */
static void dsp_SendCommandBuffer(Ctx *c) {
    static ImportFn tmax, setup, process;
    if (!process) { tmax = lifted(CLP_TIME); setup = lifted(CLP_SETUP); process = lifted(CLP_PROCESS); }
    unsigned s = (unsigned)c->x[0] & 15;
    uint64_t list = c->x[1], size = c->x[2], lr = c->x[30];
    if (!g_proc[s]) g_proc[s] = calloc(1, 0x400);
    uint64_t p = (uint64_t)(uintptr_t)g_proc[s];
    c->x[0] = p; c->x[1] = 0xffffffffu; tmax(c);
    c->x[0] = p; c->x[1] = list; c->x[2] = size; setup(c);
    c->x[0] = p; process(c);
    {   /* the device sink: skipped by this SDK's processor (jump table [sdk 0x77c38c] entry 0xf -> next command); on a
         * console the audio DSP firmware outputs it. Here: its channels, read from the mix buffers after the list ran. */
        static ImportFn mixbufs, scount;
        if (!scount) { mixbufs = lifted("_ZNK2nn5audio3dsp20CommandListProcessor13GetMixBuffersEv"); scount = lifted("_ZNK2nn5audio3dsp20CommandListProcessor14GetSampleCountEv"); }
        c->x[0] = p; mixbufs(c); uint64_t mb = c->x[0];
        c->x[0] = p; scount(c); uint32_t ns = (uint32_t)c->x[0];
        {   /* SMO_AUDIO_SCAN=<n>: every n lists, the mix buffers that hold a non-zero sample and their peaks */
            static int every = -1;
            static ImportFn bcount;
            if (every < 0) { const char *e = getenv("SMO_AUDIO_SCAN"); every = e ? atoi(e) : 0; }
            if (every > 0 && g_lists % every == 0 && mb && ns) {
                if (!bcount) bcount = lifted("_ZNK2nn5audio3dsp20CommandListProcessor14GetBufferCountEv");
                c->x[0] = p; bcount(c);
                unsigned nb = (unsigned)c->x[0], shown = 0;
                printf("smo_audio: list %lld: %u mix buffers of %u samples at %llx; with signal:", (long long)g_lists, nb, ns, (unsigned long long)mb);
                for (unsigned b = 0; b < nb && b < 256; b++) {
                    const int32_t *s = (const int32_t *)(uintptr_t)(mb + (uint64_t)b * ns * 4);
                    int32_t peak = 0;
                    for (uint32_t i = 0; i < ns; i++) { int32_t v = s[i] < 0 ? -s[i] : s[i]; if (v > peak) peak = v; }
                    if (peak && shown++ < 24) printf(" %u:%d", b, peak);
                }
                printf("%s\n", shown ? "" : " none");
            }
        }
        static int scan_every = -1;                         /* SMO_AUDIO_SCAN: also the commands of the list, by type */
        if (scan_every < 0) { const char *e = getenv("SMO_AUDIO_SCAN"); scan_every = e ? atoi(e) : 0; }
        int scan = scan_every > 0 && g_lists % scan_every == 0;
        unsigned hist[64][2];
        if (scan) memset(hist, 0, sizeof hist);
        for (uint64_t o = 0; o + 0x10 <= size;) {           /* the list: a header, then commands {magic 0xcafebabe, ...} (cmd_header) */
            const uint8_t *cmd = (const uint8_t *)(uintptr_t)(list + o);
            uint32_t magic;
            unsigned csz, type;
            int enabled;
            memcpy(&magic, cmd, 4);
            if (magic != 0xcafebabeu) { o += 4; continue; }
            cmd_header(cmd, &csz, &type, &enabled);
            if (scan && type < 64) {
                hist[type][enabled != 0]++;
                static long long shown_list = -1;
                const char *dump = getenv("SMO_AUDIO_SCAN_TYPE");            /* per scanned list, the first command of this type: its first 0x100 bytes */
                if (dump && (unsigned)strtoul(dump, NULL, 16) == type && enabled && shown_list != g_lists) {
                    shown_list = g_lists;
                    printf("smo_audio: command type %x, %u bytes:", type, csz);
                    for (unsigned i = 0; i < 0x100 && i < csz; i += 8) { uint64_t q; memcpy(&q, cmd + i, 8); printf("%s%016llx", i % 32 ? " " : "\n  ", (unsigned long long)q); }
                    const char *at = getenv("SMO_AUDIO_SCAN_AT");                /* hex offset in the command of a pointer: 32 bytes it points to */
                    if (at) {
                        uint64_t ptr;
                        memcpy(&ptr, cmd + strtoul(at, NULL, 16), 8);
                        printf("\n  at +%s -> %llx:", at, (unsigned long long)ptr);
                        __try { for (int i = 0; i < 32; i++) printf(" %02x", ((const uint8_t *)(uintptr_t)ptr)[i]); } __except (1) { printf(" (unreadable)"); }
                    }
                    printf("\n");
                }
            }
            if (type == SINK_TYPE && enabled) { Sink s; sink_fields(cmd, &s); audio_sink_out(&s, mb, ns); }
            if (csz < 0x10) break;
            o += csz;
        }
        if (scan) {
            printf("smo_audio: list %lld: %llu bytes; commands type:enabled/disabled", (long long)g_lists, (unsigned long long)size);
            for (int t = 0; t < 64; t++) if (hist[t][0] || hist[t][1]) printf(" %x:%u/%u", t, hist[t][1], hist[t][0]);
            printf("\n  in order:");
            for (uint64_t o = 0; o + 0x10 <= size;) {
                const uint8_t *cmd = (const uint8_t *)(uintptr_t)(list + o);
                uint32_t magic; unsigned csz, type; int enabled;
                memcpy(&magic, cmd, 4);
                if (magic != 0xcafebabeu) { o += 4; continue; }
                cmd_header(cmd, &csz, &type, &enabled);
                printf(" %x%s", type, enabled ? "" : "-");
                if (csz < 0x10) break;
                o += csz;
            }
            printf("\n");
        }
    }
    c->x[30] = lr;
    InterlockedIncrement64(&g_lists);
    InterlockedAdd64(&g_commands_bytes, (LONG64)size);
}

/* ---- the audio DEVICE layer: nn::audio::server::detail::* are stubs in the application SDK [sdk 0x193d94 ..
 * 0x193ddc: return 0x4990400 or nothing]; on a console the audio system module answers them. Host model, as for a
 * docked console with a stereo output: [doc] device names as the system reports them (yuzu audio_core
 * audio_device.cpp, switchbrew "Audio services"): AudioTvOutput (active when docked), AudioStereoJackOutput,
 * AudioBuiltInSpeakerOutput; output volume per device, default 1.0; 2 active channels (the PC's stereo output).
 * The device-switch and input/output notification events are real system events (os_threads.c handles) that are
 * never signalled: the output device does not change. AudioDeviceName = 256 bytes. */
static const char *const g_dev_names[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
static float g_dev_volume[3] = {1.0f, 1.0f, 1.0f};
static uint32_t g_dev_event[3];
uint32_t os_system_event_new(int autoclear, uint64_t *key);    /* os_threads.c */
static int dev_index(uint64_t name) {
    for (int i = 0; i < 3; i++) if (strncmp((const char *)(uintptr_t)name, g_dev_names[i], 256) == 0) return i;
    return -1;
}
static void dev_ok(Ctx *c) { c->x[0] = 0; }
static void dev_list(Ctx *c) {                  /* (AudioDeviceName *out, int count) -> int written */
    int n = (int)c->x[1] < 3 ? (int)c->x[1] : 3;
    for (int i = 0; i < n; i++) { memset((void *)(uintptr_t)(c->x[0] + 256ull * i), 0, 256); strcpy((char *)(uintptr_t)(c->x[0] + 256ull * i), g_dev_names[i]); }
    c->x[0] = n < 0 ? 0 : (uint64_t)n;
}
static void dev_set_volume(Ctx *c) {            /* (const AudioDeviceName*, float volume (s0), aruid) */
    int i = dev_index(c->x[0]);
    if (i >= 0) g_dev_volume[i] = c->v[0].f[0];
    c->x[0] = 0;
}
static void dev_get_volume(Ctx *c) {            /* (const AudioDeviceName*, aruid) -> float (s0) */
    int i = dev_index(c->x[0]);
    float v = i >= 0 ? g_dev_volume[i] : 0.0f;
    vzero(&c->v[0]); c->v[0].f[0] = v;
}
static void dev_get_active(Ctx *c) { memset((void *)(uintptr_t)c->x[0], 0, 256); strcpy((char *)(uintptr_t)c->x[0], g_dev_names[0]); }
static void dev_event(Ctx *c, int which) {     /* (u32 *handle, aruid) -> Result */
    if (!g_dev_event[which]) g_dev_event[which] = os_system_event_new(0, NULL);
    wr32(c->x[0], g_dev_event[which]);
    c->x[0] = 0;
}
static void dev_event_switch(Ctx *c) { dev_event(c, 0); }
static void dev_event_input(Ctx *c) { dev_event(c, 1); }
static void dev_event_output(Ctx *c) { dev_event(c, 2); }
static void dev_channels(Ctx *c) { c->x[0] = 2; }

void smo_audio_report(FILE *out) {
    if (g_wav) { wav_header(); fclose(g_wav); g_wav = NULL; fprintf(out, "audio: %u frames (%.1f s) written to %s\n", g_wav_frames, g_wav_frames / 48000.0, getenv("SMO_AUDIO_WAV")); }
    if (g_mgr_by_hipc) fprintf(out, "audio: renderer manager requests answered in-process: %llu\n", g_mgr_by_hipc);
    fprintf(out, "audio: renderers running %d, frames clocked %lld, command lists processed %lld (%lld bytes)\n",
            g_nsys, (long long)g_frames, (long long)g_lists, (long long)g_commands_bytes);
    fprintf(out, "audio: device sink: %lld frames output, %lld blocks of 40 ms queued, %lld samples dropped (device behind), %lld underruns (device waited), peak level %u of 32767\n",
            (long long)g_sink_frames, (long long)g_sink_blocks, (long long)g_sink_dropped, (long long)g_sink_underruns, g_sink_peak);
}

void register_smo_audio(void) {
    sdk_host_override("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", audio_manager_by_hipc);
    sdk_host_override(RS_START, rs_Start);
    sdk_host_override(RS_STOP, rs_Stop);
    sdk_host_override("_ZN2nn5audio3dsp17SendCommandBufferEimmibm", dsp_SendCommandBuffer);
#define SD(n) "_ZN2nn5audio6server6detail" n
    sdk_host_override(SD("21InitializeAudioDeviceERKNS_6applet20AppletResourceUserIdE"), dev_ok);
    sdk_host_override(SD("19FinalizeAudioDeviceERKNS_6applet20AppletResourceUserIdE"), dev_ok);
    sdk_host_override(SD("19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi"), dev_list);
    sdk_host_override(SD("26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEfRKNS_6applet20AppletResourceUserIdE"), dev_set_volume);
    sdk_host_override(SD("26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameERKNS_6applet20AppletResourceUserIdE"), dev_get_volume);
    sdk_host_override(SD("24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE"), dev_get_active);
    sdk_host_override(SD("27QueryAudioDeviceSystemEventEPjRKNS_6applet20AppletResourceUserIdE"), dev_event_switch);
    sdk_host_override(SD("38QueryAudioDeviceInputNotificationEventEPjRKNS_6applet20AppletResourceUserIdE"), dev_event_input);
    sdk_host_override(SD("39QueryAudioDeviceOutputNotificationEventEPjRKNS_6applet20AppletResourceUserIdE"), dev_event_output);
    sdk_host_override(SD("21GetActiveChannelCountEv"), dev_channels);
#undef SD
}
