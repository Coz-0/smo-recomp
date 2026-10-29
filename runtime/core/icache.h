/* Indirect calls of lifted code through a one-entry cache per call site (tools/lifter/recomp.py emits ic_call at BLR
 * and at a BR through a pointer when RECOMP_ICACHE=1).
 *
 * Why: the game makes ~157,000 indirect calls a frame in Cap Kingdom (C++ virtual calls), from 1,873 sites; 96 % of
 * them go where that site went the time before. Without the cache each one is two host indirect calls (game_shim.c ->
 * call_indirect), five debug-mode tests and a hash probe (hostrt.c).
 *
 * A site's cache is ONE 64-bit word, so a reader on another thread never sees half of an update:
 *     bits 63..32  the target's offset in main (main is under 4 GB)
 *     bits 31..16  its index in fn_table (54,214 functions < 65,536)
 *     bits 15..0   the epoch it was filled in
 * The host fills it (ic_fill, hostrt.c) only for a function of main that no call_indirect hook watches (with
 * SMO_NERVELOG or a sequence that follows nerves, the nerves themselves stay out), and moves the epoch (game_ic_epoch,
 * game_shim.c) whenever a hook changes, which empties every cache at once; epoch 0 = no cache (SMO_ICACHE=0,
 * SMO_ICALLS, and macOS, whose core model needs call_indirect's safe point). */
#ifndef RECOMP_ICACHE_H
#define RECOMP_ICACHE_H

#include "runtime.h"

extern volatile uint32_t game_ic_epoch;
void ic_fill(Ctx *c, uint64_t addr, uint64_t *site);     /* game_shim.c -> host: fill if allowed, then call_indirect */

static inline void ic_call(Ctx *c, uint64_t addr, uint64_t *site, uint64_t base) {
    uint64_t k = __atomic_load_n(site, __ATOMIC_RELAXED), off = addr - base;
    if ((k >> 32) == off && (uint16_t)k == (uint16_t)game_ic_epoch && (uint16_t)k) {
        fn_table[(k >> 16) & 0xffff].fn(c);
        return;
    }
    ic_fill(c, addr, site);
}

#endif
