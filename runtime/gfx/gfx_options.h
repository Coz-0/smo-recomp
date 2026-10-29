/* Graphics options: config\graphics.ini. See gfx_options.c. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SMO_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless fullscreen (desktop size), 2 fullscreen (borderless, topmost, covering the
                              * monitor), 3 borderless window (the default: no frame, width x height, centred) */
    int width, height;       /* client size of the windowed modes (windowed, borderless window) */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SMO_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SMO_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for, 0 = auto (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 1.0; env SMO_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int perf_overlay;        /* the performance overlay at start: 0 off, 1 frame rate, 2 full (perf_overlay.c; F10 cycles) */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a scene the draw is left out until they are ready; nvn_vk_pipelines.c skip_on; env SMO_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int pause_unfocused;     /* unfocused: 0 run (the game goes on while the window is not active), 1 pause (nnmisc.c oe_watch stops it) */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
#endif
