/* smo_sharc.c - the walk to the k-th shader binary of an agl shader archive (.sharc / .sharcb) in host code.
 *
 * At boot the InitSystem thread sets up agl's shader archives: for every program of an archive and each of its four
 * stages, the game's function at main+0x9ad940 finds the stage's binary by walking the archive's list of binaries from
 * the start, one record after another (each record starts with its own size: p += *(s32 *)p), k times. A program's
 * binaries come after those of the programs before it, so the walks grow with the program's index and the whole set-up
 * is quadratic in the number of binaries: the walks were 1.76 s of the 2.12 s of CPU InitSystem took before the first
 * frame of play (perf, Ryzen 5 7535HS, build/loads p1, 27/10/2026); the records are far apart, so every step is a
 * cache miss.
 * tools/lifter/game_patches.py finds the function by its code and puts, before each of the four walks, a call here that
 * gives the walk's answer and skips it. The answer is the same record: the walk is done again, but from where the last
 * one of the same list ended when that is not further on (a program's stage k is never before the last one's), else
 * from the start. What was remembered is forgotten at every entry to the function, so a list is only ever continued
 * within the call that walked it.
 *
 * SMO_SHARC=0 leaves the game's code alone; SMO_SHARC=2 (a check) lets the game walk and compares its record with this
 * one's at the end of every walk ("smo_sharc: ... differ" at exit). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

static _Thread_local uint64_t g_base, g_p;
static _Thread_local uint32_t g_k;
static long long g_walks, g_steps, g_saved, g_checked, g_differ;
static _Thread_local uint64_t g_want;

int rt_sharc_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SMO_SHARC"); on = e && e[0] ? atoi(e) : 1; }
    return on;
}

void rt_sharc_reset(void) { g_base = 0; }

/* the record k steps after the list's start `base`, as the game's walk finds it */
uint64_t rt_sharc_walk(uint64_t base, uint32_t k) {
    uint64_t p = base;
    uint32_t i = 0;
    if (base == g_base && k >= g_k) { p = g_p; i = g_k; }
    g_walks++; g_saved += i; g_steps += k - i;
    for (; i < k; i++) { int32_t n; memcpy(&n, (const void *)(uintptr_t)p, sizeof n); p += (uint64_t)(int64_t)n; }
    g_base = base; g_k = k; g_p = p;
    return p;
}

/* SMO_SHARC=2: before a walk, the record this file finds; after it (the instruction after the walk), the game's */
void rt_sharc_expect(uint64_t base, uint32_t k) { g_want = rt_sharc_walk(base, k); }
void rt_sharc_check(uint64_t got) {
    g_checked++;
    if (got != g_want && g_differ++ < 10) printf("smo_sharc: the game's walk found %#llx, this one %#llx\n", (unsigned long long)got, (unsigned long long)g_want);
}

void smo_sharc_report(FILE *out) {
    if (rt_sharc_on() == 2) { fprintf(out, "smo_sharc: SMO_SHARC=2 (check): %lld walks of the game compared, %lld differ\n", g_checked, g_differ); return; }
    if (!rt_sharc_on()) { fprintf(out, "smo_sharc: off (SMO_SHARC=0): agl's shader archive walks run as lifted\n"); return; }
    fprintf(out, "smo_sharc: %lld walks to a shader archive's k-th binary, %lld steps taken, %lld saved by going on from the last walk\n",
            g_walks, g_steps, g_saved);
}
