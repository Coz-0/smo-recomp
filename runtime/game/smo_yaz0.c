/* smo_yaz0.c - the game's Yaz0 decompression (.szs archives) in host code.
 *
 * Every .szs of the RomFS (4,666 files, 4.1 GB that become 6.7 GB) goes through one of sead's two Yaz0 decoders:
 *   - SZSDecompressor::streamDecomp(DecompContext *, const void *src, u32 len): a chunk at a time, the state kept in the
 *     context between calls. The decompression thread of al::ParallelSZSDecompressor (DecompThread) runs it on each
 *     chunk as it is read, and SZSDecompressor::tryDecompFromDevice on a file larger than its read buffer;
 *   - SZSDecompressor's whole-buffer decoder (hand-written assembly: dst in x0, the file in x1, the size back in w0),
 *     for a file read whole.
 * tools/lifter/game_patches.py finds both by their code and puts a call here at their first instruction; a nonzero
 * answer returns from the lifted function at once. The context, the answer and the output of each call are those of
 * the game's code, step for step: tests/check_yaz0.py runs both on every .szs of the RomFS, whole and in random chunks,
 * and compares them after every call. Where the output still has room, the bytes just past the write position may
 * hold some of what comes next before their turn (up to 7; the game writes them later, with the same values or new
 * ones). What differs is the speed: a whole group of eight items (a flag byte, then literals and back-references) is
 * done without the per-byte state machine when the chunk and the output left are sure to hold it, eight literals in a
 * row are one copy, and a back-reference is copied 8 bytes at a time. Measured by that test over the 4,666 files
 * (Ryzen 5 7535HS, clang 23 -O2 both): streamDecomp in 64 KB chunks 212 -> 1,475 MB of output a second, the whole-buffer
 * decoder 376 -> 1,499. The scripted Cap Kingdom run decodes 595 MB of compressed chunks and 601 MB of whole files.
 *
 * SMO_YAZ0=0 leaves the game's code alone. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "runtime.h"

/* sead::SZSDecompressor::DecompContext, as streamDecomp reads and writes it */
typedef struct {
    uint8_t *dst;          /* +0x00 where the next byte goes */
    int32_t remain;        /* +0x08 bytes still to write (the header's size, or forceDestCount when smaller) */
    int32_t force;         /* +0x0c forceDestCount: 0, or a size to stop at instead of failing */
    uint8_t mask;          /* +0x10 the flag bit of the next item (0: a flag byte comes first) */
    uint8_t flags;         /* +0x11 the group's flag byte */
    uint8_t high;          /* +0x12 the first byte of a back-reference */
    uint8_t pad0;
    int32_t step;          /* +0x14 1: a back-reference's second byte is next, 2: its length byte, else an item */
    uint16_t offset;       /* +0x18 a back-reference's distance */
    uint8_t header;        /* +0x1a header bytes still to read (16 at the start) */
} Yaz0Ctx;

/* dst[k] = dst[k - off] for k = 0 .. n-1, one byte after the other as the game does (a run when off < n): 8 bytes at a
 * time when off >= 8 (the 8 read are written already); nothing past dst + n */
static inline void lz_copy(uint8_t *dst, uint32_t off, uint32_t n) {
    const uint8_t *from = dst - off;
    if (off >= 8) {
        for (; n >= 8; n -= 8, dst += 8, from += 8) memcpy(dst, from, 8);
        for (uint32_t k = 0; k < n; k++) dst[k] = from[k];
        return;
    }
    if (off == 1) { memset(dst, dst[-1], n); return; }
    for (uint32_t k = 0; k < n; k++) dst[k] = from[k];
}

#define GROUP_IN 25        /* the most a group of eight items reads: the flag byte and eight 3-byte references */
#define GROUP_OUT (8 * 0x111)

/* One whole group: the flag byte, then eight literals or back-references, with no check of the input or the output
 * left (the caller's: GROUP_IN bytes in, GROUP_OUT + 16 out, as a short back-reference is one or two 8-byte copies that
 * write up to 7 bytes past it, written again by what follows). Moves *src and *dst on; *high, *off: the last back-reference's first byte and distance. Returns
 * the bytes written. */
static inline uint32_t group8(const uint8_t **src, uint8_t **dst, uint8_t *high, uint32_t *off) {
    const uint8_t *s = *src;
    uint8_t *d = *dst, f = *s++;
    if (f == 0xff) { memcpy(d, s, 8); d += 8; s += 8; }
    else {
        for (int k = 0; k < 8; k++, f <<= 1) {
            if (f & 0x80) { *d++ = *s++; continue; }
            uint8_t h = s[0];
            uint32_t o = ((uint32_t)(h & 0xf) << 8 | s[1]) + 1, n = h >> 4;
            if (n) { n += 2; s += 2; }
            else { n = s[2] + 0x12u; s += 3; }
            if (o >= 8 && n <= 8) memcpy(d, d - o, 8);          /* up to 5 bytes past the item, written again after */
            else if (o >= 16 && n <= 16) { memcpy(d, d - o, 8); memcpy(d + 8, d - o + 8, 8); }
            else lz_copy(d, o, n);
            d += n; *high = h; *off = o;
        }
    }
    uint32_t wrote = (uint32_t)(d - *dst);
    *src = s; *dst = d;
    return wrote;
}

/* streamDecomp(ctx, src, len) -> the bytes still to write, 0 at the end, -1 a bad header (or a chunk that ends inside
 * it), -2 a back-reference past the output without forceDestCount. Branch for branch the game's code. */
static int32_t yaz0_stream(Yaz0Ctx *x, const uint8_t *src, uint32_t len) {
    if (x->header) {
        uint32_t i = 0;
        for (;;) {
            if (!x->header) {                                        /* the header is read: forceDestCount applies */
                if (x->force > 0 && x->force < x->remain) x->remain = x->force;
                len -= i;
                if (!len) return x->remain;
                src += (int32_t)i;
                break;
            }
            uint8_t was = x->header--, b = src[i];
            if ((uint8_t)(was - 13) <= 3) {                           /* the 4 magic bytes */
                if (b != "0zaY"[was - 13]) return -1;
            } else if (x->header >= 8) {                              /* the 4 size bytes, big-endian */
                x->remain |= (int32_t)((uint32_t)b << ((x->header * 8u) & 31));
            }
            if (x->header && i == len - 1) return -1;                 /* the chunk ended inside the header */
            i++;
        }
    }
    int32_t remain = x->remain, step = x->step;
    uint8_t *dst = x->dst, mask = x->mask, flags = x->flags, high = x->high;
    uint32_t off = x->offset;
    while (remain >= 1) {
        if (step == 1) {
            off = ((uint32_t)(high & 0xf) << 8 | *src) + 1;
            uint32_t n = high >> 4;
            if (!n) step = 2;
            else {
                n += 2;
                if (n > (uint32_t)remain) { if (!x->force) return -2; n = (uint32_t)remain & 0xffff; }
                lz_copy(dst, off, n);
                dst += n; step = 0; remain -= (int32_t)n;
            }
        } else if (step == 2) {
            uint32_t n = *src + 0x12u;
            if (n > (uint32_t)remain) { if (!x->force) return -2; n = (uint32_t)remain & 0xffff; }
            lz_copy(dst, off & 0xffff, n);
            dst += n; step = 0; remain -= (int32_t)n;
        } else {
            if (!mask) {
                /* Whole groups while the chunk and the output hold one with room to spare: the game's code would
                 * not stop inside it, and ends it in the state it starts the next one (mask 0; the group's flags, the
                 * last back-reference's first byte and distance kept). */
                while (len > GROUP_IN && remain > GROUP_OUT + 16) {
                    const uint8_t *at = src;
                    flags = *src;
                    remain -= (int32_t)group8(&src, &dst, &high, &off);
                    len -= (uint32_t)(src - at);
                }
                flags = *src++;
                len--;
                mask = 0x80;
                if (!len) break;
            }
            uint8_t b = *src;
            if (flags & mask) { remain--; *dst++ = b; }
            else { step = 1; high = b; }
            mask = (mask >> 1) & 0x7f;
        }
        if (!--len) break;
        src++;
    }
    x->remain = remain; x->step = step; x->dst = dst; x->offset = (uint16_t)off;
    x->mask = mask; x->flags = flags; x->high = high;
    if (remain) return remain;
    if (len < 0x21 || x->force) return 0;
    return -1;
}

/* The whole-buffer decoder: the size from the header, then items until that many bytes are written. Returns the size. */
static uint32_t yaz0_whole(uint8_t *dst, const uint8_t *src) {
    uint32_t size = (uint32_t)src[4] << 24 | (uint32_t)src[5] << 16 | (uint32_t)src[6] << 8 | src[7], left = size;
    uint8_t mask = 0, flags = 0, high;
    uint32_t off;
    src += 16;
    while (left > GROUP_OUT + 16) left -= group8(&src, &dst, &high, &off);     /* whole groups while the output holds one */
    while (left) {
        if (!(mask >>= 1)) { flags = *src++; mask = 0x80; }
        if (flags & mask) { *dst++ = *src++; left--; continue; }
        uint32_t v = (uint32_t)src[0] << 8 | src[1], n = (v >> 12) + 2;
        src += 2;
        if (n == 2) n = *src++ + 0x12u;
        lz_copy(dst, (v & 0xfff) + 1, n);
        dst += n; left -= n;
    }
    return size;
}

/* The lifter's patches (tools/lifter/game_patches.py) at the first instruction of each decoder: 1, the call is done
 * (x0 holds the answer, the zero-extended w0 of the game's code); 0, the lifted function runs. Guest addresses are
 * host addresses. The game's code leaves only registers a callee may change behind, which its callers do not read. */
static int g_on = -1;
static volatile LONG64 g_stream_calls, g_stream_bytes, g_whole_calls, g_whole_bytes;

static int yaz0_on(void) {
    if (g_on < 0) {
        const char *e = getenv("SMO_YAZ0");
        g_on = !e || atoi(e) != 0;
        printf("smo_yaz0: %s\n", g_on ? "the game's Yaz0 decoders run in host code (SMO_YAZ0=0: the game's own code)"
                                      : "off (SMO_YAZ0=0): the game's Yaz0 decoders run as lifted");
    }
    return g_on;
}

int h_yaz0_stream(Ctx *c) {
    if (!yaz0_on()) return 0;
    InterlockedIncrement64(&g_stream_calls);
    InterlockedAdd64(&g_stream_bytes, (uint32_t)c->x[2]);
    c->x[0] = (uint32_t)yaz0_stream((Yaz0Ctx *)(uintptr_t)c->x[0], (const uint8_t *)(uintptr_t)c->x[1], (uint32_t)c->x[2]);
    return 1;
}

int h_yaz0_whole(Ctx *c) {
    if (!yaz0_on()) return 0;
    uint32_t n = yaz0_whole((uint8_t *)(uintptr_t)c->x[0], (const uint8_t *)(uintptr_t)c->x[1]);
    InterlockedIncrement64(&g_whole_calls);
    InterlockedAdd64(&g_whole_bytes, n);
    c->x[0] = n;
    return 1;
}

void smo_yaz0_report(FILE *out) {
    if (g_stream_calls || g_whole_calls)
        fprintf(out, "smo_yaz0: %lld chunks (%.1f MB compressed) through streamDecomp, %lld files (%.1f MB) whole, in host code\n",
                (long long)g_stream_calls, g_stream_bytes / 1e6, (long long)g_whole_calls, g_whole_bytes / 1e6);
}
