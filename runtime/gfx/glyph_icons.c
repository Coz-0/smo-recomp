/* Button icons that follow the controller in use. The prompts of play ("(A) Jump", "(L) Run") are glyphs of the game's
 * DeviceFont80 font, characters of Nintendo's private area (U+E0E0 A, U+E0E1 B, U+E0E2 X, U+E0E3 Y, ...), drawn from one
 * glyph sheet: a BC4 texture of 512 x 1024, stored upside down, with 80 x 84 cells every 81 x 85 pixels, 6 to a row, 39
 * glyphs (CMAP index n at column n % 6, row n / 6 from the bottom). When that sheet is uploaded, its cells are changed for
 * the controller's buttons. Xbox pads have their face buttons by position as the Switch has them but named the other way
 * round (the south button is Nintendo's B and Xbox's A), so the sheet's A and B cells trade places, and X and Y; the
 * shoulders say LB RB LT RT and + / - become Menu and View. PlayStation's pads: the face cells get the cross, circle, square
 * and triangle by position, the shoulders L1 R1 L2 R2, + its Options lines. The new marks are drawn here (strokes and rings,
 * antialiased), nothing taken from elsewhere.
 *
 * The sheet is told by its shape, not by its bytes: BC4, 512 x 1024, one level, and glyphs in exactly the 39 cells of six
 * full rows and half of a seventh, the rest empty. The blocks whose pixels change are encoded again; the rest keep their bytes.
 * Which controller: glyph_icons_style() (nnpad_sdl.c). */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "glyph_icons.h"

enum { SHEET_W = 512, SHEET_H = 1024, CELL_W = 80, CELL_H = 84, STEP_W = 81, STEP_H = 85, COLS = 6, GLYPHS = 39 };
enum { CELL_A = 8, CELL_B = 9, CELL_X = 10, CELL_Y = 11, CELL_L = 12, CELL_R = 13, CELL_ZL = 14, CELL_ZR = 15, CELL_PLUS = 22, CELL_MINUS = 23 };   /* CMAP indices: U+E0E0..E3, E4..E7, E0EF, E0F0 */

static void bc4_decode(const uint8_t *b, uint8_t *px) {          /* 8 bytes -> 16 values, row by row */
    uint8_t pal[8];
    pal[0] = b[0]; pal[1] = b[1];
    if (b[0] > b[1]) for (int k = 1; k < 7; k++) pal[1 + k] = (uint8_t)(((7 - k) * b[0] + k * b[1]) / 7);
    else { for (int k = 1; k < 5; k++) pal[1 + k] = (uint8_t)(((5 - k) * b[0] + k * b[1]) / 5); pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
    for (int p = 0; p < 16; p++) px[p] = pal[(bits >> (3 * p)) & 7];
}
static void bc4_encode(const uint8_t *px, uint8_t *b) {          /* 16 values -> 8 bytes: eight steps between min and max */
    uint8_t lo = 255, hi = 0, pal[8];
    for (int p = 0; p < 16; p++) { if (px[p] < lo) lo = px[p]; if (px[p] > hi) hi = px[p]; }
    memset(b, 0, 8);
    b[0] = hi; b[1] = lo;
    if (hi == lo) return;                                         /* a0 == a1: every index 0 gives a0 */
    pal[0] = hi; pal[1] = lo;
    for (int k = 1; k < 7; k++) pal[1 + k] = (uint8_t)(((7 - k) * hi + k * lo) / 7);
    uint64_t bits = 0;
    for (int p = 0; p < 16; p++) {
        int best = 0, err = 256;
        for (int k = 0; k < 8; k++) { int e = px[p] > pal[k] ? px[p] - pal[k] : pal[k] - px[p]; if (e < err) { err = e; best = k; } }
        bits |= (uint64_t)best << (3 * p);
    }
    for (int k = 0; k < 6; k++) b[2 + k] = (uint8_t)(bits >> (8 * k));
}
static uint8_t *block_at(uint8_t *bc4, int x, int y) { return bc4 + ((size_t)(y / 4) * (SHEET_W / 4) + (size_t)(x / 4)) * 8; }
static int cell_x(int n) { return (n % COLS) * STEP_W; }
static int cell_y(int n) { return SHEET_H - (n / COLS) * STEP_H - CELL_H; }    /* top row of the cell in the stored sheet */

static int cell_has_ink(uint8_t *bc4, int n) {                    /* any pixel of the cell over 64 */
    uint8_t px[16];
    for (int y = cell_y(n) & ~3; y < cell_y(n) + CELL_H; y += 4)
        for (int x = cell_x(n) & ~3; x < cell_x(n) + CELL_W; x += 4) {
            bc4_decode(block_at(bc4, x, y), px);
            for (int p = 0; p < 16; p++) {
                int px_x = x + p % 4, px_y = y + p / 4;
                if (px[p] > 64 && px_x >= cell_x(n) && px_x < cell_x(n) + CELL_W && px_y >= cell_y(n) && px_y < cell_y(n) + CELL_H) return 1;
            }
        }
    return 0;
}
int glyph_icons_is_sheet(int w, int h, int levels, int bc4_unorm, uint8_t *bc4) {
    if (w != SHEET_W || h != SHEET_H || levels != 1 || !bc4_unorm) return 0;
    for (int n = 0; n < (SHEET_H / STEP_H) * COLS; n++) if (cell_has_ink(bc4, n) != (n < GLYPHS)) return 0;
    return 1;
}

/* The sheet decoded to one byte a pixel; cells traded there; only the blocks whose pixels changed encoded again. */
static uint8_t g_img[SHEET_H][SHEET_W], g_was[SHEET_H][SHEET_W];
static void sheet_decode(const uint8_t *bc4) {
    uint8_t px[16];
    for (int y = 0; y < SHEET_H; y += 4)
        for (int x = 0; x < SHEET_W; x += 4) {
            bc4_decode(block_at((uint8_t *)bc4, x, y), px);
            for (int p = 0; p < 16; p++) g_img[y + p / 4][x + p % 4] = px[p];
        }
    memcpy(g_was, g_img, sizeof g_img);
}
static void sheet_encode_changed(uint8_t *bc4) {
    uint8_t px[16];
    for (int y = 0; y < SHEET_H; y += 4)
        for (int x = 0; x < SHEET_W; x += 4) {
            int changed = 0;
            for (int p = 0; p < 16; p++) { px[p] = g_img[y + p / 4][x + p % 4]; changed |= px[p] != g_was[y + p / 4][x + p % 4]; }
            if (changed) bc4_encode(px, block_at(bc4, x, y));
        }
}
static void swap_cells(int a, int b) {
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++) {
            uint8_t *p = &g_img[cell_y(a) + y][cell_x(a) + x], *q = &g_img[cell_y(b) + y][cell_x(b) + x], t = *p;
            *p = *q; *q = t;
        }
}

/* ---- drawing over a cell, upright (the sheet is stored upside down). A shape is a union of primitives in cell pixels;
 * each pixel takes its coverage from 4 x 4 samples, black over what is there. */
typedef struct { int kind; float a, b, c, d, e; } Prim;          /* 0 segment (a,b)-(c,d) width e; 1 ring centre (a,b) radius c width d */
typedef struct { Prim p[48]; int n; } Shape;
static float cellpx(int n, int x, int y) { return g_img[cell_y(n) + CELL_H - 1 - y][cell_x(n) + x]; }
static void cellset(int n, int x, int y, float v) { g_img[cell_y(n) + CELL_H - 1 - y][cell_x(n) + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }
static void seg(Shape *s, float x0, float y0, float x1, float y1, float w) { if (s->n < 48) s->p[s->n++] = (Prim){0, x0, y0, x1, y1, w}; }
static void ring(Shape *s, float cx, float cy, float r, float w) { if (s->n < 48) s->p[s->n++] = (Prim){1, cx, cy, r, w, 0}; }
static int inside(const Shape *s, float x, float y) {
    for (int i = 0; i < s->n; i++) {
        const Prim *p = &s->p[i];
        if (p->kind == 0) {
            float dx = p->c - p->a, dy = p->d - p->b, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - p->a) * dx + (y - p->b) * dy) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = x - (p->a + t * dx), ey = y - (p->b + t * dy);
            if (ex * ex + ey * ey <= p->e * p->e / 4) return 1;
        } else {
            float r = sqrtf((x - p->a) * (x - p->a) + (y - p->b) * (y - p->b));
            if (fabsf(r - p->c) <= p->d / 2) return 1;
        }
    }
    return 0;
}
static void paint(int n, const Shape *s) {
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++) {
            int hit = 0;
            for (int k = 0; k < 16; k++) hit += inside(s, x + (k % 4 + 0.5f) / 4, y + (k / 4 + 0.5f) / 4);
            if (hit) cellset(n, x, y, cellpx(n, x, y) * (1 - hit / 16.0f));
        }
}
/* The white disk of a face or +/- button: centre and radius from its ink; everything inside made white again. */
static void clear_disk(int n, float *cx, float *cy, float *r) {
    int x0 = CELL_W, x1 = -1, y0 = CELL_H, y1 = -1;
    for (int y = 0; y < CELL_H; y++) for (int x = 0; x < CELL_W; x++) if (cellpx(n, x, y) > 128) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    *cx = (x0 + x1 + 1) / 2.0f; *cy = (y0 + y1 + 1) / 2.0f; *r = (x1 - x0 + 1) / 2.0f;
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++) { float dx = x + 0.5f - *cx, dy = y + 0.5f - *cy; if (dx * dx + dy * dy < (*r - 2) * (*r - 2)) cellset(n, x, y, 255); }
}
/* The white tab of a shoulder button: its letters (dark pixels with the tab's white on all four sides) made white again;
 * the box they took is returned (where the new text goes). */
static int white_from(int n, int x, int y, int dx, int dy) {
    for (x += dx, y += dy; x >= 0 && x < CELL_W && y >= 0 && y < CELL_H; x += dx, y += dy) if (cellpx(n, x, y) > 200) return 1;
    return 0;
}
static void clear_tab(int n, float *bx0, float *by0, float *bx1, float *by1) {
    static uint8_t ink[CELL_H][CELL_W];
    int lx0 = CELL_W, lx1 = -1, ly0 = CELL_H, ly1 = -1;
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++)
            ink[y][x] = cellpx(n, x, y) < 200 && white_from(n, x, y, -1, 0) && white_from(n, x, y, 1, 0) && white_from(n, x, y, 0, -1) && white_from(n, x, y, 0, 1);
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++)
            if (ink[y][x]) { if (x < lx0) lx0 = x; if (x > lx1) lx1 = x; if (y < ly0) ly0 = y; if (y > ly1) ly1 = y; cellset(n, x, y, 255); }
    *bx0 = (float)lx0; *by0 = (float)ly0; *bx1 = lx1 + 1.0f; *by1 = ly1 + 1.0f;
}
/* Strokes of the few characters the shoulder labels need, in a box 0.62 wide and 1 high (y down). */
static void glyph(Shape *s, char c, float x, float y, float h, float w) {
    float u = h, ax[12], ay[12];
    #define S(a, b, c2, d) seg(s, x + (a) * u, y + (b) * u, x + (c2) * u, y + (d) * u, w)
    #define ARC(cx, cy, rx, ry, t0, t1) do { for (int k = 0; k < 12; k++) { float t = (t0) + ((t1) - (t0)) * k / 11.0f; ax[k] = (cx) + (rx) * cosf(t); ay[k] = (cy) + (ry) * sinf(t); } \
                                             for (int k = 0; k < 11; k++) S(ax[k], ay[k], ax[k + 1], ay[k + 1]); } while (0)
    const float P = 3.14159265f;
    switch (c) {
    case 'L': S(0, 0, 0, 1); S(0, 1, 0.55f, 1); break;
    case 'T': S(0, 0, 0.62f, 0); S(0.31f, 0, 0.31f, 1); break;
    case 'R': S(0, 0, 0, 1); S(0, 0, 0.32f, 0); S(0, 0.5f, 0.32f, 0.5f); ARC(0.32f, 0.25f, 0.25f, 0.25f, -P / 2, P / 2); S(0.3f, 0.5f, 0.6f, 1); break;
    case 'B': S(0, 0, 0, 1); S(0, 0, 0.3f, 0); S(0, 0.48f, 0.34f, 0.48f); S(0, 1, 0.34f, 1);
              ARC(0.3f, 0.24f, 0.24f, 0.24f, -P / 2, P / 2); ARC(0.34f, 0.74f, 0.26f, 0.26f, -P / 2, P / 2); break;
    case '1': S(0.34f, 0, 0.34f, 1); S(0.1f, 0.2f, 0.34f, 0); break;
    case '2': ARC(0.3f, 0.28f, 0.28f, 0.28f, -P, 0.25f * P); S(0.5f, 0.48f, 0.02f, 1); S(0.02f, 1, 0.6f, 1); break;
    default: break;
    }
    #undef S
    #undef ARC
}
static void tab_text(int n, const char *text) {
    float x0, y0, x1, y1;
    clear_tab(n, &x0, &y0, &x1, &y1);
    Shape s = {0};
    float h = (y1 - y0) * 0.92f, w = h * 0.17f, gap = h * 0.16f, width = 0;
    for (const char *c = text; *c; c++) width += h * 0.62f + (c[1] ? gap : 0);
    float x = (x0 + x1 - width) / 2, y = (y0 + y1 - h) / 2;
    for (const char *c = text; *c; c++) { glyph(&s, *c, x, y, h, w); x += h * 0.62f + gap; }
    paint(n, &s);
}
static void disk_symbol(int n, char what) {          /* PlayStation's face symbols, Xbox's Menu (≡) and View (two windows) */
    float cx, cy, r;
    clear_disk(n, &cx, &cy, &r);
    Shape s = {0};
    float w = r * 0.16f, k = r * 0.42f;
    switch (what) {
    case 'x': seg(&s, cx - k, cy - k, cx + k, cy + k, w); seg(&s, cx - k, cy + k, cx + k, cy - k, w); break;
    case 'o': ring(&s, cx, cy, r * 0.45f, w); break;
    case 's': seg(&s, cx - k, cy - k, cx + k, cy - k, w); seg(&s, cx + k, cy - k, cx + k, cy + k, w);
              seg(&s, cx + k, cy + k, cx - k, cy + k, w); seg(&s, cx - k, cy + k, cx - k, cy - k, w); break;
    case 't': { float t = r * 0.5f, hgt = t * 1.5f, top = cy - hgt * 0.62f, bot = cy + hgt * 0.38f;
                seg(&s, cx, top, cx + t, bot, w); seg(&s, cx + t, bot, cx - t, bot, w); seg(&s, cx - t, bot, cx, top, w); break; }
    case 'm': for (int i = -1; i <= 1; i++) seg(&s, cx - k, cy + i * k * 0.62f, cx + k, cy + i * k * 0.62f, w); break;
    case 'v': { float a = k * 0.62f;
                seg(&s, cx - k, cy - k, cx + a * 0.6f, cy - k, w); seg(&s, cx - k, cy - k, cx - k, cy + a * 0.6f, w);
                seg(&s, cx - a * 0.4f, cy - a * 0.4f, cx + k, cy - a * 0.4f, w); seg(&s, cx + k, cy - a * 0.4f, cx + k, cy + k, w);
                seg(&s, cx + k, cy + k, cx - a * 0.4f, cy + k, w); seg(&s, cx - a * 0.4f, cy + k, cx - a * 0.4f, cy - a * 0.4f, w); break; }
    default: break;
    }
    paint(n, &s);
}

int glyph_icons_apply(uint8_t *bc4, int style) {
    if (style != GLYPH_ICONS_XBOX && style != GLYPH_ICONS_PLAYSTATION) return 0;
    sheet_decode(bc4);
    if (style == GLYPH_ICONS_XBOX) {                          /* the same buttons by position, Xbox's names */
        swap_cells(CELL_A, CELL_B);
        swap_cells(CELL_X, CELL_Y);
        tab_text(CELL_L, "LB"); tab_text(CELL_R, "RB"); tab_text(CELL_ZL, "LT"); tab_text(CELL_ZR, "RT");
        disk_symbol(CELL_PLUS, 'm'); disk_symbol(CELL_MINUS, 'v');
    } else {                                                  /* by position: A east circle, B south cross, X north triangle, Y west square */
        disk_symbol(CELL_A, 'o'); disk_symbol(CELL_B, 'x'); disk_symbol(CELL_X, 't'); disk_symbol(CELL_Y, 's');
        tab_text(CELL_L, "L1"); tab_text(CELL_R, "R1"); tab_text(CELL_ZL, "L2"); tab_text(CELL_ZR, "R2");
        disk_symbol(CELL_PLUS, 'm');
    }
    sheet_encode_changed(bc4);
    return 1;
}
