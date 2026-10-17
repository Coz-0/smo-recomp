/* The random cases of tests/check_fast_paths.py: the runtime's index_max and live-list cell index (copied into
 * fast_paths_extract.h by that script) against the plain code they replaced. Prints the counts; exit status 0 when
 * nothing differs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <emmintrin.h>
#include "fast_paths_extract.h"

static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* index_max as it was: one index at a time */
static uint32_t index_max_ref(uint64_t addr, int count, unsigned t) {
    const uint8_t *src = P(addr);
    uint32_t mx = 0;
    if (t == 0) { for (int k = 0; k < count; k++) { uint32_t v = src[k]; if (v > mx && !(R.restart && v == 0xFFu)) mx = v; } }
    else if (t == 1) { const uint16_t *p16 = (const uint16_t *)src; for (int k = 0; k < count; k++) { uint32_t v = p16[k]; if (v > mx && !(R.restart && v == 0xFFFFu)) mx = v; } }
    else { const uint32_t *p32 = (const uint32_t *)src; for (int k = 0; k < count; k++) { uint32_t v = p32[k]; if (v > mx && !(R.restart && v == 0xFFFFFFFFu)) mx = v; } }
    return mx;
}
/* job_list_overwrite as it was: every live list, on a copy of the finalized flags */
static void overwrite_ref(int *fin, const Overwrite *w) {
    for (int k = 0; k < g_live_n; k++) {
        List *l = &g_lists[g_live[k]];
        if (!l->data || fin[g_live[k]] || l->seq >= w->seq) continue;
        int hit = 0;
        for (int p = 0; p < l->nblk && p < 4 && !hit; p++)
            for (int j = 0; j < w->nseg && j < 4 && !hit; j++)
                hit = l->blk[p][0] == w->seg[j][0] && l->blk[p][1] < w->seg[j][2] && w->seg[j][1] < l->blk[p][2];
        if (hit) fin[g_live[k]] = 2;
    }
}
/* a piece of command memory: one of three pools, up to 8 MB in (often at or next to a chunk boundary); empty, short or
 * long; now and then wider than CELL_SPAN chunks */
static void rand_piece(uint64_t *p, int wide) {
    static const uint64_t pools[3] = {0x1000, 0x2000, 0x3000};
    p[0] = pools[rnd() % 3];
    uint64_t lo = rnd() % (8u << 20), len = rnd() % 5 == 0 ? 0 : rnd() % (rnd() % 4 == 0 ? 300000 : 3000);
    if (rnd() % 3 == 0) lo = (lo | (((uint64_t)1 << CELL_SHIFT) - 1)) + rnd() % 3;      /* a boundary - 1, the boundary, + 1 */
    if (rnd() % 3 == 0) len = (((lo + len) | (((uint64_t)1 << CELL_SHIFT) - 1)) + rnd() % 3) - lo;
    if (wide) len = (uint64_t)(CELL_SPAN + 1 + rnd() % 50) << CELL_SHIFT;
    p[1] = lo; p[2] = lo + len;
}
/* The index against the live lists: every entry must be a piece of a live list over that chunk (or a wide piece in
 * g_wide), and as many entries as the live lists' pieces cover. Returns the number of wrong entries plus the difference. */
static long long index_check(void) {
    long long wrong = 0, have = 0, want = 0;
    for (int c = 0; c < g_cells_cap; c++) {
        Cell *e = &g_cells[c];
        for (int m = 0; e->used && m < e->n; m++) {
            List *l = &g_lists[e->ix[m]];
            int ok = 0;
            for (int p = 0; l->data && p < l->nblk && p < 4 && !ok; p++) {
                uint64_t c0, c1;
                cell_span(l->blk[p], &c0, &c1);
                ok = l->blk[p][0] == e->pool && c1 - c0 < CELL_SPAN && e->chunk >= c0 && e->chunk <= c1;
            }
            wrong += !ok; have++;
        }
    }
    for (int m = 0; m < g_wide_n; m++) {
        List *l = &g_lists[g_wide[m]];
        int ok = 0;
        for (int p = 0; l->data && p < l->nblk && p < 4 && !ok; p++) { uint64_t c0, c1; cell_span(l->blk[p], &c0, &c1); ok = c1 - c0 >= CELL_SPAN; }
        wrong += !ok; have++;
    }
    for (int k = 0; k < g_live_n; k++) {
        List *l = &g_lists[g_live[k]];
        for (int p = 0; p < l->nblk && p < 4; p++) { uint64_t c0, c1; cell_span(l->blk[p], &c0, &c1); want += c1 - c0 >= CELL_SPAN ? 1 : (long long)(c1 - c0 + 1); }
    }
    return wrong + (have > want ? have - want : want - have);
}

int main(void) {
    /* 1. index_max: random ranges, alignments and value patterns */
    size_t memsz = 1 << 20;
    g_mem = malloc(memsz + 64);
    long long cases = 0, bad = 0;
    for (int it = 0; it < 200000; it++) {
        unsigned t = (unsigned)(rnd() % 3);
        int count = (int)(rnd() % (rnd() % 8 == 0 ? 5000 : 70));
        uint64_t addr = (rnd() % (memsz - ((size_t)count << t) - 16)) & ~(((uint64_t)1 << t) - 1);
        int mode = (int)(rnd() % 4);           /* small values, any, near the top, with restart values */
        for (int k = 0; k < (count << t) + 16; k++) {
            uint8_t b = (uint8_t)rnd();
            if (mode == 0) b &= 0x0f;
            if (mode == 2) b |= 0xf0;
            if (mode == 3 && rnd() % 4 == 0) b = 0xff;
            g_mem[addr + k] = b;
        }
        R.restart = (int)(rnd() % 2); R.gen++;
        uint32_t a = index_max(addr, count, t), b = index_max_ref(addr, count, t);
        cases++;
        if (a != b && bad++ < 10) printf("index_max: type %u, %d indices, restart %d: %u, one at a time %u\n", t, count, R.restart, a, b);
    }
    printf("index_max: %lld ranges, %lld differ\n", cases, bad);

    /* 2. the cell index: lists registered, overwritten and dropped at random */
    int cap = 20000;
    g_lists = calloc((size_t)cap, sizeof *g_lists); g_live = malloc(sizeof *g_live * (size_t)cap);
    int *fin = calloc((size_t)cap, sizeof *fin), *freel = malloc(sizeof *freel * (size_t)cap), nfree = 0;
    LONG64 seq = 0, ov = 0, ended = 0, diff = 0, stale = 0;
    for (int it = 0; it < 300000; it++) {
        if (it % 10000 == 9999) stale += index_check();
        int op = (int)(rnd() % 10);
        if (op < 5 && (g_list_count < cap || nfree)) {           /* a new list (as list_register) */
            int i = nfree ? freel[--nfree] : g_list_count++;
            List *l = &g_lists[i];
            memset(l->blk, 0, sizeof l->blk);
            l->data = (uint8_t *)1; l->finalized = 0; l->seq = ++seq;
            l->nblk = (int)(rnd() % 6);                           /* 5: more pieces than are kept */
            for (int p = 0; p < 4; p++) rand_piece(l->blk[p], rnd() % 2000 == 0);
            list_cells(i, 1);
            l->live_pos = g_live_n; g_live[g_live_n++] = i;
        } else if (op < 8) {                                      /* a recording over command memory */
            Overwrite w;
            w.nseg = 1 + (int)(rnd() % 4); w.seq = seq + 1 - (LONG64)(rnd() % 3);
            for (int j = 0; j < 4; j++) rand_piece(w.seg[j], rnd() % 3000 == 0);
            for (int k = 0; k < g_live_n; k++) { fin[g_live[k]] = g_lists[g_live[k]].finalized; ended -= fin[g_live[k]] != 0; }
            overwrite_ref(fin, &w);
            job_list_overwrite(&w);
            ov++;
            for (int k = 0; k < g_live_n; k++) {
                int i = g_live[k];
                ended += g_lists[i].finalized != 0;
                if (fin[i] != g_lists[i].finalized && diff++ < 10) printf("overwrite %lld: list %d finalized %d, by the walk over every list %d\n", (long long)ov, i, g_lists[i].finalized, fin[i]);
            }
        } else if (g_live_n) {                                    /* a list dropped (as list_drop) */
            int k = (int)(rnd() % g_live_n), i = g_live[k];
            list_cells(i, 0);
            int last = g_live[--g_live_n];
            g_live[g_lists[i].live_pos] = last; g_lists[last].live_pos = g_lists[i].live_pos; g_lists[i].live_pos = -1;
            g_lists[i].data = NULL; freel[nfree++] = i;
        }
    }
    stale += index_check();
    printf("overwrites: %lld (%lld lists ended by them), %lld lists differ; index entries wrong or missing in 31 checks: %lld (%d cells, %d wide entries, %d live lists)\n",
           (long long)ov, (long long)ended, (long long)diff, (long long)stale, g_cells_used, g_wide_n, g_live_n);
    return bad || diff || stale;
}
