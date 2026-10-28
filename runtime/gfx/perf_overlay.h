/* perf_overlay.h - the performance overlay (perf_overlay.c): F10 cycles off, frame rate, full. */
#pragma once
#include <stdint.h>

enum { PERF_OFF = 0, PERF_FPS = 1, PERF_FULL = 2 };
int perf_overlay_mode(void);                 /* SMO_PERF_OVERLAY=0/1/2 or graphics.ini perf_overlay at start, then F10 */
void perf_overlay_toggle(void);              /* F10 (display.c): off -> frame rate -> full -> off */
int perf_overlay_wants_gpu(void);            /* the renderer writes its timestamp queries for the overlay */
void perf_overlay_set_gpu(const char *name); /* the Vulkan device's name */
/* render thread, at every present: ms since the last present, the GPU ms of a frame already finished (-1: none), the
 * size of the frame's 3D scene (0: none) */
void perf_overlay_frame(double wall_ms, float gpu_ms, int vpw, int vph);
/* render thread: the overlay's picture (BGRA, w x h) for a window of win_w x win_h; 0 = nothing to draw */
int perf_overlay_compose(uint8_t **bgra, int *w, int *h, int win_w, int win_h);
int perf_overlay_default(void);              /* gfx_options.c: graphics.ini perf_overlay (off / fps / full) */
