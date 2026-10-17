/* smo_ui.h - SMO port: menus the runtime draws itself, in the look of the game's own menus.
 *
 * Everything shown is the game's: its layout textures (BNTX in the layout archives), its fonts (BFFNT), its message
 * files (MSBT), read at run time from the RomFS the runtime mounts (nnfs.c; extracted from the user's own dump, never
 * stored by this project). The drawing is done in host memory: affine textured rectangles with bilinear sampling and
 * anti-aliased edges, blended over a frame; smo_amiibo.c composes its menu with it and nvn_vk_draw.c shows the frame.
 *
 * Coordinates: the game's layouts place their panes in a 1280 x 720 space with the origin at the centre and y up;
 * ui_xf_screen maps that space onto a frame of any size. */
#ifndef SMO_UI_H
#define SMO_UI_H
#include <stddef.h>
#include <stdint.h>

/* An image in host memory, rows from the top. ch = 4: RGBA8, alpha premultiplied; ch = 1: coverage (font sheets). */
typedef struct { int w, h, ch; uint8_t *px; } UiImage;
void ui_image_free(UiImage *im);

/* ---- the game's files (NULL when missing; the results are malloc'd unless said otherwise) */
uint8_t *ui_file(const char *path, size_t *size);                                    /* a file of the RomFS, '/' separated */
uint8_t *ui_arc(const char *path, size_t *size);                                     /* a Yaz0-compressed archive of the RomFS, decompressed */
const uint8_t *ui_sarc(const uint8_t *sarc, size_t n, const char *name, size_t *size);   /* a file inside a SARC: points into it */

/* ---- textures: level 0 of the BNTX texture `name`, decoded, with the texture's channel mapping. 1 = done */
int ui_bntx(const uint8_t *bntx, size_t n, const char *name, UiImage *out);

/* ---- fonts (BFFNT, glyph sheets in BC4 / A8 / LA8) */
typedef struct UiFont UiFont;
UiFont *ui_font(const uint8_t *bffnt, size_t n);
void ui_font_free(UiFont *f);
typedef struct { int height, ascent, linefeed, cell_w, cell_h, baseline; } UiFontInfo;   /* FINF and TGLP, in font pixels */
void ui_font_info(const UiFont *f, UiFontInfo *out);

/* ---- messages (MSBT): labels in the order of their texts; texts keep their tags (0x0E group type size payload, 0x0F group type) */
typedef struct UiMsbt UiMsbt;
UiMsbt *ui_msbt(const uint8_t *msbt, size_t n);
void ui_msbt_free(UiMsbt *m);
int ui_msbt_count(const UiMsbt *m);
const char *ui_msbt_label(const UiMsbt *m, int i);
const uint16_t *ui_msbt_text_at(const UiMsbt *m, int i);
const uint16_t *ui_msbt_text(const UiMsbt *m, const char *label);                    /* NULL = no such label */

/* ---- drawing */
typedef struct { float a, b, c, d, e, f; } UiXf;           /* x' = a x + b y + c, y' = d x + e y + f */
UiXf ui_xf_screen(int w, int h);                           /* layout space -> pixels of a w x h frame */
UiXf ui_xf_mul(UiXf outer, UiXf inner);                    /* inner first */
UiXf ui_xf_move(float x, float y);
UiXf ui_xf_rotate(float degrees);                          /* counter-clockwise with y up, as a pane's z rotation */
UiXf ui_xf_scale(float sx, float sy);
/* The rectangle [x0,x1] x [y0,y1] (through xf) filled with the texels [u0,u1] x [v0,v1] of src (v down; u0 > u1 mirrors;
 * src NULL = plain), times rgba (0xRRGGBBAA, not premultiplied), blended over dst. */
void ui_draw(UiImage *dst, UiXf xf, float x0, float y0, float x1, float y1, const UiImage *src, float u0, float v0, float u1, float v1, uint32_t rgba);
/* the same with anti-aliasing only on the edges named (UI_EDGE_*): the inner edges of pieces of one shape */
enum { UI_EDGE_LEFT = 1, UI_EDGE_RIGHT = 2, UI_EDGE_BOTTOM = 4, UI_EDGE_TOP = 8, UI_EDGE_ALL = 15 };
void ui_draw_edges(UiImage *dst, UiXf xf, float x0, float y0, float x1, float y1, const UiImage *src, float u0, float v0, float u1, float v1, uint32_t rgba, int edges);
/* One line of text, pen at (x, y) on the baseline, `scale` layout units per font pixel; len < 0 = up to the 0. Device icon
 * tags (group 6: the button and stick pictures of the footers) are drawn from `icons`, their cells as high as the text's
 * line; other tags are skipped. Returns the advance. dst NULL = measure only. */
float ui_text(UiImage *dst, UiXf xf, const UiFont *f, const UiFont *icons, const uint16_t *s, int len, float x, float y, float scale, uint32_t rgba);

/* ---- showing a menu: what smo_amiibo.c asks the render back end (RBackend.menu, nvn_vk_draw.c) on the render thread */
enum { UI_MENU_READ, UI_MENU_SHOW, UI_MENU_END };
typedef struct {
    int op;                     /* READ: the frame just presented, read back; SHOW: rgba over the whole window; END: free what SHOW made */
    void *present;              /* the PresentJob of that frame (nvn_front.h) */
    const uint8_t *rgba;        /* SHOW: w x h RGBA8, in the colour encoding of the frame READ gave */
    int w, h, changed;          /* SHOW: changed = 0: the picture of the previous SHOW again */
    uint8_t *out;               /* READ: malloc'd w x h RGBA8 (alpha 255), freed by the caller */
    int out_w, out_h, ok;
} UiMenuJob;
#endif
