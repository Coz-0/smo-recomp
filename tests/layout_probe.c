/* layout_probe.dll: the runtime's texture layout (runtime/gfx/tex_decode.c, unchanged) and a Yaz0 decoder, behind a flat C
 * interface for tests/bntx_layout_check.py. Test code only; not part of the game build. */
#include <stdint.h>
#include <string.h>
#include "tex_decode.h"

typedef struct {
    int64_t size, data_size, alignment, layer_stride;
    int32_t bh_log2, bd_log2, levels, unknown_format;
    int64_t lvl_offset[TEX_MAX_LEVELS];
} ProbeOut;

__declspec(dllexport) int probe_layout(int target, int format, int w, int h, int d, int levels, int flags, ProbeOut *o) {
    TexLayout L;
    int r = tex_layout(&L, target, format, w, h, d, levels, flags, 0, -1);
    memset(o, 0, sizeof *o);
    o->size = (int64_t)L.size; o->data_size = (int64_t)L.data_size; o->alignment = (int64_t)L.alignment;
    o->layer_stride = (int64_t)L.layer_stride; o->bh_log2 = L.bh_log2; o->bd_log2 = L.bd_log2; o->levels = L.levels;
    o->unknown_format = r != 0;
    for (int i = 0; i < L.levels && i < TEX_MAX_LEVELS; i++) o->lvl_offset[i] = (int64_t)L.lvl_offset[i];
    return r;
}

/* Yaz0 -> dst. Returns the decompressed size, or -1 on a malformed stream. */
__declspec(dllexport) int64_t yaz0_decompress(const uint8_t *src, int64_t n, uint8_t *dst, int64_t cap) {
    if (n < 16 || memcmp(src, "Yaz0", 4)) return -1;
    int64_t size = ((int64_t)src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7];
    if (size > cap) return -1;
    int64_t s = 16, o = 0;
    while (o < size) {
        if (s >= n) return -1;
        uint8_t code = src[s++];
        for (int bit = 7; bit >= 0 && o < size; bit--) {
            if (code & (1 << bit)) { if (s >= n) return -1; dst[o++] = src[s++]; }
            else {
                if (s + 1 >= n) return -1;
                int b1 = src[s++], b2 = src[s++];
                int64_t dist = (((b1 & 0xf) << 8) | b2) + 1, len = b1 >> 4;
                if (len == 0) { if (s >= n) return -1; len = src[s++] + 0x12; } else len += 2;
                if (dist > o) return -1;
                for (int64_t k = 0; k < len && o < size; k++, o++) dst[o] = dst[o - dist];
            }
        }
    }
    return size;
}
