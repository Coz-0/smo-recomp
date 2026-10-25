/* os/core_model.c, the console core model on macOS, without the game: threads stand in for guest threads, "run lifted
 * code" in chunks between safe points and wait through the Win32 shim (whose blocking hooks the model installs). Checks
 * the rules of docs/macos-port.md "The console core model on macOS" while they run, and measures preemption. Exit code =
 * failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "win32_posix.h"
#include "core_model.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void spin_us(double us) { uint64_t end = now_ns() + (uint64_t)(us * 1000); while (now_ns() < end) {} }

/* ---- occupancy: who runs "lifted code" on which core right now */
static volatile int g_occ[3], g_running, g_max_running, g_violations;
static void work(double us) {                    /* a chunk of lifted code: only with a core, alone on it */
    int c = cm_current_core();
    if (c < 0 || c > 2) { __atomic_add_fetch(&g_violations, 1, __ATOMIC_SEQ_CST); return; }
    if (__atomic_add_fetch(&g_occ[c], 1, __ATOMIC_SEQ_CST) != 1) __atomic_add_fetch(&g_violations, 1, __ATOMIC_SEQ_CST);
    int r = __atomic_add_fetch(&g_running, 1, __ATOMIC_SEQ_CST);
    if (r > 3) __atomic_add_fetch(&g_violations, 1, __ATOMIC_SEQ_CST);
    int m = g_max_running; while (r > m && !__sync_bool_compare_and_swap(&g_max_running, m, r)) m = g_max_running;
    spin_us(us);
    __atomic_sub_fetch(&g_running, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&g_occ[c], 1, __ATOMIC_SEQ_CST);
}

/* ---- 1. exclusivity under load: 9 threads, all core masks, chunks, safe points, shim waits */
static CRITICAL_SECTION g_cs;
static volatile int g_stop;
static long long g_chunks[16];
typedef struct { CmThread *t; int id; } Arg;
static void *load_thread(void *p) {
    Arg *a = p;
    cm_enter(a->t);
    unsigned r = (unsigned)a->id * 2654435761u;
    while (!g_stop) {
        work(20 + (r = r * 1103515245u + 12345u) % 80);
        g_chunks[a->id]++;
        CM_SAFE_POINT();
        switch ((r >> 8) % 8) {
        case 0: Sleep(1); break;                                     /* a sleep: the core goes to another thread */
        case 1: EnterCriticalSection(&g_cs); work(10); LeaveCriticalSection(&g_cs); break;   /* a shared lock, contended */
        case 2: cm_yield(); break;
        default: break;
        }
    }
    cm_leave();
    return NULL;
}
static void test_exclusive(void) {
    static const uint64_t masks[9] = {1, 1, 2, 2, 4, 4, 7, 7, 3};
    static const int prios[9] = {16, 20, 16, 18, 17, 18, 16, 22, 16};
    pthread_t th[9];
    Arg a[9];
    InitializeCriticalSection(&g_cs);
    g_stop = 0; g_violations = 0; g_max_running = 0;
    for (int i = 0; i < 9; i++) { a[i].t = cm_new(prios[i], masks[i], "load"); a[i].id = i; pthread_create(&th[i], NULL, load_thread, &a[i]); }
    Sleep(2000);
    g_stop = 1;
    for (int i = 0; i < 9; i++) pthread_join(th[i], NULL);
    long long total = 0, starved = 0;
    for (int i = 0; i < 9; i++) { total += g_chunks[i]; starved += g_chunks[i] == 0; }
    CHECK(g_violations == 0, "never two threads on a core, never more than three running, never lifted code without a core (%d violations)", g_violations);
    CHECK(g_max_running == 3, "all three cores were used at once (most at once: %d)", g_max_running);
    CHECK(starved == 0, "every thread ran (%lld chunks in 2 s, %lld threads never ran)", total, starved);
}

/* ---- 2. a blocked thread leaves its core: A (core 0, priority 16) waits for an event; B (core 0, priority 20) runs */
static HANDLE g_ev;
static volatile int g_b_ran_while_a_waits, g_a_waiting;
static void *a_waiter(void *p) {
    cm_enter(p);
    work(100);
    g_a_waiting = 1;
    WaitForSingleObject(g_ev, INFINITE);         /* blocks: core 0 is free for B */
    g_a_waiting = 0;
    work(100);
    cm_leave();
    return NULL;
}
static void *b_runner(void *p) {
    cm_enter(p);
    for (int i = 0; i < 200; i++) { work(50); if (g_a_waiting) g_b_ran_while_a_waits++; CM_SAFE_POINT(); }
    cm_leave();
    return NULL;
}
static void test_block_releases(void) {
    pthread_t a, b;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    pthread_create(&a, NULL, a_waiter, cm_new(16, 1, "A"));
    Sleep(20);
    pthread_create(&b, NULL, b_runner, cm_new(20, 1, "B"));
    Sleep(50);
    SetEvent(g_ev);
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(g_b_ran_while_a_waits > 0, "a thread waiting for an event leaves its core: the lower-priority thread of the core ran meanwhile (%d chunks)", g_b_ran_while_a_waits);
}

/* ---- 3. handover order: core 1 held; L (priority 20) queues, then M (priority 10): M gets the core first */
static volatile int g_hold_release;
static void *holder(void *p) { cm_enter(p); while (!g_hold_release) { work(20); CM_SAFE_POINT(); } cm_leave(); return NULL; }
static CmThread *g_L, *g_M;
static volatile int g_first;
static void *ord_L(void *p) { (void)p; cm_enter(g_L); if (!g_first) g_first = 'L'; cm_leave(); return NULL; }
static void *ord_M(void *p) { (void)p; cm_enter(g_M); if (!g_first) g_first = 'M'; cm_leave(); return NULL; }
static void test_order(void) {
    pthread_t h, l, m;
    g_hold_release = 0; g_first = 0;
    /* the holder has priority 5: neither waiter may take the core from it, so both queue */
    pthread_create(&h, NULL, holder, cm_new(5, 2, "holder"));
    Sleep(20);
    g_L = cm_new(20, 2, "L"); g_M = cm_new(10, 2, "M");
    pthread_create(&l, NULL, ord_L, NULL);
    Sleep(20);
    pthread_create(&m, NULL, ord_M, NULL);
    Sleep(20);
    g_hold_release = 1;
    pthread_join(h, NULL); pthread_join(l, NULL); pthread_join(m, NULL);
    CHECK(g_first == 'M', "a freed core goes to the highest-priority waiter, not the first to come (got %c)", g_first ? g_first : '?');
}

/* ---- 4. preemption: the 17-against-18 case of the Windows notes. On core 2, D (priority 18, the decompressor) runs
 * lifted code without a pause, reaching a safe point every `chunk` us; U (priority 17, the draw buffer update) sleeps 2 ms
 * and wants the core at once. Measured: the time from U ready to U running. */
static volatile int g_u_stop;
static double g_lat[4000];
static int g_nlat;
static double g_chunk_us;
static void *decomp_loop(void *p) { cm_enter(p); while (!g_u_stop) { work(g_chunk_us); CM_SAFE_POINT(); } cm_leave(); return NULL; }
static void *update_loop(void *p) {
    cm_enter(p);
    while (!g_u_stop && g_nlat < 4000) {
        cm_block_begin();                         /* the 2 ms sleep, with the ready time taken where the wait ends */
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        uint64_t ready = now_ns();
        cm_block_end();                           /* wants core 2 back: D must give it up at its next safe point */
        g_lat[g_nlat++] = (double)(now_ns() - ready) / 1000.0;
        work(200);
    }
    cm_leave();
    return NULL;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void test_preempt(double chunk_us, double limit_p99_us) {
    pthread_t d, u;
    g_u_stop = 0; g_nlat = 0; g_chunk_us = chunk_us;
    pthread_create(&d, NULL, decomp_loop, cm_new(18, 4, "DecompThread"));
    Sleep(10);
    pthread_create(&u, NULL, update_loop, cm_new(17, 4, "draw buffer update"));
    Sleep(1500);
    g_u_stop = 1;
    pthread_join(u, NULL); pthread_join(d, NULL);
    qsort(g_lat, (size_t)g_nlat, sizeof g_lat[0], cmp_d);
    double p50 = g_lat[g_nlat / 2], p99 = g_lat[(int)(g_nlat * 0.99)], mx = g_lat[g_nlat - 1];
    printf("  preemption, safe point every %.0f us: %d wake-ups, ready to running: median %.1f us, p99 %.1f us, longest %.1f us\n",
           chunk_us, g_nlat, p50, p99, mx);
    CHECK(g_nlat > 100 && p99 < limit_p99_us, "priority 17 takes core 2 from a busy priority 18 at its next safe point (p99 %.1f us, limit %.0f)", p99, limit_p99_us);
}

/* ---- 5. YieldThread: two threads of one priority on core 0 take turns */
static volatile int g_turns[2], g_yield_stop;
static void *yielder(void *p) {
    Arg *a = p;
    cm_enter(a->t);
    while (!g_yield_stop) { work(20); g_turns[a->id]++; cm_yield(); }
    cm_leave();
    return NULL;
}
static void test_yield(void) {
    pthread_t t[2];
    Arg a[2] = {{cm_new(16, 1, "Y0"), 0}, {cm_new(16, 1, "Y1"), 1}};
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, yielder, &a[i]);
    Sleep(300);
    g_yield_stop = 1;
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    double ratio = (double)g_turns[0] / (double)(g_turns[1] ? g_turns[1] : 1);
    CHECK(g_turns[0] > 100 && g_turns[1] > 100 && ratio > 0.8 && ratio < 1.25, "YieldThread alternates two threads of one priority (%d and %d turns)", g_turns[0], g_turns[1]);
}

/* ---- 6. a new core mask moves a running thread at its next safe point */
static volatile int g_cores_seen;
static volatile int g_mask_stop;
static void *mover(void *p) { cm_enter(p); while (!g_mask_stop) { work(20); g_cores_seen |= 1 << cm_current_core(); CM_SAFE_POINT(); } cm_leave(); return NULL; }
static void test_mask(void) {
    pthread_t t;
    CmThread *ct = cm_new(16, 1, "mover");
    pthread_create(&t, NULL, mover, ct);
    Sleep(30);
    int before = g_cores_seen;
    g_cores_seen = 0;
    cm_set_mask(ct, 4);
    Sleep(30);
    g_mask_stop = 1;
    pthread_join(t, NULL);
    CHECK(before == 1 && (g_cores_seen & 4) && !(g_cores_seen & 2), "SetThreadCoreMask 1 -> 4 moves the thread from core 0 to core 2 (seen %x then %x)", before, g_cores_seen);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cm_enabled();
    recomp_set_block_hooks(cm_block_begin, cm_block_end);
    test_exclusive();
    test_block_releases();
    test_order();
    test_yield();
    test_mask();
    test_preempt(20, 200);       /* lifted code with a safe point every 20 us (an SDK or indirect call) */
    test_preempt(500, 2000);     /* a long stretch without one: the wait follows it */
    cm_report(stdout);
    printf("core model: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail != 0;
}
