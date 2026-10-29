/* perf_overlay.c - the performance overlay: F10 cycles off, frame rate, full (graphics.ini perf_overlay sets the start).
 *
 * A panel in the window's top-left corner, blended over the window image after the game's picture (nvn_vk_draw.c
 * perf_draw), so pictures of the Capture button and SMO_SHOT* never have it. Frame rate: frames per second and
 * milliseconds over the last second. Full: the panel of TotK recomp's F10 overlay ([doc] valentderah/totk-pc-recomp,
 * MIT, runtime/ui/menu.cpp perf_full_draw, itself after Savage Wind's runtime/perfmon.c), its rows and colours:
 *   - the machine (perf_machine.c): GPU use, temperature, power, memory and clock; CPU use of all processors and of
 *     this process, its temperature; memory of the machine and of the process;
 *   - the game's frames: rate, average and longest of the last second, a graph of the last 120;
 *   - per frame, averaged over half a second: the batch replay (the render thread turning the game's NVN commands
 *     into Vulkan), GPU milliseconds (timestamp queries, written while the full overlay is on), the render thread's
 *     waits for pipelines and for shader stages, texture uploads; and, only the renderer knows them, CPU milliseconds
 *     of the game's main thread and of the render thread, frames over 33.4 ms (two refreshes missed) in the last minute;
 *   - the picture: the window, the game's image (its size and the internal resolution scale), the 3D scene the game
 *     draws in it (its dynamic resolution), the scaling to the window, the frame limit and the present mode.
 * The letters are the game's MessageFont38 (smo_ui.c, read from the RomFS at run time), each drawn once per size at
 * twice that size and halved (smooth at any size). The panel is composed about ten times a second on the overlay's own
 * thread; the render thread only copies the latest picture when there is a new one. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "perf_overlay.h"
#include "perf_machine.h"
#include "../game/smo_ui.h"

static volatile LONG g_mode = -1;          /* PERF_OFF / PERF_FPS / PERF_FULL */
static void thread_start(void);
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
static char g_gpu_name[64];
void perf_overlay_set_gpu(const char *name) { snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : ""); }

/* ---- frames (render thread; read by the overlay's thread under g_flock) */
#define HIST 120
#define SLOWS 4096
typedef struct {
    float ms[HIST]; double at[HIST]; int n, pos;           /* frame times and when each frame was presented (seconds) */
    double slow_at[SLOWS]; int nslow;                      /* when frames over 33.4 ms were presented */
    float replay, gpu, pipe, shader, uploads, main_ms, render_ms;      /* per frame, the last half second; -1: none */
    PerfFrame pic;                                         /* the last frame's picture rows */
    char scale_from[24], present[24];
} Frames;
static Frames g_fr = {.replay = -1, .gpu = -1, .pipe = -1, .shader = -1, .uploads = -1, .main_ms = -1, .render_ms = -1};
static CRITICAL_SECTION g_flock;
static volatile LONG g_win_h = 720;        /* the window's height (the size of the panel follows it) */
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

void perf_overlay_frame(const PerfFrame *f) {
    if (perf_overlay_mode() == PERF_OFF) { g_fr.n = 0; return; }
    thread_start();
    double t = now_s();
    if (!g_render_tid) g_render_tid = GetCurrentThreadId();
    /* sums over half a second, then the averages (and the threads' CPU per frame) are published */
    static double last, main0 = -1, render0 = -1, s_replay, s_gpu, s_pipe, s_shader, s_up;
    static int frames, gpu_n;
    frames++; s_replay += f->replay_ms; s_pipe += f->pipe_wait_ms; s_shader += f->shader_wait_ms; s_up += f->uploads;
    if (f->gpu_ms > 0) { s_gpu += f->gpu_ms; gpu_n++; }
    EnterCriticalSection(&g_flock);
    if (f->wall_ms > 0) {
        g_fr.ms[g_fr.pos] = (float)f->wall_ms; g_fr.at[g_fr.pos] = t; g_fr.pos = (g_fr.pos + 1) % HIST; if (g_fr.n < HIST) g_fr.n++;
        if (f->wall_ms > 33.4) g_fr.slow_at[g_fr.nslow++ % SLOWS] = t;
    }
    g_fr.pic = *f;
    InterlockedExchange(&g_win_h, f->win_h);
    snprintf(g_fr.scale_from, sizeof g_fr.scale_from, "%s", f->scale_from ? f->scale_from : "");
    snprintf(g_fr.present, sizeof g_fr.present, "%s", f->present ? f->present : "");
    g_fr.pic.scale_from = g_fr.pic.present = NULL;
    if (t - last >= 0.5) {
        double m = thread_cpu_ms(g_main_tid), r = thread_cpu_ms(g_render_tid);
        if (last > 0) {
            g_fr.replay = (float)(s_replay / frames); g_fr.pipe = (float)(s_pipe / frames); g_fr.shader = (float)(s_shader / frames);
            g_fr.uploads = (float)(s_up / frames); g_fr.gpu = gpu_n ? (float)(s_gpu / gpu_n) : -1;
            g_fr.main_ms = m >= 0 && main0 >= 0 ? (float)((m - main0) / frames) : -1;
            g_fr.render_ms = r >= 0 && render0 >= 0 ? (float)((r - render0) / frames) : -1;
        }
        main0 = m; render0 = r; last = t; frames = gpu_n = 0;
        s_replay = s_gpu = s_pipe = s_shader = s_up = 0;
    }
    LeaveCriticalSection(&g_flock);
}

/* ---- drawing on the canvas (RGBA, premultiplied; rows from the top) */
static UiImage g_cv;                       /* the picture being composed */
static UiFont *g_font;
static float g_ts, g_em;                   /* font pixels -> picture pixels; the letters' height in picture pixels */
/* The letters at the panel's size, made once per size: each drawn by smo_ui at twice that size and halved (smooth at
 * any size), then copied as coverage. Drawing every letter of every frame through smo_ui cost 19.9 ms a picture at a
 * 720-line window (the mean of 20); the copies are what is left. */
typedef struct { uint8_t *cov; int w, h, ox, oy; float adv; } Glyph;      /* ox, oy: the bitmap's corner from the pen on the baseline */
static Glyph g_gl[96];                     /* ' ' .. '~', and the degree sign (95: a ring, the game's font has none) */
static float g_gl_em;
static void line(float ax, float ay, float bx, float by, float wdt, uint32_t rgba);
static void glyphs_make(void) {
    if (g_gl_em == g_em) return;
    UiFontInfo fi;
    ui_font_info(g_font, &fi);
    float s2 = 2.0f * g_ts;
    int pad = 4, cw = (int)ceilf((float)fi.cell_w * s2) + 2 * pad, ch = (int)ceilf((float)fi.cell_h * s2) + 2 * pad;
    float base2 = (float)pad + (float)fi.baseline * s2;            /* the baseline in the 2x cell, from its top */
    UiImage c2 = {cw, ch, 4, malloc((size_t)cw * ch * 4)};
    UiXf down = {1, 0, 0, 0, -1, (float)ch};
    for (int i = 0; i < 96 && c2.px; i++) {
        Glyph *g = &g_gl[i];
        free(g->cov);
        memset(c2.px, 0, (size_t)cw * ch * 4);
        if (i < 95) {
            uint16_t one[2] = {(uint16_t)(32 + i), 0};
            g->adv = ui_text(&c2, down, g_font, NULL, one, 1, (float)pad, (float)ch - base2, s2, 0xffffffffu) / 2.0f;
        } else {                                   /* the ring, drawn on the 2x cell with line() */
            UiImage keep = g_cv; float r = 0.2f * g_em, cx = (float)pad + 0.16f * g_em + r, cy = base2 - 1.3f * g_em + r;
            g_cv = c2;
            for (int k = 0; k < 12; k++) { float a0 = (float)k * 0.5236f, a1 = a0 + 0.5236f; line(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), 0.14f * g_em, 0xffffffffu); }
            g_cv = keep;
            g->adv = 0.42f * g_em;
        }
        g->w = cw / 2; g->h = ch / 2; g->ox = -pad / 2; g->oy = -(int)(base2 / 2.0f + 0.5f);
        g->cov = malloc((size_t)g->w * g->h);
        for (int y = 0; g->cov && y < g->h; y++)
            for (int x = 0; x < g->w; x++) {
                const uint8_t *q = c2.px + ((size_t)(2 * y) * cw + 2 * x) * 4 + 3, *r = q + (size_t)cw * 4;
                g->cov[(size_t)y * g->w + x] = (uint8_t)((q[0] + q[4] + r[0] + r[4] + 2) / 4);
            }
    }
    free(c2.px);
    g_gl_em = g_em;
}
static const Glyph *glyph_of_char(unsigned char c) { return c == 0xb0 ? &g_gl[95] : &g_gl[c >= 32 && c < 127 ? c - 32 : '?' - 32]; }
static float text_w(const char *s) { float w = 0; for (; *s; s++) w += glyph_of_char((unsigned char)*s)->adv; return w; }
static void blend(uint8_t *q, float cov, uint32_t rgba);
static void text(float x, float y, float row, uint32_t rgba, const char *s) {     /* in a row of height `row` from y, capitals centred */
    int base = (int)(y + row * 0.5f + g_em * 0.36f + 0.5f);
    float pen = x;
    unsigned a = rgba & 255, cr = rgba >> 24, cg = rgba >> 16 & 255, cb = rgba >> 8 & 255;
    for (; *s; s++) {
        const Glyph *g = glyph_of_char((unsigned char)*s);
        int gx = (int)(pen + 0.5f) + g->ox, gy = base + g->oy;
        for (int j = 0; g->cov && j < g->h; j++) {
            int Y = gy + j;
            if (Y < 0 || Y >= g_cv.h) continue;
            for (int i = 0; i < g->w; i++) {
                int X = gx + i;
                unsigned c = g->cov[(size_t)j * g->w + i];
                if (!c || X < 0 || X >= g_cv.w) continue;
                unsigned k = c * a / 255, keep = 255 - k;          /* premultiplied over premultiplied, in integers */
                uint8_t *q = g_cv.px + ((size_t)Y * g_cv.w + X) * 4;
                q[0] = (uint8_t)((cr * k + q[0] * keep + 127) / 255); q[1] = (uint8_t)((cg * k + q[1] * keep + 127) / 255);
                q[2] = (uint8_t)((cb * k + q[2] * keep + 127) / 255); q[3] = (uint8_t)((255 * k + q[3] * keep + 127) / 255);
            }
        }
        pen += g->adv;
    }
}
static void text_right(float x1, float y, float row, uint32_t rgba, const char *s) { text(x1 - text_w(s), y, row, rgba, s); }
static void blend(uint8_t *q, float cov, uint32_t rgba) {          /* rgba not premultiplied, over a premultiplied pixel */
    float a = (float)(rgba & 255) / 255.0f * cov, keep = 1.0f - a;
    if (a <= 0) return;
    for (int c = 0; c < 3; c++) q[c] = (uint8_t)((float)(rgba >> (24 - 8 * c) & 255) * a + (float)q[c] * keep + 0.5f);
    q[3] = (uint8_t)(255.0f * a + (float)q[3] * keep + 0.5f);
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void rrect(float x0, float y0, float x1, float y1, float r, uint32_t rgba) {     /* a rounded rectangle, smooth edges */
    for (int Y = (int)y0 < 0 ? 0 : (int)y0; Y < (int)ceilf(y1) && Y < g_cv.h; Y++)
        for (int X = (int)x0 < 0 ? 0 : (int)x0; X < (int)ceilf(x1) && X < g_cv.w; X++) {
            float px = (float)X + 0.5f, py = (float)Y + 0.5f;
            if (px > x0 + r + 1 && px < x1 - r - 1 && py > y0 + 1 && py < y1 - 1) {      /* inside, away from the edges: the whole pixel */
                blend(g_cv.px + ((size_t)Y * g_cv.w + X) * 4, 1.0f, rgba);
                continue;
            }
            float dx = fmaxf(fmaxf(x0 + r - px, px - (x1 - r)), 0), dy = fmaxf(fmaxf(y0 + r - py, py - (y1 - r)), 0);
            float d = sqrtf(dx * dx + dy * dy) - r;                /* < 0 inside the corner circles */
            float cov = clamp01(0.5f - d) * clamp01(px - x0 + 0.5f) * clamp01(x1 - px + 0.5f) * clamp01(py - y0 + 0.5f) * clamp01(y1 - py + 0.5f);
            if (cov > 0) blend(g_cv.px + ((size_t)Y * g_cv.w + X) * 4, cov, rgba);
        }
}
static void line(float ax, float ay, float bx, float by, float wdt, uint32_t rgba) {     /* a segment, smooth */
    float x0 = fminf(ax, bx) - wdt, x1 = fmaxf(ax, bx) + wdt, y0 = fminf(ay, by) - wdt, y1 = fmaxf(ay, by) + wdt;
    float vx = bx - ax, vy = by - ay, ll = vx * vx + vy * vy;
    for (int Y = (int)y0 < 0 ? 0 : (int)y0; Y <= (int)y1 && Y < g_cv.h; Y++)
        for (int X = (int)x0 < 0 ? 0 : (int)x0; X <= (int)x1 && X < g_cv.w; X++) {
            float px = (float)X + 0.5f - ax, py = (float)Y + 0.5f - ay, k = ll > 0 ? clamp01((px * vx + py * vy) / ll) : 0;
            float dx = px - k * vx, dy = py - k * vy, cov = clamp01(wdt * 0.5f + 0.5f - sqrtf(dx * dx + dy * dy));
            if (cov > 0) blend(g_cv.px + ((size_t)Y * g_cv.w + X) * 4, cov, rgba);
        }
}

/* TotK's colours (menu.cpp perf_full_draw), 0xRRGGBBAA */
#define C_GPU 0x8cd44fffu
#define C_CPU 0x2e96ccffu
#define C_RAM 0xcc73b3ffu
#define C_FPS 0xf2f2f2ffu
#define C_WAIT 0xedc254ffu
#define C_PIC 0x9eb3f2ffu
#define C_TEXT 0xffffffffu
#define C_DIM 0x808080ffu
#define C_BG 0x0f0f0f99u                   /* ImGui's window background at TotK's 60 % */
#define C_PLOT_BG 0x294a7a8au              /* ImGui's frame background */
#define C_PLOT 0x9c9c9cffu                 /* ImGui's plot line */

static void fmt(char *b, size_t n, const char *f, float v) { if (v < 0) snprintf(b, n, "-"); else snprintf(b, n, f, v); }
static float gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

typedef struct { const char *label; uint32_t colour; char value[48]; } Row;
/* a table of label + right-aligned value rows, the labels' column stretching to `w` */
static float rows_w(const Row *r, int n, float gap) { float w = 0; for (int i = 0; i < n; i++) w = fmaxf(w, text_w(r[i].label) + gap + text_w(r[i].value)); return w; }
static float rows_draw(const Row *r, int n, float x, float y, float w, float row) {
    for (int i = 0; i < n; i++, y += row) { text(x, y, row, r[i].colour, r[i].label); text_right(x + w, y, row, strcmp(r[i].value, "-") ? C_TEXT : C_DIM, r[i].value); }
    return y;
}

/* The frames of the last second: rate, average, longest. */
static void last_second(const Frames *f, double t, float *fps, float *avg, float *mx) {
    double sum = 0, m = 0;
    int n = 0;
    for (int i = 0; i < f->n; i++) {
        int k = (f->pos - 1 - i + HIST) % HIST;
        if (t - f->at[k] > 1.0) break;
        sum += f->ms[k]; if (f->ms[k] > m) m = f->ms[k]; n++;
    }
    *fps = n && sum > 0 ? (float)(n * 1000.0 / sum) : 0; *avg = n ? (float)(sum / n) : 0; *mx = (float)m;
}

/* The full panel or the frame-rate line on the canvas; returns 0 when there is nothing to draw. U: the
 * height of a row in canvas pixels. */
static int compose(int mode, float U, const PerfMachine *m, const char *cpu_name) {
    Frames f;
    EnterCriticalSection(&g_flock); f = g_fr; LeaveCriticalSection(&g_flock);
    double t = now_s();
    float fps, avg, mx;
    last_second(&f, t, &fps, &avg, &mx);
    UiFontInfo fi;
    ui_font_info(g_font, &fi);
    g_em = 0.72f * U; g_ts = g_em / (float)fi.height;
    glyphs_make();
    float pad = 0.55f * U, gap = 0.9f * U;
    char b[64];
    if (mode == PERF_FPS) {                                        /* TotK's frame-rate line: a dark box, the rate in white */
        snprintf(b, sizeof b, "%.0f FPS  %.1f ms", fps, avg);
        int w = (int)(text_w(b) + 2 * pad + 1) & ~1, h = (int)(U + pad + 1) & ~1;
        if (g_cv.w != w || g_cv.h != h) { free(g_cv.px); g_cv.px = malloc((size_t)w * h * 4); g_cv.w = w; g_cv.h = h; g_cv.ch = 4; }
        if (!g_cv.px) return 0;
        memset(g_cv.px, 0, (size_t)w * h * 4);
        rrect(0, 0, (float)w, (float)h, 0.3f * U, 0x00000096u);
        text(pad, pad * 0.5f, U, 0xffffffebu, b);
        return 1;
    }
    /* the machine and the frames: label, three right-aligned numbers per row (TotK's widths: samples of 8s) */
    static const char *const lab[4] = {"GPU", "VRAM", "CPU", "RAM"};
    static const uint32_t lab_c[4] = {C_GPU, C_GPU, C_CPU, C_RAM};
    char v[5][3][32];
    fmt(v[0][0], 32, "%.0f%%", m->gpu); fmt(v[0][1], 32, "%.0f \xb0" "C", m->gpu_temp); fmt(v[0][2], 32, "%.0f W", m->gpu_w);
    fmt(v[1][0], 32, "%.1f GB", gb(m->vram_used)); fmt(v[1][1], 32, "/ %.1f GB", gb(m->vram_total)); fmt(v[1][2], 32, "%.0f MHz", m->gpu_mhz);
    fmt(v[2][0], 32, "%.0f%%", m->cpu); fmt(v[2][1], 32, "%.0f \xb0" "C", m->cpu_temp); fmt(v[2][2], 32, "game %.0f%%", m->cpu_game);
    fmt(v[3][0], 32, "%.1f GB", gb(m->ram_used)); fmt(v[3][1], 32, "/ %.1f GB", gb(m->ram_total)); fmt(v[3][2], 32, "game %.1f GB", gb(m->ram_game));
    snprintf(v[4][0], 32, "%.1f ms", avg); snprintf(v[4][1], 32, "max %.1f ms", mx); v[4][2][0] = 0;
    float c0 = fmaxf(text_w("VRAM"), text_w("888 FPS")), c1 = fmaxf(text_w("100%"), text_w("888.8 GB")), c2 = fmaxf(text_w("188 \xb0" "C"), fmaxf(text_w("/ 888.8 GB"), text_w("max 888.8 ms"))),
          c3 = fmaxf(fmaxf(text_w("8888 MHz"), text_w("game 100%")), text_w("game 888.8 GB"));
    float w1 = c0 + c1 + c2 + c3 + 3 * gap;
    /* per frame */
    Row wr[8] = {{"GPU frame prep", C_WAIT}, {"GPU busy per frame", C_WAIT}, {"Pipeline waits", C_WAIT}, {"Shader waits", C_WAIT},
                 {"Texture uploads", C_WAIT}, {"Game thread CPU", C_WAIT}, {"Render thread CPU", C_WAIT}, {"Frames over 33 ms (1 min)", C_WAIT}};
    int slow = 0;
    for (int i = 0; i < f.nslow && i < SLOWS; i++) slow += t - f.slow_at[(f.nslow - 1 - i) % SLOWS] <= 60.0;
    fmt(wr[0].value, 48, "%.2f ms", f.replay); fmt(wr[1].value, 48, "%.2f ms", f.gpu); fmt(wr[2].value, 48, "%.2f ms", f.pipe);
    fmt(wr[3].value, 48, "%.2f ms", f.shader); fmt(wr[4].value, 48, "%.1f per frame", f.uploads); fmt(wr[5].value, 48, "%.2f ms", f.main_ms);
    fmt(wr[6].value, 48, "%.2f ms", f.render_ms); snprintf(wr[7].value, 48, "%d", slow);
    /* the picture */
    const PerfFrame *p = &f.pic;
    Row pr[7] = {{"Window", C_PIC}, {"Game image", C_PIC}, {"Rendered at", C_PIC}, {"3D scene", C_PIC}, {"Scaling", C_PIC}, {"Frame limit", C_PIC}, {"Presentation", C_PIC}};
    snprintf(pr[0].value, 48, "%dx%d", p->win_w, p->win_h);
    if (p->game_w > 0) snprintf(pr[1].value, 48, "%dx%d", p->game_w, p->game_h); else snprintf(pr[1].value, 48, "-");
    if (p->image_w > 0) snprintf(pr[2].value, 48, "%dx%d, %.0f%% (%s)", p->image_w, p->image_h, p->scale * 100.0f, f.scale_from); else snprintf(pr[2].value, 48, "-");
    if (p->scene_w > 0 && p->game_w > 0) snprintf(pr[3].value, 48, "%dx%d, %.0f%%", p->scene_w, p->scene_h, p->scene_w * 100.0 / p->game_w); else snprintf(pr[3].value, 48, "-");
    snprintf(pr[4].value, 48, "%s", p->image_w <= 0 ? "-" : p->image_w == p->win_w && p->image_h == p->win_h ? "none (1:1)" : "bilinear");
    fmt(pr[5].value, 48, "%.0f FPS", p->rate_hz > 0 ? p->rate_hz : -1);
    snprintf(pr[6].value, 48, "%s", f.present[0] ? f.present : "-");
    float w = fmaxf(w1, fmaxf(rows_w(wr, 8, gap), rows_w(pr, 7, gap)));
    w = fmaxf(w, fmaxf(text_w(g_gpu_name), text_w(cpu_name)));
    float plot_h = 2.2f * U, sp = 0.35f * U;
    int foot = (g_gpu_name[0] != 0) + (cpu_name[0] != 0);
    int W = ((int)(w + 2 * pad) + 1) & ~1, H = ((int)(pad * 2 + 5 * U + sp + plot_h + sp + 8 * U + sp + 7 * U + (foot ? sp : 0) + foot * U) + 1) & ~1;
    if (g_cv.w != W || g_cv.h != H) { free(g_cv.px); g_cv.px = malloc((size_t)W * H * 4); g_cv.w = W; g_cv.h = H; g_cv.ch = 4; }
    if (!g_cv.px) return 0;
    memset(g_cv.px, 0, (size_t)W * H * 4);
    rrect(0, 0, (float)W, (float)H, 0.45f * U, C_BG);
    float x = pad, y = pad, xr = (float)W - pad;
    for (int r = 0; r < 5; r++, y += U) {
        if (r < 4) text(x, y, U, lab_c[r], lab[r]);
        else { snprintf(b, sizeof b, "%.0f FPS", fps); text(x, y, U, C_FPS, b); }
        float ends[3] = {x + c0 + gap + c1, x + c0 + c1 + c2 + 2 * gap, xr};
        for (int c = 0; c < 3; c++) if (v[r][c][0]) text_right(ends[c], y, U, strcmp(v[r][c], "-") ? C_TEXT : C_DIM, v[r][c]);
    }
    y += sp;
    rrect(x, y, xr, y + plot_h, 0, C_PLOT_BG);                    /* the last 120 frames, 0 .. max(50 ms, the longest) */
    {   float top = fmaxf(50.0f, mx), px0 = x + 1, pw = xr - x - 2, ph = plot_h - 2;
        for (int i = 1; i < f.n; i++) {
            int k0 = (f.pos - f.n + i - 1 + HIST) % HIST, k1 = (k0 + 1) % HIST;
            float a = fminf(f.ms[k0], top) / top, c = fminf(f.ms[k1], top) / top, s = pw / (float)(HIST - 1);
            float xa = px0 + (float)(HIST - f.n + i - 1) * s;
            line(xa, y + 1 + ph * (1 - a), xa + s, y + 1 + ph * (1 - c), fmaxf(1.0f, U / 11.0f), C_PLOT);
        }
    }
    y += plot_h + sp;
    y = rows_draw(wr, 8, x, y, xr - x, U) + sp;
    y = rows_draw(pr, 7, x, y, xr - x, U);
    if (foot) y += sp;
    if (g_gpu_name[0]) { text(x, y, U, C_DIM, g_gpu_name); y += U; }
    if (cpu_name[0]) text(x, y, U, C_DIM, cpu_name);
    return 1;
}

/* ---- the overlay's thread: the machine twice a second (full overlay only), the picture ten times a second */
static CRITICAL_SECTION g_plock;
static uint8_t *g_pic;                     /* the latest picture, RGBA premultiplied */
static int g_pic_w, g_pic_h, g_pic_margin;
static unsigned g_pic_ver;
static int load_font(void) {
    static int tried;
    if (tried) return g_font != NULL;
    tried = 1;
    size_t n = 0, fn = 0;
    uint8_t *arc = ui_arc("LocalizedData/USen/LayoutData/FontData.szs", &n);
    const uint8_t *b = arc ? ui_sarc(arc, n, "MessageFont38.bffnt", &fn) : NULL;
    g_font = b ? ui_font(b, fn) : NULL;
    free(arc);
    if (!g_font) printf("perf overlay: the game's MessageFont38 could not be read: no overlay\n");
    return g_font != NULL;
}
static void publish(int ok) {
    uint8_t *out = ok ? malloc((size_t)g_cv.w * g_cv.h * 4) : NULL;
    if (out) memcpy(out, g_cv.px, (size_t)g_cv.w * g_cv.h * 4);
    EnterCriticalSection(&g_plock);
    free(g_pic); g_pic = out; g_pic_w = out ? g_cv.w : 0; g_pic_h = out ? g_cv.h : 0; g_pic_margin = (int)fmaxf(8.0f, (float)g_win_h / 90.0f); g_pic_ver++;
    LeaveCriticalSection(&g_plock);
}
static DWORD WINAPI overlay_thread(LPVOID arg) {
    (void)arg;
    PerfMachine m = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    char cpu_name[64];
    perf_machine_cpu_name(cpu_name, sizeof cpu_name);
    double sampled = 0;
    for (int was = PERF_OFF;; Sleep(100)) {
        int mode = perf_overlay_mode();
        float u = fmaxf(17.0f, (float)g_win_h / 48.0f);              /* a row: 22.5 pixels on a window 1080 high, at least 17 */
        if (mode == PERF_FULL && now_s() - sampled >= 0.5) { perf_machine_sample(&m); sampled = now_s(); }
        if (mode != PERF_OFF || was != PERF_OFF) {
            int ok = mode != PERF_OFF && load_font();
            double t0 = now_s();
            ok = ok && compose(mode, u, &m, cpu_name);
            static int n; static double sum;
            if (ok && mode == PERF_FULL && n < 21 && ++n > 1 && (sum += now_s() - t0, n == 21))      /* the mean of 20, the font's loading left out */
                printf("perf overlay: the full panel (%dx%d) composed in %.1f ms on its own thread (mean of 20), ten times a second\n", g_cv.w, g_cv.h, sum * 1000.0 / 20);
            publish(ok);
        }
        was = mode;
    }
    return 0;
}
static void thread_start(void) {          /* render thread, the first frame with the overlay on */
    static int started;
    if (started) return;
    started = 1;
    InitializeCriticalSection(&g_flock); InitializeCriticalSection(&g_plock);
    HANDLE h = CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

int perf_overlay_picture(uint8_t *dst, size_t cap, int *w, int *h, unsigned *version, int *margin) {
    if (perf_overlay_mode() == PERF_OFF || !g_pic_ver) return 0;
    int r;
    EnterCriticalSection(&g_plock);
    *w = g_pic_w; *h = g_pic_h; *margin = g_pic_margin;
    if (!g_pic) r = 0;
    else if (*version == g_pic_ver) r = 1;
    else if (cap < (size_t)g_pic_w * g_pic_h * 4) r = -1;
    else { memcpy(dst, g_pic, (size_t)g_pic_w * g_pic_h * 4); *version = g_pic_ver; r = 2; }
    LeaveCriticalSection(&g_plock);
    return r;
}
