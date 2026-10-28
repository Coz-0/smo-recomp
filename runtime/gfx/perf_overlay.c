/* perf_overlay.c - the performance overlay: F10 cycles off, frame rate, full (graphics.ini perf_overlay sets the start).
 *
 * Numbers in a corner of the window, drawn over the window image after the game's picture (nvn_vk_draw.c overlay_draw),
 * so pictures of the Capture button and SMO_SHOT* never have them. Frame rate: frames per second and milliseconds over
 * the last second. Full, as Savage Wind's overlay ([doc] Revivedx/SavageWindRecomp, MIT, runtime/perfmon.c and its F10
 * overlay) plus what only the renderer knows:
 *   - the game's frames: rate, average and longest of the last second, a graph of the last 120, frames over 33.4 ms
 *     (two refreshes missed) in the last minute;
 *   - the renderer: GPU milliseconds of a frame (timestamp queries, written while the full overlay is on, the frame
 *     read back NSLOT frames later), the size the game draws its 3D scene at (its dynamic resolution), CPU milliseconds
 *     per frame of the game's main thread and of the render thread;
 *   - the machine, twice a second on a thread of its own and only while the full overlay is shown: CPU use of all
 *     processors and of this process, memory of the process and of the machine; on Linux also the CPU temperature
 *     (hwmon: k10temp, zenpower, coretemp) and the GPU's use, temperature, power, clock and memory (amdgpu sysfs).
 *     A number the platform does not give is "-".
 * The letters: font8x8_basic, 8x8 pixels, public domain ([doc] dhepper/font8x8, from the IBM VGA font), printable ASCII,
 * drawn 2x on a window 1000 pixels high or more. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>                         /* PROCESS_MEMORY_COUNTERS; K32GetProcessMemoryInfo is in kernel32 */
#else
#include "win32_posix.h"
#include <dirent.h>
#endif
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif
#include "perf_overlay.h"

static const uint64_t g_glyph[95] = {      /* U+0020..U+007E: row r = byte r, bit x = column x */
    0x0000000000000000ull, 0x00180018183c3c18ull, 0x0000000000003636ull, 0x0036367f367f3636ull, 0x000c1f301e033e0cull, 0x0063660c18336300ull,
    0x006e333b6e1c361cull, 0x0000000000030606ull, 0x00180c0606060c18ull, 0x00060c1818180c06ull, 0x0000663cff3c6600ull, 0x00000c0c3f0c0c00ull,
    0x060c0c0000000000ull, 0x000000003f000000ull, 0x000c0c0000000000ull, 0x000103060c183060ull, 0x003e676f7b73633eull, 0x003f0c0c0c0c0e0cull,
    0x003f33061c30331eull, 0x001e33301c30331eull, 0x0078307f33363c38ull, 0x001e3330301f033full, 0x001e33331f03061cull, 0x000c0c0c1830333full,
    0x001e33331e33331eull, 0x000e18303e33331eull, 0x000c0c00000c0c00ull, 0x060c0c00000c0c00ull, 0x00180c0603060c18ull, 0x00003f00003f0000ull,
    0x00060c1830180c06ull, 0x000c000c1830331eull, 0x001e037b7b7b633eull, 0x0033333f33331e0cull, 0x003f66663e66663full, 0x003c66030303663cull,
    0x001f36666666361full, 0x007f46161e16467full, 0x000f06161e16467full, 0x007c66730303663cull, 0x003333333f333333ull, 0x001e0c0c0c0c0c1eull,
    0x001e333330303078ull, 0x006766361e366667ull, 0x007f66460606060full, 0x0063636b7f7f7763ull, 0x006363737b6f6763ull, 0x001c36636363361cull,
    0x000f06063e66663full, 0x00381e3b3333331eull, 0x006766363e66663full, 0x001e33380e07331eull, 0x001e0c0c0c0c2d3full, 0x003f333333333333ull,
    0x000c1e3333333333ull, 0x0063777f6b636363ull, 0x0063361c1c366363ull, 0x001e0c0c1e333333ull, 0x007f664c1831637full, 0x001e06060606061eull,
    0x00406030180c0603ull, 0x001e18181818181eull, 0x0000000063361c08ull, 0xff00000000000000ull, 0x0000000000180c0cull, 0x006e333e301e0000ull,
    0x003b66663e060607ull, 0x001e3303331e0000ull, 0x006e33333e303038ull, 0x001e033f331e0000ull, 0x000f06060f06361cull, 0x1f303e33336e0000ull,
    0x006766666e360607ull, 0x001e0c0c0c0e000cull, 0x1e33333030300030ull, 0x0067361e36660607ull, 0x001e0c0c0c0c0c0eull, 0x00636b7f7f330000ull,
    0x00333333331f0000ull, 0x001e3333331e0000ull, 0x0f063e66663b0000ull, 0x78303e33336e0000ull, 0x000f06666e3b0000ull, 0x001f301e033e0000ull,
    0x00182c0c0c3e0c08ull, 0x006e333333330000ull, 0x000c1e3333330000ull, 0x00367f7f6b630000ull, 0x0063361c36630000ull, 0x1f303e3333330000ull,
    0x003f260c193f0000ull, 0x00380c0c070c0c38ull, 0x0018181800181818ull, 0x00070c0c380c0c07ull, 0x0000000000003b6eull,
};

static volatile LONG g_mode = -1;          /* PERF_OFF / PERF_FPS / PERF_FULL */
int perf_overlay_mode(void) {
    if (g_mode < 0) {
        const char *e = getenv("SMO_PERF_OVERLAY");
        int m = e ? atoi(e) : perf_overlay_default();
        InterlockedExchange(&g_mode, m >= 0 && m <= 2 ? m : 0);
    }
    return g_mode;
}
void perf_overlay_toggle(void) {
    int m = (perf_overlay_mode() + 1) % 3;
    InterlockedExchange(&g_mode, m);
    printf("perf overlay: %s (F10)\n", m == PERF_FULL ? "full" : m == PERF_FPS ? "frame rate" : "off");
}
int perf_overlay_wants_gpu(void) { return perf_overlay_mode() == PERF_FULL; }

/* ---- frames (render thread) */
#define HIST 120
#define SLOWS 4096
static float g_ms[HIST];
static double g_at[HIST];                  /* seconds, when each frame was presented */
static int g_n, g_pos;
static double g_slow_at[SLOWS];            /* when frames over 33.4 ms were presented */
static int g_nslow;
static float g_gpu_ms = -1, g_main_ms = -1, g_render_ms = -1;
static int g_vpw, g_vph;
static DWORD g_render_tid;
static DWORD g_main_tid;                   /* the thread nnMain runs on (perf_overlay_main_here) */
void perf_overlay_main_here(void) { g_main_tid = GetCurrentThreadId(); }      /* boot.c, just before nnMain */

static double now_s(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart / (double)f.QuadPart; }
static double thread_cpu_ms(DWORD tid) {   /* CPU time a thread used, ms; -1 when it cannot be read */
    if (!tid) return -1;
#ifdef _WIN32
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME c, e, k, u;
    double ms = -1;
    if (h && GetThreadTimes(h, &c, &e, &k, &u))
        ms = ((double)(((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (double)(((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
    if (h) CloseHandle(h);
    return ms;
#elif defined(__linux__)
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/self/task/%u/stat", (unsigned)tid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *s = strrchr(b, ')');               /* fields after the name: state is field 3; utime, stime are 14, 15 */
    unsigned long long ut = 0, st = 0;
    if (!s || sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return -1;
    return (double)(ut + st) * 1000.0 / (double)sysconf(_SC_CLK_TCK);
#else
    return -1;
#endif
}

void perf_overlay_frame(double wall_ms, float gpu_ms, int vpw, int vph) {
    if (perf_overlay_mode() == PERF_OFF) { g_n = 0; return; }
    double t = now_s();
    if (!g_render_tid) g_render_tid = GetCurrentThreadId();
    if (wall_ms > 0) {
        g_ms[g_pos] = (float)wall_ms; g_at[g_pos] = t; g_pos = (g_pos + 1) % HIST; if (g_n < HIST) g_n++;
        if (wall_ms > 33.4) g_slow_at[g_nslow++ % SLOWS] = t;
    }
    if (gpu_ms > 0) g_gpu_ms = gpu_ms;
    if (vpw > 0) { g_vpw = vpw; g_vph = vph; }
    static double last; static double main0, render0; static int frames;
    frames++;
    if (t - last >= 0.5) {                   /* threads' CPU per frame, twice a second */
        double m = thread_cpu_ms(g_main_tid), r = thread_cpu_ms(g_render_tid);
        if (last > 0 && frames > 0) {
            g_main_ms = m >= 0 && main0 >= 0 ? (float)((m - main0) / frames) : -1;
            g_render_ms = r >= 0 && render0 >= 0 ? (float)((r - render0) / frames) : -1;
        }
        main0 = m; render0 = r; last = t; frames = 0;
    }
}

/* ---- the machine (its own thread, twice a second while the full overlay is shown) */
typedef struct { float cpu, cpu_game, cpu_temp, ram_game, ram_used, ram_total, gpu, gpu_temp, gpu_w, gpu_mhz, vram_used, vram_total, gtt_used, gtt_total; } Machine;
static Machine g_mach = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static CRITICAL_SECTION g_mlock;
static char g_cpu_name[64], g_gpu_name[64];
void perf_overlay_set_gpu(const char *name) { snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : ""); }

static ULONG64 ft64(FILETIME f) { return ((ULONG64)f.dwHighDateTime << 32) | f.dwLowDateTime; }
#ifdef __linux__
static double read_num(const char *path) {  /* the first number in a sysfs file, -1 if none */
    FILE *f = fopen(path, "r");
    double v = -1;
    if (f) { if (fscanf(f, "%lf", &v) != 1) v = -1; fclose(f); }
    return v;
}
static char g_hw_cpu[300], g_hw_gpu[300], g_drm[300];       /* the CPU's hwmon folder, the GPU's hwmon folder, its drm device */
static void find_sensors(void) {
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[300], n[64] = "";
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(n, sizeof n, f)) n[0] = 0;
        fclose(f);
        n[strcspn(n, "\n")] = 0;
        if (!g_hw_cpu[0] && (!strcmp(n, "k10temp") || !strcmp(n, "zenpower") || !strcmp(n, "coretemp"))) snprintf(g_hw_cpu, sizeof g_hw_cpu, "/sys/class/hwmon/%s", e->d_name);
        if (!g_hw_gpu[0] && !strcmp(n, "amdgpu")) snprintf(g_hw_gpu, sizeof g_hw_gpu, "/sys/class/hwmon/%s", e->d_name);
    }
    if (d) closedir(d);
    for (int i = 0; i < 8 && !g_drm[0]; i++) {
        char p[300];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", i);
        if (read_num(p) >= 0) snprintf(g_drm, sizeof g_drm, "/sys/class/drm/card%d/device", i);
    }
}
#endif
static void sample_machine(Machine *m) {
    static ULONG64 idle0, kern0, user0, pk0, pu0;
    FILETIME i, k, u, c, e, pk, pu;
    m->cpu = m->cpu_game = -1;
    if (GetSystemTimes(&i, &k, &u) && GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
        ULONG64 total = (ft64(k) - kern0) + (ft64(u) - user0), idle = ft64(i) - idle0, game = (ft64(pk) - pk0) + (ft64(pu) - pu0);
        if (kern0 && total) { m->cpu = (float)(100.0 * (double)(total - idle) / (double)total); m->cpu_game = (float)(100.0 * (double)game / (double)total); }
        idle0 = ft64(i); kern0 = ft64(k); user0 = ft64(u); pk0 = ft64(pk); pu0 = ft64(pu);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    m->ram_game = K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? (float)((double)pmc.WorkingSetSize / 1048576.0) : -1;
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) { m->ram_total = (float)((double)ms.ullTotalPhys / 1048576.0); m->ram_used = (float)((double)(ms.ullTotalPhys - ms.ullAvailPhys) / 1048576.0); }
#elif defined(__linux__)
    {   FILE *f = fopen("/proc/meminfo", "r");
        char l[128];
        double tot = -1, avail = -1;
        while (f && fgets(l, sizeof l, f)) { if (!strncmp(l, "MemTotal:", 9)) tot = atof(l + 9); else if (!strncmp(l, "MemAvailable:", 13)) avail = atof(l + 13); }
        if (f) fclose(f);
        if (tot > 0 && avail >= 0) { m->ram_total = (float)(tot / 1024.0); m->ram_used = (float)((tot - avail) / 1024.0); }
    }
    static int found;
    if (!found) { found = 1; find_sensors(); }
    char p[400];
    if (g_hw_cpu[0]) { snprintf(p, sizeof p, "%s/temp1_input", g_hw_cpu); double v = read_num(p); m->cpu_temp = v >= 0 ? (float)(v / 1000.0) : -1; }
    if (g_drm[0]) {
        snprintf(p, sizeof p, "%s/gpu_busy_percent", g_drm); m->gpu = (float)read_num(p);
        double v;
        snprintf(p, sizeof p, "%s/mem_info_vram_used", g_drm); v = read_num(p); m->vram_used = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_vram_total", g_drm); v = read_num(p); m->vram_total = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_gtt_used", g_drm); v = read_num(p); m->gtt_used = v >= 0 ? (float)(v / 1048576.0) : -1;
        snprintf(p, sizeof p, "%s/mem_info_gtt_total", g_drm); v = read_num(p); m->gtt_total = v >= 0 ? (float)(v / 1048576.0) : -1;
    }
    if (g_hw_gpu[0]) {
        double v;
        snprintf(p, sizeof p, "%s/temp1_input", g_hw_gpu); v = read_num(p); m->gpu_temp = v >= 0 ? (float)(v / 1000.0) : -1;
        snprintf(p, sizeof p, "%s/power1_average", g_hw_gpu); v = read_num(p);
        if (v < 0) { snprintf(p, sizeof p, "%s/power1_input", g_hw_gpu); v = read_num(p); }
        m->gpu_w = v >= 0 ? (float)(v / 1e6) : -1;
        snprintf(p, sizeof p, "%s/freq1_input", g_hw_gpu); v = read_num(p); m->gpu_mhz = v >= 0 ? (float)(v / 1e6) : -1;
    }
#endif
}
static DWORD WINAPI machine_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        if (perf_overlay_mode() == PERF_FULL) {
            Machine m = g_mach;
            sample_machine(&m);
            EnterCriticalSection(&g_mlock); g_mach = m; LeaveCriticalSection(&g_mlock);
        }
        Sleep(500);
    }
    return 0;
}
static void machine_start(void) {
    static int started;
    if (started) return;
    started = 1;
    InitializeCriticalSection(&g_mlock);
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
    {   unsigned r[12];
        if (__get_cpuid(0x80000000, &r[0], &r[1], &r[2], &r[3]) && r[0] >= 0x80000004) {
            for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
            char b[49];
            memcpy(b, r, 48); b[48] = 0;
            char *s = b; while (*s == ' ') s++;
            snprintf(g_cpu_name, sizeof g_cpu_name, "%s", s);
        }
    }
#endif
    HANDLE h = CreateThread(NULL, 0, machine_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

/* ---- the picture */
static uint8_t *g_px;                      /* BGRA */
static int g_pw, g_ph, g_sc;
static void fill(int x, int y, int w, int h, uint32_t bgra) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < g_ph; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < g_pw; i++) memcpy(g_px + ((size_t)j * g_pw + i) * 4, &bgra, 4);
}
static void text(int x, int y, uint32_t bgra, const char *s) {
    for (; *s; s++, x += 8 * g_sc) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        uint64_t g = g_glyph[ch - 32];
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (g >> (r * 8 + c) & 1) fill(x + c * g_sc, y + r * g_sc, g_sc, g_sc, bgra);
    }
}
#define BG 0xff141414u                     /* 0xAARRGGBB: B, G, R, A in memory */
#define WHITE 0xffe6e6e6u
#define GREY 0xff8c8c8cu
#define GREEN 0xff5ad25au
#define YELLOW 0xffe6c828u
#define RED 0xffeb4646u
static uint32_t rate_colour(float fps) { return fps >= 55 ? GREEN : fps >= 30 ? YELLOW : RED; }
static void num(char *b, size_t n, const char *fmt, float v) { if (v < 0) snprintf(b, n, "-"); else snprintf(b, n, fmt, v); }

int perf_overlay_compose(uint8_t **bgra, int *w, int *h, int win_w, int win_h) {
    int mode = perf_overlay_mode();
    if (mode == PERF_OFF || win_w < 200 || win_h < 100) return 0;
    if (mode == PERF_FULL) machine_start();
    g_sc = win_h >= 1000 ? 2 : 1;
    double t = now_s(), sum = 0, mx = 0;
    int n = 0, slow = 0;
    for (int i = 0; i < g_n; i++) {
        int k = (g_pos - 1 - i + HIST) % HIST;
        if (t - g_at[k] > 1.0) break;
        sum += g_ms[k]; if (g_ms[k] > mx) mx = g_ms[k]; n++;
    }
    for (int i = 0; i < g_nslow && i < SLOWS; i++) slow += t - g_slow_at[(g_nslow - 1 - i) % SLOWS] <= 60.0;
    float fps = n && sum > 0 ? (float)(n * 1000.0 / sum) : 0, avg = n ? (float)(sum / n) : 0;
    char l[16][64];
    int lines = 0, gh = 0;
    snprintf(l[lines++], 64, mode == PERF_FPS ? "%.0f FPS  %.1f ms" : "FPS %.1f  %.1f ms  max %.1f", fps, avg, mx);
    if (mode == PERF_FULL) {
        Machine m;
        EnterCriticalSection(&g_mlock); m = g_mach; LeaveCriticalSection(&g_mlock);
        char a[16], b[16], c[16], d[16];
        gh = 40 * g_sc;
        num(a, sizeof a, "%.1f ms", g_gpu_ms); snprintf(l[lines++], 64, "GPU frame %s  3D %dx%d", a, g_vpw, g_vph);
        num(a, sizeof a, "%.1f", g_main_ms); num(b, sizeof b, "%.1f", g_render_ms); snprintf(l[lines++], 64, "CPU ms/frame main %s render %s", a, b);
        snprintf(l[lines++], 64, "frames over 33 ms, last minute: %d", slow);
        num(a, sizeof a, "%.0f%%", m.cpu); num(b, sizeof b, "%.0f%%", m.cpu_game); num(c, sizeof c, "%.0f C", m.cpu_temp);
        snprintf(l[lines++], 64, "CPU %s  game %s  %s", a, b, c);
        num(a, sizeof a, "%.1f", m.ram_game < 0 ? -1 : m.ram_game / 1024); num(b, sizeof b, "%.1f", m.ram_used < 0 ? -1 : m.ram_used / 1024); num(c, sizeof c, "%.1f", m.ram_total < 0 ? -1 : m.ram_total / 1024);
        snprintf(l[lines++], 64, "RAM game %s GB  %s / %s GB", a, b, c);
        num(a, sizeof a, "%.0f%%", m.gpu); num(b, sizeof b, "%.0f C", m.gpu_temp); num(c, sizeof c, "%.0f W", m.gpu_w); num(d, sizeof d, "%.0f MHz", m.gpu_mhz);
        snprintf(l[lines++], 64, "GPU %s  %s  %s  %s", a, b, c, d);
        num(a, sizeof a, "%.0f", m.vram_used); num(b, sizeof b, "%.0f", m.vram_total); num(c, sizeof c, "%.0f", m.gtt_used); num(d, sizeof d, "%.0f", m.gtt_total);
        snprintf(l[lines++], 64, m.gtt_total > 0 ? "VRAM %s/%s MB  GTT %s/%s MB" : "VRAM %s/%s MB", a, b, c, d);
        snprintf(l[lines++], 64, "%.36s", g_gpu_name);
        snprintf(l[lines++], 64, "%.36s", g_cpu_name);
    }
    int cols = 0;
    for (int i = 0; i < lines; i++) { int k = (int)strlen(l[i]); if (k > cols) cols = k; }
    int pad = 4 * g_sc, lh = 10 * g_sc;
    int pw = cols * 8 * g_sc + 2 * pad, ph = lines * lh + 2 * pad + (gh ? gh + pad : 0);
    if (mode == PERF_FULL && pw < HIST * 2 * g_sc + 2 * pad) pw = HIST * 2 * g_sc + 2 * pad;
    if (pw > win_w) pw = win_w;
    if (ph > win_h) ph = win_h;
    if (pw != g_pw || ph != g_ph) { free(g_px); g_px = malloc((size_t)pw * ph * 4); g_pw = pw; g_ph = ph; if (!g_px) { g_pw = g_ph = 0; return 0; } }
    fill(0, 0, pw, ph, BG);
    int y = pad;
    text(pad, y, rate_colour(fps), l[0]); y += lh;
    if (gh) {                                  /* the last 120 frames, 0..50 ms, lines at 16.7 and 33.3 ms */
        int x0 = pad, bw = 2 * g_sc;
        for (int i = 0; i < g_n; i++) {
            int k = (g_pos - g_n + i + HIST) % HIST;
            float v = g_ms[k] > 50 ? 50 : g_ms[k];
            int bh = (int)(v / 50 * gh); if (bh < 1) bh = 1;
            fill(x0 + (HIST - g_n + i) * bw, y + gh - bh, bw, bh, g_ms[k] > 33.4f ? RED : g_ms[k] > 17.5f ? YELLOW : GREEN);
        }
        fill(x0, y + gh - (int)(16.7 / 50 * gh), HIST * bw, g_sc, WHITE);      /* over the bars: 60 and 30 frames a second */
        fill(x0, y + gh - (int)(33.3 / 50 * gh), HIST * bw, g_sc, WHITE);
        y += gh + pad;
    }
    for (int i = 1; i < lines; i++, y += lh) text(pad, y, i >= lines - 2 ? GREY : WHITE, l[i]);
    *bgra = g_px; *w = pw; *h = ph;
    return 1;
}
