/* The console core model on macOS (docs/macos-port.md "The console core model on macOS"). Windows gets the same rules from
 * its scheduler (os_threads.c: three pinned host processors, priorities); macOS has no processor affinity, so here they are
 * kept by hand:
 *   - a token per console core; a guest thread runs lifted code only while it holds the token of a core of its mask;
 *   - a thread that blocks (the Win32 shim's waits call cm_block_begin / cm_block_end) gives its token back meanwhile;
 *   - a free core goes to the highest-priority waiting thread whose mask has it, equal priorities first come first served;
 *   - a thread that cannot get a core asks the lowest-priority holder below it (in its mask) to yield; the holder does at
 *     its next safe point (CM_SAFE_POINT: SDK calls and indirect calls of lifted code).
 * One lock guards everything; a thread waits for its token on a word of its own (os_sync_wait_on_address). */
#ifndef _WIN32
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <os/lock.h>
#include <os/os_sync_wait_on_address.h>
#include "core_model.h"

#define CORES 3
#define MAX_THREADS 512

struct CmThread {
    int prio;
    uint64_t mask;                         /* console cores, bits 0..2 */
    char name[40];
    int core;                              /* the core held, -1 none */
    int waiting;                           /* in the queue for a core */
    uint64_t seq;                          /* queue order (first come first served among equals) */
    volatile uint32_t granted;             /* the word it waits on: 1 once a core was given */
    volatile int flag;                     /* asked to give its core up at the next safe point */
    int depth;                             /* block nesting (the shim's waits call each other) */
    int last_core;
    uint64_t req_ns, held_since;
    long long waits, preempted, yields;
    int registered;
};

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static CmThread *g_holder[CORES];
static CmThread *g_waiting[MAX_THREADS];
static int g_nwaiting;
static CmThread *g_all[MAX_THREADS];
static int g_nall;
static uint64_t g_seq;
static int g_on = -1;
static int g_timeslice_ms;
/* the wait from "asked to yield" to "gave the core up", the measure of safe-point preemption */
static const double k_lat_edges_us[] = {50, 200, 1000, 5000, 20000};
static long long g_lat[6], g_lat_n, g_requests, g_handoffs, g_direct;
static double g_lat_max_us;

static volatile int g_zero;
_Thread_local volatile int *cm_t_flag = &g_zero;
static _Thread_local CmThread *t_self;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

int cm_enabled(void) {
    if (g_on < 0) {
        const char *e = getenv("SMO_OS_CONSOLE_CORES"), *ts = getenv("SMO_OS_TIMESLICE");
        g_on = !(e && atoi(e) == 0);
        g_timeslice_ms = ts ? atoi(ts) : 0;
    }
    return g_on;
}

CmThread *cm_new(int priority, uint64_t core_mask, const char *name) {
    CmThread *t = calloc(1, sizeof *t);
    t->prio = priority;
    t->mask = core_mask & 7 ? core_mask & 7 : 1;
    t->core = -1;
    t->last_core = -1;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    return t;
}
void cm_set_name(CmThread *t, const char *name) { if (t && name) snprintf(t->name, sizeof t->name, "%s", name); }

/* ---- with the lock held */
static void dequeue(CmThread *t) {
    if (!t->waiting) return;
    for (int i = 0; i < g_nwaiting; i++) if (g_waiting[i] == t) { g_waiting[i] = g_waiting[--g_nwaiting]; break; }
    t->waiting = 0;
}
/* the waiting thread that should get core c: the highest priority (lowest number), then the earliest; only one of
 * priority better than `limit` (strict) or at least as good (!strict) */
static CmThread *best_waiter(int c, int limit, int strict) {
    CmThread *best = NULL;
    for (int i = 0; i < g_nwaiting; i++) {
        CmThread *w = g_waiting[i];
        if (!(w->mask >> c & 1) || (strict ? w->prio >= limit : w->prio > limit)) continue;
        if (!best || w->prio < best->prio || (w->prio == best->prio && w->seq < best->seq)) best = w;
    }
    return best;
}
static void grant(CmThread *t, int c) {
    dequeue(t);
    g_holder[c] = t;
    t->core = t->last_core = c;
    t->held_since = now_ns();
    __atomic_store_n(&t->granted, 1, __ATOMIC_RELEASE);
    os_sync_wake_by_address_any((void *)&t->granted, sizeof t->granted, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}
/* t gives its core up: to the best waiter of that core, or the core is free */
static void give_up(CmThread *t) {
    int c = t->core;
    if (c < 0) return;
    if (t->req_ns) {                       /* it had been asked to: how long that took */
        double us = (double)(now_ns() - t->req_ns) / 1000.0;
        int b = 0;
        while (b < 5 && us >= k_lat_edges_us[b]) b++;
        g_lat[b]++; g_lat_n++;
        if (us > g_lat_max_us) g_lat_max_us = us;
        t->req_ns = 0;
    }
    t->flag = 0;
    t->core = -1;
    g_holder[c] = NULL;
    CmThread *w = best_waiter(c, 1 << 30, 1);
    if (w) { g_handoffs++; grant(w, c); }
}
/* a thread that waits for a core asks the lowest-priority holder below it, on a core of its mask, to yield */
static void ask_preempt(CmThread *t) {
    CmThread *victim = NULL;
    for (int c = 0; c < CORES; c++) {
        CmThread *h = g_holder[c];
        if (!(t->mask >> c & 1) || !h || h->prio <= t->prio) continue;
        if (!victim || h->prio > victim->prio) victim = h;
    }
    if (victim && !victim->flag) { victim->flag = 1; victim->req_ns = now_ns(); g_requests++; }
}
/* t takes a free core of its mask (its last one first), or queues and asks for one; 1 = it has a core now */
static int take(CmThread *t) {
    int c = -1;
    if (t->last_core >= 0 && (t->mask >> t->last_core & 1) && !g_holder[t->last_core]) c = t->last_core;
    for (int k = 0; k < CORES && c < 0; k++) if ((t->mask >> k & 1) && !g_holder[k]) c = k;
    if (c >= 0) { g_direct++; grant(t, c); return 1; }
    t->granted = 0;
    t->seq = ++g_seq;
    t->waiting = 1;
    t->waits++;
    if (g_nwaiting < MAX_THREADS) g_waiting[g_nwaiting++] = t;
    ask_preempt(t);
    return 0;
}

/* ---- without the lock */
static void wait_granted(CmThread *t) {
    while (!__atomic_load_n(&t->granted, __ATOMIC_ACQUIRE))
        if (os_sync_wait_on_address((void *)&t->granted, 0, sizeof t->granted, OS_SYNC_WAIT_ON_ADDRESS_NONE) < 0 && errno != EINTR && errno != EFAULT) break;
}
static void *timeslice_thread(void *arg);

void cm_enter(CmThread *t) {
    static int ticker;
    t_self = t;
    cm_t_flag = &t->flag;
    os_unfair_lock_lock(&g_lock);
    if (!t->registered && g_nall < MAX_THREADS) { g_all[g_nall++] = t; t->registered = 1; }
    if (g_timeslice_ms > 0 && !ticker) { pthread_t p; ticker = 1; pthread_create(&p, NULL, timeslice_thread, NULL); pthread_detach(p); }
    int ok = take(t);
    os_unfair_lock_unlock(&g_lock);
    if (!ok) wait_granted(t);
}
void cm_leave(void) {
    CmThread *t = t_self;
    if (!t) return;
    os_unfair_lock_lock(&g_lock);
    give_up(t);
    dequeue(t);
    os_unfair_lock_unlock(&g_lock);
    t_self = NULL;
    cm_t_flag = &g_zero;
}
void cm_block_begin(void) {
    CmThread *t = t_self;
    if (!t || t->depth++) return;
    os_unfair_lock_lock(&g_lock);
    give_up(t);
    os_unfair_lock_unlock(&g_lock);
}
void cm_block_end(void) {
    CmThread *t = t_self;
    if (!t || --t->depth) return;
    os_unfair_lock_lock(&g_lock);
    int ok = take(t);
    os_unfair_lock_unlock(&g_lock);
    if (!ok) wait_granted(t);
}
void cm_safe_point(void) {
    CmThread *t = t_self;
    if (!t || t->depth) return;
    os_unfair_lock_lock(&g_lock);
    int c = t->core, ok = 1;
    t->flag = 0;
    /* give the core up when a waiter of higher priority wants it, when the core left the thread's mask, or (time slices
     * on) when it has run its slice and a waiter of the same priority wants the core */
    int sliced = g_timeslice_ms > 0 && c >= 0 && now_ns() - t->held_since >= (uint64_t)g_timeslice_ms * 1000000ull && best_waiter(c, t->prio, 0);
    if (c >= 0 && (!(t->mask >> c & 1) || best_waiter(c, t->prio, 1) || sliced)) {
        t->preempted++;
        give_up(t);
        ok = take(t);
    } else t->req_ns = 0;
    os_unfair_lock_unlock(&g_lock);
    if (!ok) wait_granted(t);
}
void cm_yield(void) {
    CmThread *t = t_self;
    if (!t || t->depth) return;
    os_unfair_lock_lock(&g_lock);
    int c = t->core, ok = 1;
    if (c >= 0 && best_waiter(c, t->prio, 0)) {   /* a ready thread of the same or higher priority gets the core */
        t->yields++;
        give_up(t);
        ok = take(t);
    }
    os_unfair_lock_unlock(&g_lock);
    if (!ok) wait_granted(t);
}
int cm_current_core(void) { return t_self ? t_self->core : -1; }

void cm_set_priority(CmThread *t, int priority) {
    if (!t) return;
    os_unfair_lock_lock(&g_lock);
    t->prio = priority;
    if (t->core >= 0) { if (best_waiter(t->core, priority, 1) && !t->flag) { t->flag = 1; t->req_ns = now_ns(); g_requests++; } }
    else if (t->waiting) ask_preempt(t);
    os_unfair_lock_unlock(&g_lock);
}
void cm_set_mask(CmThread *t, uint64_t core_mask) {
    if (!t) return;
    os_unfair_lock_lock(&g_lock);
    t->mask = core_mask & 7 ? core_mask & 7 : 1;
    if (t->core >= 0 && !(t->mask >> t->core & 1) && !t->flag) { t->flag = 1; t->req_ns = now_ns(); g_requests++; }
    if (t->waiting) {                         /* a core of the new mask may be free now */
        for (int c = 0; c < CORES; c++) if ((t->mask >> c & 1) && !g_holder[c]) { g_direct++; grant(t, c); break; }
        if (t->waiting) ask_preempt(t);
    }
    os_unfair_lock_unlock(&g_lock);
}

/* SMO_OS_TIMESLICE=<ms>: holders that ran their slice while a thread of the same priority waits are asked to yield */
static void *timeslice_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
        os_unfair_lock_lock(&g_lock);
        uint64_t now = now_ns();
        for (int c = 0; c < CORES; c++) {
            CmThread *h = g_holder[c];
            if (h && !h->flag && now - h->held_since >= (uint64_t)g_timeslice_ms * 1000000ull && best_waiter(c, h->prio, 0)) { h->flag = 1; h->req_ns = now; g_requests++; }
        }
        os_unfair_lock_unlock(&g_lock);
    }
    return NULL;
}

void cm_report(FILE *out) {
    if (!g_on || !g_nall) return;
    os_unfair_lock_lock(&g_lock);
    fprintf(out, "nn::os core model (macOS, core tokens): %lld cores taken free, %lld handed to a waiter, %lld preemption requests; "
                 "the wait from a request to the core given up: %lld measured, <50us %lld, <200us %lld, <1ms %lld, <5ms %lld, <20ms %lld, "
                 ">=20ms %lld, longest %.0f us%s\n",
            g_direct, g_handoffs, g_requests, g_lat_n, g_lat[0], g_lat[1], g_lat[2], g_lat[3], g_lat[4], g_lat[5], g_lat_max_us,
            g_timeslice_ms > 0 ? " (time slices on: SMO_OS_TIMESLICE)" : "");
    for (int i = 0; i < g_nall; i++) {
        CmThread *t = g_all[i];
        fprintf(out, "  core model: %-28s priority %2d mask %llx: waited for a core %lld times, preempted %lld, yielded %lld\n",
                t->name[0] ? t->name : "?", t->prio, (unsigned long long)t->mask, t->waits, t->preempted, t->yields);
    }
    os_unfair_lock_unlock(&g_lock);
}
#endif
