/* smo_aspect.c - the game's 3D views at the window's shape.
 *
 * The game builds every view for the Switch's 16:9 screen. tools/lifter/game_patches.py puts a call to rt_persp at
 * each read of sead's PerspectiveProjection tan(fovy / 2) (+0xac) and aspect (+0xb0): the matrix and the
 * screen-to-camera steps. A 16:9 view is fitted to the window (the swapchain's size, smo_aspect_window):
 *   - wider than 16:9 (21:9, 32:9): the same vertical view, more to the sides (aspect times the shape);
 *   - narrower (16:10, 4:3): the same horizontal view, more above and below (aspect times the shape, tan(fovy / 2)
 *     divided by it).
 * Only the world's cameras (far plane over 10000); views of another shape and the HUD's layouts are left alone. SMO_ASPECT=0: every view stays as the game made it (16:9, stretched to the
 * window as before). SMO_ASPECT_TEST=<w/h>: that shape whatever the window (tests). */
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

static volatile float g_shape = 1.0f;              /* window aspect / 16:9; 1: views left alone */
static int g_on = -1;
static float g_test;

static int aspect_on(void) {
    if (g_on < 0) {
        const char *e = getenv("SMO_ASPECT"), *t = getenv("SMO_ASPECT_TEST");
        g_on = !e || atoi(e) != 0;
        g_test = t ? (float)atof(t) : 0;
    }
    return g_on;
}

/* the renderer, when it makes the swapchain */
void smo_aspect_window(int w, int h) {
    if (!aspect_on() || w <= 0 || h <= 0) { g_shape = 1.0f; return; }
    float a = g_test > 0 ? g_test : (float)w / (float)h, s = a / (16.0f / 9.0f);
    if (fabsf(s - 1.0f) < 0.005f) s = 1.0f;
    if (s != g_shape) printf("smo_aspect: the game's 16:9 views fitted to %.4f (%dx%d%s)\n", a, w, h, g_test > 0 ? ", SMO_ASPECT_TEST" : "");
    g_shape = s;
}

float smo_aspect_shape(void) { return g_shape; }

/* What rt_persp wrote back into a projection, and what the game had set there: a read that finds the written values
 * starts again from the game's (a view is not widened twice), and a projection the game has set again is widened
 * from its new values. */
static struct { uint64_t proj; uint32_t set_t, set_a, wrote_t, wrote_a; } g_wrote[64];
static unsigned g_wrote_next;
static SRWLOCK g_wrote_lock = SRWLOCK_INIT;

void rt_persp(uint64_t proj, uint32_t *tan_fovy, uint32_t *aspect) {
    float s = g_shape, t, a;
    if (s == 1.0f) return;
    AcquireSRWLockExclusive(&g_wrote_lock);
    int k = -1;
    for (int i = 0; i < 64; i++) if (g_wrote[i].proj == proj) { k = i; break; }
    if (k >= 0 && g_wrote[k].wrote_t == *tan_fovy && g_wrote[k].wrote_a == *aspect) { *tan_fovy = g_wrote[k].set_t; *aspect = g_wrote[k].set_a; }
    uint32_t set_t = *tan_fovy, set_a = *aspect;
    memcpy(&t, tan_fovy, 4); memcpy(&a, aspect, 4);
    {   /* SMO_ASPECT_LOG=1: each kind of view the first time (aspect, fovy, near, far; by value: many are copies) */
        static int log = -1; static float seen[128][4]; static int n;
        if (log < 0) log = getenv("SMO_ASPECT_LOG") != NULL;
        float nf[2]; memcpy(nf, (const void *)(uintptr_t)(proj + 0x98), 8);
        float key[4] = {a, t, nf[0], nf[1]};
        int known = 0;
        for (int i = 0; i < n && !known; i++) known = !memcmp(seen[i], key, sizeof key);
        if (log && !known && n < 128) { memcpy(seen[n++], key, sizeof key); printf("smo_aspect: view %#llx: aspect %.4f, fovy %.2f deg, near %g far %g\n", (unsigned long long)proj, a, 2.0 * atan(t) * 57.29578, nf[0], nf[1]); }
    }
    float far_;
    memcpy(&far_, (const void *)(uintptr_t)(proj + 0x9c), 4);
    /* the world's cameras only (far plane 100000 or 500000, SMO_ASPECT_LOG): the HUD's and the 2D screens' layouts are
     * 16:9 views too (fovy 15, far 10000), and fitted they shrank into the middle of the frame (45 empty rows above
     * and below of 900 at 16:10, Cap Kingdom 27/10/2026) */
    if (fabsf(a - 16.0f / 9.0f) >= 0.01f || far_ <= 10001.0f) { ReleaseSRWLockExclusive(&g_wrote_lock); return; }
    a *= s;
    if (s < 1.0f) t /= s;                          /* taller than 16:9: the width kept, more height */
    memcpy(tan_fovy, &t, 4); memcpy(aspect, &a, 4);
    /* Written back (with fovy and its sine and cosine at +0xa0, +0xa4, +0xa8), so the game's other uses of the view see
     * the one that is drawn, as Savage Wind does for its world camera (there, its shadow cascades). Here no difference
     * was seen: 16:10 screenshots with and without differ by 0.4-2.9 grey levels on average (Metro, frames 1800-2400),
     * moving things between two runs. SMO_ASPECT_WRITE=0: the matrix only. */
    static int write = -1;
    if (write < 0) write = !(getenv("SMO_ASPECT_WRITE") && !atoi(getenv("SMO_ASPECT_WRITE")));
    if (write) {
        float half = atanf(t), fovy = 2.0f * half, sn = sinf(half), cs = cosf(half);
        memcpy((void *)(uintptr_t)(proj + 0xa0), &fovy, 4); memcpy((void *)(uintptr_t)(proj + 0xa4), &sn, 4);
        memcpy((void *)(uintptr_t)(proj + 0xa8), &cs, 4); memcpy((void *)(uintptr_t)(proj + 0xac), &t, 4);
        memcpy((void *)(uintptr_t)(proj + 0xb0), &a, 4);
        if (k < 0) k = (int)(g_wrote_next++ & 63);
        g_wrote[k].proj = proj; g_wrote[k].set_t = set_t; g_wrote[k].set_a = set_a; g_wrote[k].wrote_t = *tan_fovy; g_wrote[k].wrote_a = *aspect;
    }
    ReleaseSRWLockExclusive(&g_wrote_lock);
}
