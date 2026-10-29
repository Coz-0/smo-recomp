/* perf_overlay.h - the performance overlay (perf_overlay.c): F10 cycles off, frame rate, full. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };
int perf_overlay_mode(void);                 /* SMO_PERF_OVERLAY=0/1/2 or graphics.ini perf_overlay at start, then F10 */
void perf_overlay_toggle(void);              /* F10 (display.c): off -> frame rate -> full -> off */
int perf_overlay_wants_gpu(void);            /* the renderer writes its timestamp queries for the overlay */
void perf_overlay_set_gpu(const char *name); /* the Vulkan device's name */

/* What the renderer knows of one presented frame (render thread, at every present while the overlay is on). */
typedef struct {
    double wall_ms;                          /* since the last present */
    float gpu_ms;                            /* GPU busy time of a frame already finished; -1: none */
    float replay_ms, pipe_wait_ms, shader_wait_ms;       /* this frame: batch replay, waits for pipelines and for shader stages */
    int uploads;                             /* this frame: texture uploads */
    int scene_w, scene_h;                    /* the frame's main 3D pass, the game's pixels (its dynamic resolution); 0: none */
    int win_w, win_h;                        /* the window image */
    int game_w, game_h, image_w, image_h;    /* the game's window texture: its size and its image's (the internal resolution) */
    float scale;                             /* the internal resolution scale */
    const char *scale_from;                  /* where it came from ("auto", "graphics.ini", "SMO_VK_SCALE") */
    float rate_hz;                           /* the frame limit: display.c's refresh clock */
    const char *present;                     /* the present mode */
} PerfFrame;
void perf_overlay_frame(const PerfFrame *f);

/* The overlay's latest pictures (RGBA8, alpha premultiplied, w x h), composed on the overlay's own thread about ten times
 * a second: the panel, for the top-left corner *margin pixels in, and a notice, for the top middle. *version (in: the one
 * the caller has) changes with every new picture. Returns 0: nothing to draw; 1: the caller's picture is still the
 * latest; 2: a new one copied into dst; -1: dst holds less than w x h x 4 bytes (nothing copied). */
enum { PERF_PANEL = 0, PERF_NOTICE = 1 };
int perf_overlay_picture(int which, uint8_t *dst, size_t cap, int *w, int *h, unsigned *version, int *margin);
/* A line shown for 2.5 s at the top of the window whatever the overlay's mode (F9's new resolution); render thread */
void perf_overlay_notice(const char *text, int win_h);
int perf_overlay_default(void);              /* gfx_options.c: graphics.ini perf_overlay (off / fps / full) */
