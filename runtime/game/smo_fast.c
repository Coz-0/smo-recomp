/* smo_fast.c - host versions of hot game code, put into the loaded game DLL at start-up.
 *
 * THE SAMPLER LIST SORT. agl::driver::NVNMgr::registerSampler_ [main] adds a sampler to the manager's list and then
 * sorts the WHOLE list again by its 16-bit key with sead::PtrArrayImpl::sort, a cocktail-shaker sort that calls the
 * comparator (agl::driver::NVNMgr::compareSamplerKey_) through a function pointer for every pair it looks at: at least
 * one pass over the list per new sampler. At Cascade Kingdom's first checkpoint the game registers about a dozen
 * samplers a frame (and releases as many): 85 million comparator calls in 30 s on the main thread (SMO_ICALLS), each
 * an indirect call into lifted code. That was 1-2 ms of the main thread a frame, and 8-10 ms in the frames where the
 * registrations bunch up, enough to miss the refresh (SMO_HITCH: the main thread inside compareSamplerKey_). Here the
 * call in registerSampler_ goes to the same algorithm in host code with the comparison inlined, so the list ends in
 * the same order, swap for swap; a call with any other comparator goes on to the lifted sort.
 *
 * HOW. Lifted functions call each other with direct calls (`f_<address>(c);`, compiled at -O1 without inlining to a
 * `call rel32` to the callee's entry). The one call to the sort in
 * the lifted registerSampler_ is found inside that function's code (its range from the unwind table) by its target,
 * and its rel32 is pointed at a stub within 2 GB that jumps to the host function. Nothing else in the DLL changes, and
 * nothing at all when the call is not found exactly once (said in the log).
 *
 * SMO_FAST=0 leaves the game's code alone. SMO_FAST_CHECK=1 also runs the lifted sort on the same list and stops the
 * game if the two orders ever differ (counts at exit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "runtime.h"
#include "hostrt.h"

#define SYM_SORT GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E
#define SYM_REGISTER GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj
#define SYM_KEYCMP GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_

static void (*g_lifted_sort)(Ctx *);
static int g_check;
static LONG64 g_sorts, g_items, g_swaps, g_other, g_checked;

static void (*host_fn_of(uint64_t guest))(Ctx *) {          /* the lifted function at a guest address (fn_table is sorted) */
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == guest) return fn_table[mid].fn;
        if (fn_table[mid].addr < guest) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* compareSamplerKey_(a, b) [main]: the 16-bit key at offset 0x62 of a minus that of b */
static inline int key_cmp(uint64_t a, uint64_t b) { return (int)rd16(a + 0x62) - (int)rd16(b + 0x62); }

/* sead::PtrArrayImpl::sort [main], step for step: a forward pass over [lo, hi) swaps where cmp(p[k], p[k+1]) >= 1, a
 * backward pass from the forward pass's last swap down to lo swaps where cmp(p[k], p[k-1]) < 0; the next passes are
 * bounded by those last swaps; it ends when a pass's last swap is where it started. */
static LONG64 shaker_sort_keys(uint64_t *p, int n) {
    int lo = 0, hi = n - 1;
    LONG64 swaps = 0;
    for (;;) {
        int last = lo;
        for (int k = lo; k < hi; k++)
            if (key_cmp(p[k], p[k + 1]) >= 1) { uint64_t t = p[k]; p[k] = p[k + 1]; p[k + 1] = t; last = k; swaps++; }
        if (lo == last) return swaps;
        int first = last;
        for (int k = last; k > lo; k--)
            if (key_cmp(p[k], p[k - 1]) < 0) { uint64_t t = p[k]; p[k] = p[k - 1]; p[k - 1] = t; first = k; swaps++; }
        hi = last;
        if (first == last) return swaps;
        lo = first;
    }
}

/* Reached from the call in registerSampler_ (through the stub): sead::PtrArrayImpl::sort(this = x0, cmp = x1). The
 * registers the lifted sort would leave behind are only ones a callee may change (x0..x18, flags), and nothing the
 * caller reads after the call; the callee-saved ones are not touched. */
static void sampler_sort(Ctx *c) {
    if (c->x[1] != SYM_KEYCMP) { g_other++; g_lifted_sort(c); return; }
    int n = (int)rd32(c->x[0]);                  /* PtrArrayImpl: +0 count (int), +8 the pointers */
    if (n < 2) return;
    uint64_t *p = (uint64_t *)(uintptr_t)rd64(c->x[0] + 8);      /* guest addresses are host addresses */
    g_sorts++; g_items += n;
    if (!g_check) { g_swaps += shaker_sort_keys(p, n); return; }
    uint64_t *before = malloc((size_t)n * 8), *mine = malloc((size_t)n * 8);
    if (!before || !mine) guest_trap("smo_fast: out of host memory for SMO_FAST_CHECK", c->x[30]);
    memcpy(before, p, (size_t)n * 8);
    g_swaps += shaker_sort_keys(p, n);
    memcpy(mine, p, (size_t)n * 8);
    memcpy(p, before, (size_t)n * 8);
    g_lifted_sort(c);                            /* the game's own sort of the same list, as the call would have run */
    for (int i = 0; i < n; i++)
        if (p[i] != mine[i]) {
            printf("smo_fast: CHECK FAILED: sampler list sort #%lld of %d entries differs from the game's at index %d (%llx, game %llx)\n",
                   (long long)g_sorts, n, i, (unsigned long long)mine[i], (unsigned long long)p[i]);
            fflush(stdout);
            guest_trap("smo_fast: the host sampler list sort differs from the game's (SMO_FAST_CHECK)", c->x[30]);
        }
    g_checked++;
    free(before); free(mine);
}

/* registerSampler_(this = x0, index = w1, sampler = x2, key = w3) [main] begins:
 *     x9 = this->slots (+0x160); w8 = this->count (+0x158); x10 = x9 + (int32)w1 * 0x68
 *     slot = count > index (unsigned) ? x10 : x9;  if (slot->refs (+0x60, u16)) return -1
 * The game's registerSampler looks for a free slot by calling it for one index after another from a cursor (next fit),
 * and in New Donk City the table holds thousands of samplers, nearly all taken: thousands of lifted calls for one new
 * sampler, a fifth of the main thread's time there (SMO_HOSTPROF). Here the taken slot is answered in host code with
 * what the game's code leaves behind (w0 = -1; x8, x9, x10 as it sets them; the callee-saved registers it restores);
 * a free slot goes to the lifted function. */
static void (*g_lifted_register)(Ctx *);
static LONG64 g_reg_taken, g_reg_free, g_reg_skipped;
/* The free-slot search that calls registerSampler_ [main 0x980e80..0x980ef4] keeps its cursor in this->+0x16a
 * (u16) and reads it again after each -1: loop 1 from the cursor + 1 up to this->count, loop 2 from 0 up to where loop
 * 1 began. A taken slot reached from one of its two calls also moves the cursor to the slot before the next free one
 * (or to count - 1): the game's next call is on that free slot, with the same outcome as its walk over the taken ones
 * (only a failed loop 2 leaves its cursor further on, past slots it found taken). In New Donk City the table is nearly
 * full (3,836 of 3,839) and the walk was about 88,000 calls a frame. g_site: the guest return addresses of the two
 * calls (x30), set when the loop's code is the one expected (install_register). */
static uint64_t g_site[2];
static void sampler_register(Ctx *c) {
    uint64_t self = c->x[0], slots = rd64(self + 0x160);
    uint32_t count = rd32(self + 0x158), index = (uint32_t)c->x[1];
    uint64_t at = slots + (uint64_t)((int64_t)(int32_t)index * 0x68);
    uint64_t slot = count > index ? at : slots;
    uint32_t refs = rd16(slot + 0x60);
    if (!refs) {
        g_reg_free++;
        g_lifted_register(c);
        return;
    }
    g_reg_taken++;
    if (g_site[0] && (c->x[30] == g_site[0] || c->x[30] == g_site[1]) && count > index && count <= 0xffff) {
        uint32_t j = index + 1;
        while (j < count && rd16(slots + (uint64_t)j * 0x68 + 0x60)) j++;
        *(uint16_t *)(uintptr_t)(self + 0x16a) = (uint16_t)(j - 1);
        g_reg_skipped += j - index - 1;
    }
    c->x[8] = refs; c->x[9] = slots; c->x[10] = at;
    c->x[0] = 0xffffffffu;                       /* mov w21, #-1 ... mov w0, w21: the upper half is zero */
}

/* A page within 2 GB of `site` for the jump stub (a rel32 must reach it). */
static uint8_t *alloc_near(uint8_t *site) {
    uintptr_t base = (uintptr_t)site & ~(uintptr_t)0xffff;
    for (uintptr_t d = 0x10000; d < 0x7ff00000; d += 0x10000) {
        for (int s = 0; s < 2; s++) {
            uintptr_t a = s ? base + d : base - d;
            if (!s && base < d) continue;
            void *m = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (m) return m;
        }
    }
    return NULL;
}

#ifdef _WIN32
/* Every direct call of the lifted registerSampler_ in the game DLL's code goes to sampler_register (through one stub). */
static void install_register(void) {
    g_lifted_register = host_fn_of(SYM_REGISTER);
    if (!g_lifted_register) { printf("smo_fast: registerSampler_'s free-slot test NOT installed: the lifted function was not found\n"); return; }
    {   /* the free-slot loop just before registerSampler_ (the same words wherever the function is) */
        static const uint32_t k_loop[30] = {0x7942d688, 0x11000508, 0x7902d688, 0x12003d01, 0xb9415a88, 0x6b01011f, 0x5400010d, 0x79401be3, 0xaa1403e0, 0xaa1503e2,
                                            0x94000072, 0x3100041f, 0x54fffe80, 0x14000022, 0x7902d69f, 0x340001f7, 0x2a1f03e8, 0x79401be3, 0x12003d01, 0xaa1403e0,
                                            0xaa1503e2, 0x94000067, 0x3100041f, 0x54000301, 0x7942d688, 0x11000508, 0x12003d09, 0x6b17013f, 0x7902d688, 0x54fffe83};
        uint64_t base = (uint64_t)SYM_REGISTER - 0x1f0;
        int same = 1;
        for (int i = 0; i < 30 && same; i++) same = *(const uint32_t *)(uintptr_t)(base + 4 * (uint64_t)i) == k_loop[i];
        const char *e = getenv("SMO_FAST_SKIP");
        if (same && !(e && !atoi(e))) { g_site[0] = base + 0x2c; g_site[1] = base + 0x58; }
        printf("smo_fast: registerSampler's free-slot walk %s\n", g_site[0] ? "jumps over taken slots in host code (SMO_FAST_SKIP=0: one call per slot)" : same ? "one call per slot (SMO_FAST_SKIP=0)" : "one call per slot (its loop is not the expected code)");
    }
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)g_lifted_register, &m);
    if (!m) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *target = (uint8_t *)(uintptr_t)g_lifted_register, *stub = NULL;
    int sites = 0, patched = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t *q = (uint8_t *)m + sec[s].VirtualAddress, *end = q + sec[s].Misc.VirtualSize;
        for (; q + 5 <= end; q++) {
            int32_t rel;
            if (q[0] != 0xE8) continue;
            memcpy(&rel, q + 1, 4);
            if (q + 5 + rel != target) continue;
            sites++;
            if (!stub) {
                static const uint8_t jmp_abs[6] = {0xFF, 0x25, 0, 0, 0, 0};
                void (*fn)(Ctx *) = sampler_register;
                DWORD old;
                if (!(stub = alloc_near(q))) break;
                memcpy(stub, jmp_abs, 6); memcpy(stub + 6, &fn, 8);
                VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
            }
            int64_t rel64 = stub - (q + 5);
            DWORD old;
            if (rel64 != (int32_t)rel64 || !VirtualProtect(q + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
            rel = (int32_t)rel64;
            memcpy(q + 1, &rel, 4);
            VirtualProtect(q + 1, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), q, 5);
            patched++;
            q += 4;
        }
    }
    printf("smo_fast: registerSampler_'s taken-slot answer in host code at %d of %d call sites (the free-slot search of registerSampler)\n", patched, sites);
}
#endif

void smo_fast_install(void) {
    const char *e = getenv("SMO_FAST");
    if (e && !atoi(e)) { printf("smo_fast: off (SMO_FAST=0): the game's sampler list sort runs as lifted\n"); return; }
    if (!SYM_SORT || !SYM_REGISTER || !SYM_KEYCMP) {   /* gen_tables.py: not in this main.elf (a stripped update) */
        printf("smo_fast: off: the game's sampler list functions are not known; its sort runs as lifted\n");
        return;
    }
    g_check = getenv("SMO_FAST_CHECK") && atoi(getenv("SMO_FAST_CHECK"));
#ifndef _WIN32
    /* macOS: the game's code is in a signed library, and a written code page fails its signature check when it runs.
     * The call cannot be redirected after loading; the sort runs as lifted (a hook made by the lifter is the way here). */
    (void)host_fn_of; (void)g_lifted_sort; (void)sampler_register;
    printf("smo_fast: NOT installed on macOS (signed code cannot be patched): the game's sampler list sort runs as lifted\n");
    return;
#else
    g_lifted_sort = host_fn_of(SYM_SORT);
    uint8_t *caller = (uint8_t *)(uintptr_t)host_fn_of(SYM_REGISTER);
    DWORD64 img = 0;
    PRUNTIME_FUNCTION rf = caller ? RtlLookupFunctionEntry((DWORD64)(uintptr_t)caller, &img, NULL) : NULL;
    if (!g_lifted_sort || !rf || (uint8_t *)(uintptr_t)(img + rf->BeginAddress) != caller) {
        printf("smo_fast: NOT installed: the lifted registerSampler_ or sead::PtrArrayImpl::sort was not found, or has no unwind entry\n");
        return;
    }
    uint8_t *end = (uint8_t *)(uintptr_t)(img + rf->EndAddress), *at = NULL;
    int found = 0;
    for (uint8_t *q = caller; q + 5 <= end; q++) {        /* call rel32 whose target is the lifted sort */
        int32_t rel;
        memcpy(&rel, q + 1, 4);
        if (q[0] == 0xE8 && q + 5 + rel == (uint8_t *)(uintptr_t)g_lifted_sort) { at = q; found++; }
    }
    if (found != 1) { printf("smo_fast: NOT installed: %d calls of the sort in the lifted registerSampler_ (%d bytes of code), expected exactly 1\n", found, (int)(end - caller)); return; }
    uint8_t *stub = alloc_near(at);
    if (!stub) { printf("smo_fast: NOT installed: no page within 2 GB of the game's code for the jump\n"); return; }
    void (*target)(Ctx *) = sampler_sort;
    static const uint8_t jmp_abs[6] = {0xFF, 0x25, 0, 0, 0, 0};      /* jmp [rip + 0], then the 8-byte address */
    memcpy(stub, jmp_abs, 6); memcpy(stub + 6, &target, 8);
    DWORD old;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
    int64_t rel64 = stub - (at + 5);
    if (rel64 != (int32_t)rel64) { printf("smo_fast: NOT installed: the stub is out of rel32 range\n"); return; }
    int32_t rel = (int32_t)rel64;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { printf("smo_fast: NOT installed: the game's code cannot be written (error %lu)\n", GetLastError()); return; }
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    printf("smo_fast: agl::driver::NVNMgr::registerSampler_ sorts its sampler list in host code (SMO_FAST=0: the game's own code%s)\n",
           g_check ? "; SMO_FAST_CHECK=1: every sort compared with the game's" : ", SMO_FAST_CHECK=1: compare with it");
    install_register();
#endif
}

void smo_fast_report(FILE *out) {
    if (g_reg_skipped) fprintf(out, "smo_fast: registerSampler's free-slot walk: %lld taken slots jumped over\n", g_reg_skipped);
    if (g_reg_taken || g_reg_free) fprintf(out, "smo_fast: registerSampler_ calls: %lld on a taken slot answered in host code, %lld on a free slot passed to the game's code\n",
                                            (long long)g_reg_taken, (long long)g_reg_free);
    if (!g_sorts && !g_other) return;
    fprintf(out, "smo_fast: sampler list sorts in host code: %lld (%lld entries in all, %lld swaps); calls with another comparator, passed to the game's sort: %lld\n",
            (long long)g_sorts, (long long)g_items, (long long)g_swaps, (long long)g_other);
    if (g_check) fprintf(out, "smo_fast: SMO_FAST_CHECK: %lld sorts compared with the game's sort of the same list, all equal\n", (long long)g_checked);
}
