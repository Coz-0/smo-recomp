/* Button icons that follow the controller in use. The prompts of play ("(A) Jump", "(L) Run") are glyphs of the game's
 * DeviceFont80 font, characters of Nintendo's private area (U+E0E0 A, U+E0E1 B, U+E0E2 X, U+E0E3 Y, ...), drawn from one
 * glyph sheet: a BC4 texture of 512 x 1024, stored upside down, with 80 x 84 cells every 81 x 85 pixels, 6 to a row, 39
 * glyphs (CMAP index n at column n % 6, row n / 6 from the bottom). When that sheet is uploaded, its cells are changed for
 * the controller's buttons. Xbox pads have their face buttons by position as the Switch has them but named the other way
 * round (the south button is Nintendo's B and Xbox's A), so the sheet's A and B cells trade places, and X and Y.
 *
 * The sheet is told by its shape, not by its bytes: BC4, 512 x 1024, one level, and glyphs in exactly the 39 cells of six
 * full rows and half of a seventh, the rest empty. The blocks whose pixels change are encoded again; the rest keep their bytes.
 * Which controller: glyph_icons_style() (nnpad_sdl.c). */
#include <stdint.h>
#include <string.h>
#include "glyph_icons.h"

enum { SHEET_W = 512, SHEET_H = 1024, CELL_W = 80, CELL_H = 84, STEP_W = 81, STEP_H = 85, COLS = 6, GLYPHS = 39 };
enum { CELL_A = 8, CELL_B = 9, CELL_X = 10, CELL_Y = 11 };       /* CMAP indices of U+E0E0..U+E0E3 */

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

int glyph_icons_apply(uint8_t *bc4, int style) {
    if (style != GLYPH_ICONS_XBOX) return 0;
    sheet_decode(bc4);
    swap_cells(CELL_A, CELL_B);
    swap_cells(CELL_X, CELL_Y);
    sheet_encode_changed(bc4);
    return 1;
}
