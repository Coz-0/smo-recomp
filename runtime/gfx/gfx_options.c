/* Graphics options: config\graphics.ini.
 *
 * Path: SMO_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SMO_CONFIG names (SMO_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SMO_RENDERER, SMO_VK_DEVICE, SMO_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"

static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\r\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\r\n"
"; Every value below is the default; the defaults draw exactly what the game draws on the console.\r\n"
"\r\n"
"; renderer = vulkan | opengl\r\n"
";   vulkan: the default. opengl: the first renderer (NVIDIA only), kept as a fallback.\r\n"
"renderer = vulkan\r\n"
"\r\n"
"; window = borderless_window | borderless_fullscreen | windowed | fullscreen\r\n"
";   borderless_window:     a window without a frame or title bar, the size below, centred on the screen. Drag it\r\n"
";                          anywhere to move it; double-click it for borderless_fullscreen and back.\r\n"
";   borderless_fullscreen: no frame, covers the monitor the window is on (the desktop keeps its resolution).\r\n"
";   windowed:              a normal window with a title bar, the client size below.\r\n"
";   fullscreen:            as borderless_fullscreen, and always on top. (Exclusive fullscreen with a mode change is\r\n"
";                          not implemented.)\r\n"
";   F11 switches between covering the monitor and the window while the game runs.\r\n"
"window = borderless_window\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; unfocused = run | pause\r\n"
";   What the game does while its window is not the active window (another program has the focus) or is minimised.\r\n"
";   run:   it keeps running; pads and the keyboard count only while the window is active.\r\n"
";   pause: it stops until the window is active again, as the console stops the game while the HOME menu is open.\r\n"
"unfocused = run\r\n"
"\r\n"
"; vsync = clock | on | off | mailbox      (Vulkan)\r\n"
";   clock:   frames are shown at once (mailbox when the GPU offers it, else immediate) and the game is paced by the\r\n"
";            program's own 60 Hz clock. Lowest latency; can tear only when mailbox is missing.\r\n"
";   on:      the display's own vertical sync (FIFO). No tearing; on a display that is not 60 Hz the two clocks drift.\r\n"
";   off:     immediate presentation, tearing allowed. The game still runs at 60 frames per second.\r\n"
";   mailbox: mailbox or nothing (falls back to clock's choice with a message when the GPU does not offer it).\r\n"
"vsync = clock\r\n"
"\r\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\r\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\r\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\r\n"
"gpu = auto\r\n"
"\r\n"
"; console_mode = docked | handheld\r\n"
";   What nn::oe::GetOperationMode answers. Leave it at docked: handheld is not measured on PC.\r\n"
"console_mode = docked\r\n"
"\r\n"
"; resolution_scale = auto | 0.25 .. 1.0\r\n"
";   Internal resolution scale (Vulkan only; OpenGL ignores it). 1.0 = the game's own (its picture is 1600x900).\r\n"
";   0.75 = 1200x675, 0.5 = 800x450: the main switch for a weak GPU. The whole picture is drawn smaller, the 2D\r\n"
";   interface too (it gets softer), and is scaled to the window size. auto: the window's size at start, at most\r\n"
";   1.0 (a 1280x720 window: 0.8), so no pixel is drawn only to be scaled down.\r\n"
";   F9 / Shift+F9 step it while the game runs (auto, 1.0, 0.85, 0.75, 0.67, 0.5) and write it here.\r\n"
"resolution_scale = auto\r\n"
"\r\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\r\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\r\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\r\n"
"anisotropic_filtering = game\r\n"
"\r\n"
"; shader_wait = hold | skip      (Vulkan)\r\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\r\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: during play the draw is\r\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\r\n"
"shader_wait = hold\r\n"
"\r\n"
"; profile = custom | low\r\n"
";   custom: every key of this file as written. low: the set for a weak or integrated GPU: resolution_scale = 0.5,\r\n"
";   anisotropic_filtering = game, vsync = clock; those three keys of the file are then not used. The log says which\r\n"
";   profile is active.\r\n"
"profile = custom\r\n"
"\r\n"
"; frame_time_overlay = off | on\r\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\r\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\r\n"
"frame_time_overlay = off\r\n"
"\r\n"
"; perf_overlay = off | fps | full\r\n"
";   The performance overlay in the window's corner at start; F10 cycles it while the game runs. fps: frames per second\r\n"
";   and milliseconds. full: the machine's GPU, CPU and memory, a graph of the last frames, the render thread's work\r\n"
";   and waits per frame, the GPU time of a frame, and the picture's sizes (the 3D scene's too), scaling and pacing.\r\n"
"perf_overlay = off\r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SMO_GRAPHICS_CONFIG"), *c = getenv("SMO_CONFIG");
    char tmp[MAX_PATH], exe[MAX_PATH];
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 > s) s = s2;
        if (s) { *s = 0; snprintf(out, MAX_PATH, "%s\\graphics.ini", tmp); } else snprintf(out, MAX_PATH, "graphics.ini");
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\graphics.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
static BOOL CALLBACK opt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 0.0f; o->window_mode = 3;
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!_stricmp(key, "renderer")) o->renderer = !_stricmp(val, "opengl") || !_stricmp(val, "gl");
        else if (!_stricmp(key, "window")) o->window_mode = !_stricmp(val, "borderless_fullscreen") || !_stricmp(val, "borderless") ? 1 : !_stricmp(val, "fullscreen") ? 2 : !_stricmp(val, "windowed") ? 0 : 3;
        else if (!_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (!_stricmp(val, "auto")) o->scale = 0.0f; else if (v > 0) o->scale = v; }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "perf_overlay")) o->perf_overlay = !_stricmp(val, "full") ? 2 : !_stricmp(val, "fps") ? 1 : 0;
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "unfocused")) o->pause_unfocused = !_stricmp(val, "pause");
        else unknown++;
    }
    if (f) fclose(f);
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[4] = {"windowed", "borderless_fullscreen", "fullscreen", "borderless_window"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    char scale[16];
    if (o->scale > 0) snprintf(scale, sizeof scale, "%.2f", o->scale); else snprintf(scale, sizeof scale, "auto");
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d unfocused=%s vsync=%s gpu=%s console_mode=%s resolution_scale=%s anisotropic_filtering=%d frame_time_overlay=%s perf_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height,
           o->pause_unfocused ? "pause" : "run", k_vs[o->present], o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", scale, o->aniso, o->overlay ? "on" : "off", o->perf_overlay == 2 ? "full" : o->perf_overlay ? "fps" : "off",
           unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%s is ignored by the OpenGL renderer (Vulkan only)\n", scale);
    if (o->scale != 1.0f && o->scale > 0 && (o->scale < 0.25f || o->scale > 1.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 1.0: the nearest of the two is used\n", o->scale);
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}

int perf_overlay_default(void) { return gfx_options()->perf_overlay; }      /* perf_overlay.c */

/* F9 (nvn_vk_draw.c rs_step): the resolution_scale line of graphics.ini set to `v` (0 = auto) in place, the rest of
 * the file kept; a file without the key gets it at the end. 1 = written. */
int gfx_options_write_scale(float v) {
    const char *path = gfx_options()->path;
    FILE *f = fopen(path, "rb");
    char *old = NULL, *out;
    long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); old = malloc((size_t)n + 1); if (old && fread(old, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f); }
    if (!old) { old = malloc(1); n = 0; }
    if (!old) return 0;
    old[n] = 0;
    char val[32];
    if (v > 0) snprintf(val, sizeof val, "%.2f", v); else snprintf(val, sizeof val, "auto");
    out = malloc((size_t)n + 64);
    if (!out) { free(old); return 0; }
    size_t o = 0;
    int done = 0;
    for (char *l = old; *l;) {                     /* line by line: the first active resolution_scale line is replaced */
        char *e = strchr(l, '\n'), *k = l;
        size_t len = e ? (size_t)(e - l + 1) : strlen(l);
        while (*k == ' ' || *k == '\t') k++;
        if (!done && !_strnicmp(k, "resolution_scale", 16) && (k[16] == ' ' || k[16] == '=' || k[16] == '\t')) {
            o += (size_t)sprintf(out + o, "resolution_scale = %s%s", val, len > 1 && l[len - 2] == '\r' ? "\r\n" : "\n");
            done = 1;
        } else { memcpy(out + o, l, len); o += len; }
        l += len;
    }
    if (!done) o += (size_t)sprintf(out + o, "%sresolution_scale = %s\r\n", o && out[o - 1] != '\n' ? "\r\n" : "", val);
    int ok = (f = fopen(path, "wb")) != NULL && fwrite(out, 1, o, f) == o;
    if (f && fclose(f) != 0) ok = 0;
    free(old); free(out);
    return ok;
}
