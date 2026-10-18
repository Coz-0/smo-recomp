/* smo_ui.c - the game's layout textures, fonts and messages, and a small software renderer (see smo_ui.h).
 *
 * The formats, as read here (all little-endian except the Yaz0 header):
 *  Yaz0  "Yaz0", u32 big-endian decompressed size, 8 bytes; then groups of a code byte (bit set = one literal byte,
 *        clear = a back reference of 2 bytes NR RR: distance RRR + 1, length N + 2, or N = 0 and a third byte + 0x12).
 *  SARC  header of 0x14 bytes (file data offset at 0xc); SFAT: node count at +6, nodes of 16 bytes from +0xc (name hash,
 *        attributes: bit 24 = named, low 16 bits = name offset / 4 inside SFNT's names; data start, data end); SFNT:
 *        8-byte header, then the names.
 *  BNTX  "NX  " at 0x20, texture count at 0x24, offset of a table of BRTI offsets at 0x28. BRTI: tiling @0x12, mip
 *        count @0x16, format @0x1c (type << 8 | kind: 1 unorm, 2 snorm, 6 sRGB), width @0x24, height @0x28, layout
 *        word @0x34 (block height log2 in bits 0..2), channel sources @0x58 (bytes R G B A: 0 zero, 1 one, 2 R, 3 G,
 *        4 B, 5 A), name @0x60 (u16 length + characters), offset of the mip data offsets @0x70. These are the fields
 *        tests/bntx_layout_check.py compares with tex_layout for every texture of the RomFS.
 *  BFFNT version 4.1. FINF @0x14: height +9, width +10, ascent +11, line feed +12, offsets of the TGLP / CWDH / CMAP data
 *        +20 / +24 / +28 (each points 8 bytes past its section's magic). TGLP: cell width +8, cell height +9, sheet
 *        count +10, sheet size +12, baseline +16, sheet format +18 (5 LA8, 8 A8, 12 BC4), cells per row +20, rows +22,
 *        sheet width +24, height +26, data offset +28. In this game's fonts the sheet data is a BNTX of its own whose
 *        one texture is an array with a layer per sheet (otherwise: block-linear surfaces `sheet size` bytes apart),
 *        each stored with its bottom row first; glyph cells are cell size + 1 apart from the top left, in rows of
 *        `cells per row`. CWDH: first and last glyph index, next block,
 *        then (left, glyph width, advance) per glyph. CMAP: first and last code, method (0 direct, 1 table, 2 scan:
 *        u16 count, then u32 code + u16 index per 8 bytes), next block.
 *  MSBT  "MsgStdBn", section count at 0xe, sections from 0x20 (magic, size, 8 bytes, data, padded to 16): LBL1 (bucket
 *        count, buckets of count + offset, labels of u8 length + name + u32 text index), TXT2 (count, offsets, UTF-16).
 * Nothing here is game data: every byte comes from the files at run time. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "smo_ui.h"
#include "tex_decode.h"

const char *fs_rom_root(void);                           /* nnfs.c */

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64le(const uint8_t *p) { return (uint64_t)u32le(p) | (uint64_t)u32le(p + 4) << 32; }

void ui_image_free(UiImage *im) { if (im) { free(im->px); memset(im, 0, sizeof *im); } }

/* ================================================================ files */
uint8_t *ui_file(const char *path, size_t *size) {
    char full[MAX_PATH];
    snprintf(full, sizeof full, "%s\\%s", fs_rom_root(), path);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b && size) *size = (size_t)n;
    return b;
}

static uint8_t *yaz0(const uint8_t *s, size_t n, size_t *out) {
    if (n < 16 || memcmp(s, "Yaz0", 4)) return NULL;
    size_t size = (size_t)s[4] << 24 | (size_t)s[5] << 16 | (size_t)s[6] << 8 | s[7], si = 16, di = 0;
    uint8_t *d = malloc(size ? size : 1);
    if (!d) return NULL;
    while (di < size && si < n) {
        uint8_t code = s[si++];
        for (int bit = 0; bit < 8 && di < size; bit++, code <<= 1) {
            if (code & 0x80) { if (si >= n) goto bad; d[di++] = s[si++]; continue; }
            if (si + 1 >= n) goto bad;
            size_t b1 = s[si++], b2 = s[si++], dist = ((b1 & 0xf) << 8 | b2) + 1, len = b1 >> 4;
            if (!len) { if (si >= n) goto bad; len = (size_t)s[si++] + 0x12; } else len += 2;
            if (dist > di) goto bad;
            for (; len && di < size; len--, di++) d[di] = d[di - dist];
        }
    }
    if (di != size) goto bad;
    *out = size;
    return d;
bad:
    free(d);
    return NULL;
}

uint8_t *ui_arc(const char *path, size_t *size) {
    size_t n = 0, m = 0;
    uint8_t *raw = ui_file(path, &n), *d;
    if (!raw) return NULL;
    d = yaz0(raw, n, &m);
    free(raw);
    if (d && size) *size = m;
    return d;
}

const uint8_t *ui_sarc(const uint8_t *s, size_t n, const char *name, size_t *size) {
    if (!s || n < 0x20 || memcmp(s, "SARC", 4) || u16le(s + 6) != 0xfeff) return NULL;
    size_t data = u32le(s + 0xc), sfat = u16le(s + 4);
    if (sfat + 0xc > n || memcmp(s + sfat, "SFAT", 4)) return NULL;
    int nodes = u16le(s + sfat + 6);
    size_t sfnt = sfat + 0xc + (size_t)nodes * 16;
    if (sfnt + 8 > n || memcmp(s + sfnt, "SFNT", 4)) return NULL;
    size_t names = sfnt + u16le(s + sfnt + 4);
    for (int i = 0; i < nodes; i++) {
        const uint8_t *nd = s + sfat + 0xc + (size_t)i * 16;
        uint32_t attr = u32le(nd + 4), b = u32le(nd + 8), e = u32le(nd + 12);
        if (!(attr & 0x01000000)) continue;
        size_t at = names + (size_t)(attr & 0xffff) * 4;
        if (at >= n || strncmp((const char *)s + at, name, n - at) || data + e > n || e < b) continue;
        if (size) *size = e - b;
        return s + data + b;
    }
    return NULL;
}

/* ================================================================ textures */
static int nvn_of_bntx(uint32_t fmt) {                    /* BNTX surface format -> NVN format (tex_decode.h) */
    int type = (int)(fmt >> 8), kind = (int)(fmt & 0xff), srgb = kind == 6, snorm = kind == 2;
    switch (type) {
    case 0x02: return TEX_FMT_R8;
    case 0x07: return TEX_FMT_RGB565;
    case 0x09: return TEX_FMT_RG8;
    case 0x0b: return srgb ? TEX_FMT_RGBA8_SRGB : TEX_FMT_RGBA8;
    case 0x0c: return srgb ? TEX_FMT_BGRA8_SRGB : TEX_FMT_BGRA8;
    case 0x1a: return srgb ? TEX_FMT_RGBA_DXT1_SRGB : TEX_FMT_RGBA_DXT1;
    case 0x1b: return srgb ? TEX_FMT_RGBA_DXT3_SRGB : TEX_FMT_RGBA_DXT3;
    case 0x1c: return srgb ? TEX_FMT_RGBA_DXT5_SRGB : TEX_FMT_RGBA_DXT5;
    case 0x1d: return snorm ? TEX_FMT_RGTC1_SNORM : TEX_FMT_RGTC1_UNORM;
    case 0x1e: return snorm ? TEX_FMT_RGTC2_SNORM : TEX_FMT_RGTC2_UNORM;
    case 0x20: return srgb ? TEX_FMT_BPTC_UNORM_SRGB : TEX_FMT_BPTC_UNORM;
    default:
        if (type >= 0x2d && type < 0x2d + 14) return (srgb ? TEX_FMT_ASTC_4x4_SRGB : TEX_FMT_ASTC_4x4) + (type - 0x2d);
        return -1;
    }
}

/* level 0 of a block-linear surface, decoded to RGBA8 (malloc'd); NULL if the data is short or the format unknown */
static uint8_t *decode_surface(int nvnfmt, const uint8_t *src, size_t avail, int w, int h, int mips, int bh_log2) {
    TexLayout L;
    const TexFormat *f = tex_format(nvnfmt);
    if (!f || tex_layout(&L, TEX_TARGET_2D, nvnfmt, w, h, 1, mips < 1 ? 1 : mips, 0, 0, bh_log2) < 0 || L.lvl_size[0] > avail) return NULL;
    uint8_t *lin = malloc(L.lvl_linear_size[0]), *rgba = malloc((size_t)w * h * 4);
    if (!lin || !rgba) { free(lin); free(rgba); return NULL; }
    tex_deswizzle_level(&L, 0, src, lin);
    int r = f->codec ? tex_decode_rgba8(nvnfmt, lin, w, h, rgba) : tex_convert_rgba8(nvnfmt, lin, w, h, rgba);
    free(lin);
    if (r < 0) { free(rgba); return NULL; }
    return rgba;
}

typedef struct { int fmt, w, h, mips, layers, bh; uint64_t data; uint8_t map[4]; } Brti;
/* The BRTI record named `name` (NULL: the first one) of a BNTX. 1 = found and usable. */
static int bntx_find(const uint8_t *b, size_t n, const char *name, Brti *o) {
    if (!b || n < 0x40 || memcmp(b, "BNTX", 4) || memcmp(b + 0x20, "NX  ", 4)) return 0;
    uint32_t count = u32le(b + 0x24);
    uint64_t table = u64le(b + 0x28);
    if (table + (uint64_t)count * 8 > n) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t t = u64le(b + table + (uint64_t)i * 8);
        if (t + 0x78 > n || memcmp(b + t, "BRTI", 4)) continue;
        if (name) {
            uint64_t name_at = u64le(b + t + 0x60);
            if (name_at + 2 > n) continue;
            size_t len = u16le(b + name_at);
            if (name_at + 2 + len > n || len != strlen(name) || memcmp(b + name_at + 2, name, len)) continue;
        }
        uint64_t ptrs = u64le(b + t + 0x70);
        o->fmt = nvn_of_bntx(u32le(b + t + 0x1c));
        o->mips = u16le(b + t + 0x16); o->w = (int)u32le(b + t + 0x24); o->h = (int)u32le(b + t + 0x28);
        o->layers = (int)u32le(b + t + 0x30); o->bh = (int)(u32le(b + t + 0x34) & 7);
        memcpy(o->map, b + t + 0x58, 4);
        if (o->layers < 1) o->layers = 1;
        if (u16le(b + t + 0x12) != 0 || o->fmt < 0 || o->w <= 0 || o->h <= 0 || o->w > 8192 || o->h > 8192 || ptrs + 8 > n) return 0;
        o->data = u64le(b + ptrs);
        return o->data < n;
    }
    return 0;
}
/* one layer of a BRTI, level 0, decoded to RGBA8 (before the channel mapping) */
static uint8_t *bntx_layer(const uint8_t *b, size_t n, const Brti *t, int layer) {
    TexLayout L;
    if (tex_layout(&L, t->layers > 1 ? TEX_TARGET_2D_ARRAY : TEX_TARGET_2D, t->fmt, t->w, t->h, t->layers, t->mips < 1 ? 1 : t->mips, 0, 0, t->bh) < 0) return NULL;
    uint64_t at = t->data + (uint64_t)layer * L.layer_stride;
    if (layer >= t->layers || at >= n) return NULL;
    return decode_surface(t->fmt, b + at, n - at, t->w, t->h, t->mips, t->bh);
}

int ui_bntx(const uint8_t *b, size_t n, const char *name, UiImage *out) {
    Brti t;
    memset(out, 0, sizeof *out);
    if (!bntx_find(b, n, name, &t)) return 0;
    uint8_t *rgba = bntx_layer(b, n, &t, 0);
    if (!rgba) return 0;
    for (size_t p = 0; p < (size_t)t.w * t.h; p++) {        /* the texture's channel sources, then alpha premultiplied */
        uint8_t *q = rgba + p * 4, v[4];
        for (int k = 0; k < 4; k++) v[k] = t.map[k] == 0 ? 0 : t.map[k] == 1 ? 255 : t.map[k] <= 5 ? q[t.map[k] - 2] : q[k];
        for (int k = 0; k < 3; k++) q[k] = (uint8_t)((v[k] * v[3] + 127) / 255);
        q[3] = v[3];
    }
    out->w = t.w; out->h = t.h; out->ch = 4; out->px = rgba;
    return 1;
}

/* ================================================================ fonts */
typedef struct { uint32_t code; uint16_t index; } CodeMap;
typedef struct { uint16_t first, last; const int8_t *w; } WidthBlock;   /* w: 3 bytes per glyph, into the copy */
struct UiFont {
    uint8_t *file;                                         /* a copy of the BFFNT (CWDH data points into it) */
    UiImage *sheets;
    int nsheets, cell_w, cell_h, baseline, cols, rows, height, ascent, linefeed;
    int8_t def_w[3];
    CodeMap *map; int nmap;
    WidthBlock *wb; int nwb;
};
static int map_cmp(const void *a, const void *b) { uint32_t x = ((const CodeMap *)a)->code, y = ((const CodeMap *)b)->code; return x < y ? -1 : x > y; }

UiFont *ui_font(const uint8_t *src, size_t n) {
    if (!src || n < 0x60 || memcmp(src, "FFNT", 4) || u16le(src + 4) != 0xfeff) return NULL;
    UiFont *f = calloc(1, sizeof *f);
    uint8_t *b = f ? malloc(n) : NULL;
    if (!b) { free(f); return NULL; }
    memcpy(b, src, n);
    f->file = b;
    const uint8_t *finf = b + u16le(b + 6);
    if (finf + 0x20 > b + n || memcmp(finf, "FINF", 4)) goto bad;
    f->height = finf[9]; f->ascent = finf[11]; f->linefeed = u16le(finf + 12);
    memcpy(f->def_w, finf + 16, 3);
    size_t tg = u32le(finf + 20) - 8, cw = u32le(finf + 24) - 8, cm = u32le(finf + 28) - 8;
    if (tg + 0x20 > n || memcmp(b + tg, "TGLP", 4)) goto bad;
    const uint8_t *t = b + tg;
    f->cell_w = t[8]; f->cell_h = t[9]; f->nsheets = t[10];
    size_t sheet_size = u32le(t + 12), data = u32le(t + 28);
    int fmt = u16le(t + 18) & 0x3fff, sw = u16le(t + 24), sh = u16le(t + 26);
    f->baseline = u16le(t + 16); f->cols = u16le(t + 20); f->rows = u16le(t + 22);
    if (f->nsheets <= 0 || f->cols <= 0 || f->rows <= 0 || data + sheet_size * (size_t)f->nsheets > n) goto bad;
    f->sheets = calloc((size_t)f->nsheets, sizeof *f->sheets);
    if (!f->sheets) goto bad;
    Brti arr;                                              /* the sheets: one texture array in a BNTX of their own */
    int in_bntx = bntx_find(b + data, n - data, NULL, &arr) && arr.w == sw && arr.h == sh && arr.layers >= f->nsheets;
    for (int s = 0; s < f->nsheets; s++) {
        int nvnfmt = fmt == 12 ? TEX_FMT_RGTC1_UNORM : fmt == 8 ? TEX_FMT_R8 : fmt == 5 ? TEX_FMT_RG8 : -1;
        uint8_t *rgba = in_bntx ? bntx_layer(b + data, n - data, &arr, s)
                      : nvnfmt < 0 ? NULL : decode_surface(nvnfmt, b + data + sheet_size * (size_t)s, sheet_size, sw, sh, 1, -1);
        if (!rgba) { printf("ui: font sheet format %d not supported or short\n", fmt); goto bad; }
        UiImage *im = &f->sheets[s];
        im->w = sw; im->h = sh; im->ch = 1; im->px = malloc((size_t)sw * sh);
        if (!im->px) { free(rgba); goto bad; }
        for (int y = 0; y < sh; y++)                       /* the sheets are stored bottom row first: turned upright here */
            for (int x = 0; x < sw; x++) im->px[(size_t)y * sw + x] = rgba[((size_t)(sh - 1 - y) * sw + x) * 4 + (fmt == 5 ? 1 : 0)];   /* BC4 / A8: R; LA8: the second byte */
        free(rgba);
    }
    for (size_t p = cw; p && p + 8 <= n && f->nwb < 64; ) {   /* CWDH blocks */
        if (memcmp(b + p, "CWDH", 4)) break;
        WidthBlock *nw = realloc(f->wb, sizeof *f->wb * (size_t)(f->nwb + 1));
        if (!nw) break;
        f->wb = nw;
        f->wb[f->nwb].first = u16le(b + p + 8); f->wb[f->nwb].last = u16le(b + p + 10); f->wb[f->nwb].w = (const int8_t *)(b + p + 16);
        if (p + 16 + 3u * (size_t)(f->wb[f->nwb].last - f->wb[f->nwb].first + 1) > n) break;
        f->nwb++;
        size_t next = u32le(b + p + 12);
        p = next ? next - 8 : 0;
    }
    for (size_t p = cm; p && p + 24 <= n; ) {               /* CMAP blocks */
        if (memcmp(b + p, "CMAP", 4)) break;
        uint32_t first = u32le(b + p + 8), last = u32le(b + p + 12);
        int method = u16le(b + p + 16);
        const uint8_t *d = b + p + 24;
        size_t add = method == 2 ? u16le(d) : last >= first ? (size_t)(last - first + 1) : 0, k = (size_t)f->nmap;
        CodeMap *nm = realloc(f->map, sizeof *f->map * (k + add + 1));
        if (!nm) break;
        f->map = nm;
        for (size_t i = 0; i < add; i++) {
            uint32_t code; uint16_t idx;
            if (method == 0) { code = first + (uint32_t)i; idx = (uint16_t)(u16le(d) + i); }
            else if (method == 1) { code = first + (uint32_t)i; idx = u16le(d + 2 * i); }
            else { code = u32le(d + 4 + 8 * i); idx = u16le(d + 8 + 8 * i); }
            if (idx != 0xffff) { f->map[k].code = code; f->map[k].index = idx; k++; }
        }
        f->nmap = (int)k;
        size_t next = u32le(b + p + 20);
        p = next ? next - 8 : 0;
    }
    qsort(f->map, (size_t)f->nmap, sizeof *f->map, map_cmp);
    return f;
bad:
    ui_font_free(f);
    return NULL;
}
void ui_font_free(UiFont *f) {
    if (!f) return;
    for (int s = 0; f->sheets && s < f->nsheets; s++) ui_image_free(&f->sheets[s]);
    free(f->sheets); free(f->map); free(f->wb); free(f->file); free(f);
}
void ui_font_info(const UiFont *f, UiFontInfo *o) {
    o->height = f->height; o->ascent = f->ascent; o->linefeed = f->linefeed;
    o->cell_w = f->cell_w; o->cell_h = f->cell_h; o->baseline = f->baseline;
}
static int glyph_of(const UiFont *f, uint32_t code) {
    int lo = 0, hi = f->nmap - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->map[mid].code == code) return f->map[mid].index;
        if (f->map[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static void widths_of(const UiFont *f, int g, int8_t out[3]) {
    for (int i = 0; i < f->nwb; i++)
        if (g >= f->wb[i].first && g <= f->wb[i].last) { memcpy(out, f->wb[i].w + 3 * (g - f->wb[i].first), 3); return; }
    memcpy(out, f->def_w, 3);
}

/* ================================================================ messages */
struct UiMsbt { int n; char **labels; uint16_t **texts; int *index; int ntexts; };
UiMsbt *ui_msbt(const uint8_t *b, size_t n) {
    if (!b || n < 0x20 || memcmp(b, "MsgStdBn", 8) || b[8] != 0xff || b[9] != 0xfe) return NULL;   /* little-endian only */
    UiMsbt *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    int sections = u16le(b + 0xe);
    size_t p = 0x20;
    for (int s = 0; s < sections && p + 16 <= n; s++) {
        size_t size = u32le(b + p + 4);
        const uint8_t *d = b + p + 16;
        if (p + 16 + size > n) break;
        if (!memcmp(b + p, "LBL1", 4) && size >= 4) {
            uint32_t buckets = u32le(d);
            for (uint32_t k = 0; k < buckets && 4 + 8 * (size_t)k + 8 <= size; k++) {
                uint32_t cnt = u32le(d + 4 + 8 * k), at = u32le(d + 8 + 8 * k);
                for (uint32_t j = 0; j < cnt && at < size; j++) {
                    size_t len = d[at];
                    if (at + 1 + len + 4 > size) break;
                    char **nl = realloc(m->labels, sizeof *m->labels * (size_t)(m->n + 1));
                    int *ni = realloc(m->index, sizeof *m->index * (size_t)(m->n + 1));
                    if (nl) m->labels = nl;
                    if (ni) m->index = ni;
                    if (!nl || !ni) break;
                    m->labels[m->n] = malloc(len + 1);
                    if (!m->labels[m->n]) break;
                    memcpy(m->labels[m->n], d + at + 1, len); m->labels[m->n][len] = 0;
                    m->index[m->n] = (int)u32le(d + at + 1 + len);
                    m->n++;
                    at += (uint32_t)(1 + len + 4);
                }
            }
        } else if (!memcmp(b + p, "TXT2", 4) && size >= 4) {
            uint32_t cnt = u32le(d);
            if (4 + 4 * (size_t)cnt > size) break;
            m->texts = calloc(cnt ? cnt : 1, sizeof *m->texts);
            if (!m->texts) break;
            m->ntexts = (int)cnt;
            for (uint32_t k = 0; k < cnt; k++) {
                size_t a = u32le(d + 4 + 4 * k), e = k + 1 < cnt ? u32le(d + 8 + 4 * k) : size;
                if (a > size || e > size || e < a) e = a;
                size_t chars = (e - a) / 2;
                m->texts[k] = calloc(chars + 1, 2);
                if (!m->texts[k]) continue;
                for (size_t i = 0; i < chars; i++) m->texts[k][i] = u16le(d + a + 2 * i);
            }
        }
        p += 16 + ((size + 15) & ~(size_t)15);
    }
    for (int i = 1; i < m->n; i++)                         /* the labels in the order of their texts (LBL1 keeps them by hash) */
        for (int k = i; k > 0 && m->index[k - 1] > m->index[k]; k--) {
            char *l = m->labels[k]; m->labels[k] = m->labels[k - 1]; m->labels[k - 1] = l;
            int x = m->index[k]; m->index[k] = m->index[k - 1]; m->index[k - 1] = x;
        }
    return m;
}
void ui_msbt_free(UiMsbt *m) {
    if (!m) return;
    for (int i = 0; i < m->n; i++) free(m->labels[i]);
    for (int i = 0; i < m->ntexts; i++) free(m->texts[i]);
    free(m->labels); free(m->index); free(m->texts); free(m);
}
int ui_msbt_count(const UiMsbt *m) { return m ? m->n : 0; }
const char *ui_msbt_label(const UiMsbt *m, int i) { return m && i >= 0 && i < m->n ? m->labels[i] : NULL; }
const uint16_t *ui_msbt_text_at(const UiMsbt *m, int i) {
    if (!m || i < 0 || i >= m->n || m->index[i] < 0 || m->index[i] >= m->ntexts) return NULL;
    return m->texts[m->index[i]];
}
const uint16_t *ui_msbt_text(const UiMsbt *m, const char *label) {
    for (int i = 0; m && i < m->n; i++) if (!strcmp(m->labels[i], label)) return ui_msbt_text_at(m, i);
    return NULL;
}

/* ================================================================ drawing */
UiXf ui_xf_screen(int w, int h) { UiXf x = {(float)w / 1280.0f, 0, (float)w * 0.5f, 0, -(float)h / 720.0f, (float)h * 0.5f}; return x; }
UiXf ui_xf_mul(UiXf o, UiXf i) {
    UiXf r = {o.a * i.a + o.b * i.d, o.a * i.b + o.b * i.e, o.a * i.c + o.b * i.f + o.c,
              o.d * i.a + o.e * i.d, o.d * i.b + o.e * i.e, o.d * i.c + o.e * i.f + o.f};
    return r;
}
UiXf ui_xf_move(float x, float y) { UiXf r = {1, 0, x, 0, 1, y}; return r; }
UiXf ui_xf_rotate(float deg) { float s = sinf(deg * 3.14159265f / 180.0f), c = cosf(deg * 3.14159265f / 180.0f); UiXf r = {c, -s, 0, s, c, 0}; return r; }
UiXf ui_xf_scale(float sx, float sy) { UiXf r = {sx, 0, 0, 0, sy, 0}; return r; }

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void ui_draw(UiImage *dst, UiXf xf, float x0, float y0, float x1, float y1, const UiImage *src, float u0, float v0, float u1, float v1, uint32_t rgba) {
    ui_draw_edges(dst, xf, x0, y0, x1, y1, src, u0, v0, u1, v1, rgba, UI_EDGE_ALL);
}
void ui_draw_edges(UiImage *dst, UiXf xf, float x0, float y0, float x1, float y1, const UiImage *src, float u0, float v0, float u1, float v1, uint32_t rgba, int edges) {
    if (!dst || !dst->px || x1 <= x0 || y1 <= y0) return;
    float det = xf.a * xf.e - xf.b * xf.d;
    if (fabsf(det) < 1e-12f) return;
    float ia = xf.e / det, ib = -xf.b / det, id = -xf.d / det, ie = xf.a / det;
    float kx = sqrtf(xf.a * xf.a + xf.d * xf.d), ky = sqrtf(xf.b * xf.b + xf.e * xf.e);     /* pixels per unit along the rectangle's axes */
    float cx[4] = {x0, x1, x1, x0}, cy[4] = {y0, y0, y1, y1}, minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int k = 0; k < 4; k++) {
        float X = xf.a * cx[k] + xf.b * cy[k] + xf.c, Y = xf.d * cx[k] + xf.e * cy[k] + xf.f;
        if (X < minx) minx = X; if (X > maxx) maxx = X; if (Y < miny) miny = Y; if (Y > maxy) maxy = Y;
    }
    int bx0 = (int)floorf(minx) - 1, bx1 = (int)ceilf(maxx) + 1, by0 = (int)floorf(miny) - 1, by1 = (int)ceilf(maxy) + 1;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > dst->w) bx1 = dst->w; if (by1 > dst->h) by1 = dst->h;
    float cr = (float)(rgba >> 24) / 255.0f, cg = (float)((rgba >> 16) & 255) / 255.0f, cb = (float)((rgba >> 8) & 255) / 255.0f, ca = (float)(rgba & 255) / 255.0f;
    float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    for (int Y = by0; Y < by1; Y++) {
        uint8_t *row = dst->px + (size_t)Y * dst->w * 4;
        for (int X = bx0; X < bx1; X++) {
            float px = (float)X + 0.5f - xf.c, py = (float)Y + 0.5f - xf.f;
            float lx = ia * px + ib * py, ly = id * px + ie * py;
            /* coverage: smooth over a pixel at the edges in `edges`, a hard half-open step at the others (pieces of one shape
             * that share an edge then cover each pixel once, without a seam) */
            float el = edges & UI_EDGE_LEFT ? clamp01((lx - x0) * kx + 0.5f) : (float)(lx >= x0), er = edges & UI_EDGE_RIGHT ? clamp01((x1 - lx) * kx + 0.5f) : (float)(lx < x1);
            float eb = edges & UI_EDGE_BOTTOM ? clamp01((ly - y0) * ky + 0.5f) : (float)(ly >= y0), et = edges & UI_EDGE_TOP ? clamp01((y1 - ly) * ky + 0.5f) : (float)(ly < y1);
            float cov = (el < er ? el : er) * (eb < et ? eb : et);
            if (cov <= 0.0f) continue;
            float sr = 1, sg = 1, sb = 1, sa = 1;
            if (src) {
                float tu = u0 + (lx - x0) * du - 0.5f, tv = v0 + (y1 - ly) * dv - 0.5f;
                int ix = (int)floorf(tu), iy = (int)floorf(tv);
                float fx = tu - (float)ix, fy = tv - (float)iy, acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++) {
                    int sx = ix + (k & 1), sy = iy + (k >> 1);
                    float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                    if (sx < 0) sx = 0; if (sy < 0) sy = 0; if (sx >= src->w) sx = src->w - 1; if (sy >= src->h) sy = src->h - 1;
                    if (src->ch == 1) acc[3] += wgt * src->px[(size_t)sy * src->w + sx];
                    else { const uint8_t *q = src->px + ((size_t)sy * src->w + sx) * 4; for (int c = 0; c < 4; c++) acc[c] += wgt * q[c]; }
                }
                if (src->ch == 1) { sa = acc[3] / 255.0f; sr = sg = sb = sa; }
                else { sr = acc[0] / 255.0f; sg = acc[1] / 255.0f; sb = acc[2] / 255.0f; sa = acc[3] / 255.0f; }
            }
            float a = sa * ca * cov;                          /* premultiplied source times the colour */
            if (a <= 0.0f) continue;
            float r = sr * cr * ca * cov, g = sg * cg * ca * cov, bl = sb * cb * ca * cov, keep = 1.0f - a;
            uint8_t *q = row + (size_t)X * 4;
            q[0] = (uint8_t)(clamp01(r + (float)q[0] / 255.0f * keep) * 255.0f + 0.5f);
            q[1] = (uint8_t)(clamp01(g + (float)q[1] / 255.0f * keep) * 255.0f + 0.5f);
            q[2] = (uint8_t)(clamp01(bl + (float)q[2] / 255.0f * keep) * 255.0f + 0.5f);
            q[3] = (uint8_t)(clamp01(a + (float)q[3] / 255.0f * keep) * 255.0f + 0.5f);
        }
    }
}

/* The picture of a device icon tag (group 6) in the console's private-use font area: the type numbers are the ones the
 * game's footers use (Footer.msbt: 2 with A, 3 with B, 5 with Y, 16 with the left stick, 17 with the right stick). */
static uint32_t icon_code(int group, int type) {
    if (group != 6) return 0;
    switch (type) {
    case 2: return 0xe0e0;   /* A */
    case 3: return 0xe0e1;   /* B */
    case 4: return 0xe0e2;   /* X */
    case 5: return 0xe0e3;   /* Y */
    case 16: return 0xe101;  /* left stick */
    case 17: return 0xe102;  /* right stick */
    default: return 0;
    }
}

static float glyph(UiImage *dst, UiXf xf, const UiFont *f, int g, float x, float y, float scale, uint32_t rgba) {
    int8_t w[3];
    widths_of(f, g, w);
    int per = f->cols * f->rows, sheet = g / per, cell = g % per, col = cell % f->cols, row = cell / f->cols;
    if (dst && sheet < f->nsheets && (uint8_t)w[1]) {
        float u = (float)(col * (f->cell_w + 1)), v = (float)(row * (f->cell_h + 1));
        float gx = x + (float)w[0] * scale, top = y + (float)f->baseline * scale;
        ui_draw(dst, xf, gx, top - (float)f->cell_h * scale, gx + (float)(uint8_t)w[1] * scale, top, &f->sheets[sheet], u, v, u + (float)(uint8_t)w[1], v + (float)f->cell_h, rgba);
    }
    return (float)(uint8_t)w[2] * scale;
}

float ui_text(UiImage *dst, UiXf xf, const UiFont *f, const UiFont *icons, const uint16_t *s, int len, float x, float y, float scale, uint32_t rgba) {
    float pen = x;
    if (!f || !s) return 0;
    for (int i = 0; len < 0 ? s[i] != 0 : i < len; i++) {
        uint16_t ch = s[i];
        if (ch == 0x0e) {                                  /* tag: group, type, payload size in bytes, payload */
            int group = s[i + 1], type = s[i + 2], size = s[i + 3];
            uint32_t code = icon_code(group, type);
            i += 3 + size / 2;
            int gi = icons && code ? glyph_of(icons, code) : -1;
            if (gi >= 0) {                                 /* the icon's cell as high as the text's line, centred on its capitals */
                float k = scale * (float)f->height / (float)icons->cell_h;
                float base = y + 0.33f * ((float)f->ascent * scale - (float)icons->ascent * k);
                pen += glyph(dst, xf, icons, gi, pen, base, k, rgba);
            }
            continue;
        }
        if (ch == 0x0f) { i += 2; continue; }             /* end of a tag: group, type */
        if (ch == '\n' || ch == '\r') break;
        int gi = glyph_of(f, ch);
        if (gi < 0) gi = glyph_of(f, '?');
        if (gi >= 0) pen += glyph(dst, xf, f, gi, pen, y, scale, rgba);
    }
    return pen - x;
}
