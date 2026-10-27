/* nn::os threading and synchronisation on top of Win32.
 *
 * Guest synchronisation objects (MutexType, EventType, ...) are opaque here:
 * each one is identified by its guest address and backed by a host object
 * kept in a side table, so the guest layout never has to be known. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include "win32_posix.h"
#include "core_model.h"
#endif
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define TICK_HZ 19200000ULL

typedef struct Obj {
    uint64_t key;
    struct Obj *next;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int signaled, autoclear;                 /* events */
    uint64_t *buf, cap, len, head;           /* message queues */
    HANDLE thread;                           /* threads */
    uint64_t fn, arg, stack, stack_size, name;
    int priority, finished;
    int ideal_core, core_set;                /* threads: nn::os ideal core and affinity mask as the game set them */
    uint64_t core_mask;
    struct Obj *alias;                       /* system event attached by handle: the event object it shares */
    uint32_t rhandle, whandle;               /* kernel-style handles of a system event created here */
#ifndef _WIN32
    struct CmThread *cm;                     /* threads: their place in the core model (core_model.c) */
#endif
} Obj;

#define BUCKETS 65536
static Obj *g_table[BUCKETS];
static SRWLOCK g_table_lock = SRWLOCK_INIT;

static Obj *obj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    AcquireSRWLockExclusive(&g_table_lock);
    Obj *o = g_table[h];
    while (o && o->key != key) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        InitializeCriticalSection(&o->cs);
        InitializeConditionVariable(&o->cv);
        o->next = g_table[h];
        g_table[h] = o;
    }
    ReleaseSRWLockExclusive(&g_table_lock);
    return o->alias ? o->alias : o;            /* a system event attached by handle shares its event */
}
/* the object itself, without following an alias (its own clear mode) */
static Obj *obj_raw(uint64_t key) {
    Obj *o = obj(key);
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    AcquireSRWLockShared(&g_table_lock);
    Obj *r = g_table[h];
    while (r && r->key != key) r = r->next;
    ReleaseSRWLockShared(&g_table_lock);
    return r ? r : o;
}
/* Forget an object so a new one created at the same address starts clean. */
static void obj_reset(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = o->autoclear = 0;
    o->len = o->head = 0;
    LeaveCriticalSection(&o->cs);
}

static DWORD ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000); }

/* nn::os::SleepThread takes nanoseconds and the console's timer is good to a few microseconds. Sleep() counts whole
 * milliseconds on a 1 ms (or 15.6 ms) grid, so the wait runs on a high-resolution waitable timer (100 ns units,
 * Windows 10 1803 and later); Sleep() is only the fallback. A zero or negative span yields, as on the console. */
static int console_cores(void);
static void sleep_ns(int64_t ns) {
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int no_timer;
    LARGE_INTEGER due;
#ifdef _WIN32
    if (ns <= 0) { SwitchToThread(); return; }
#else
    if (ns <= 0) { if (console_cores()) cm_yield(); else SwitchToThread(); return; }   /* SleepThread(0) yields, as on the console */
#endif
#ifdef _WIN32
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) no_timer = 1;
    }
#else
    (void)no_timer;
#endif
    due.QuadPart = -((ns + 99) / 100);
    /* SMO_SLEEP_LOG=1: a line for every sleep that lasted 2 ms longer than asked (first 200). */
    static int lg = -1; static LONG nl;
    if (lg < 0) lg = getenv("SMO_SLEEP_LOG") && atoi(getenv("SMO_SLEEP_LOG"));
    LARGE_INTEGER q0, q1, fq;
    if (lg) QueryPerformanceCounter(&q0);
#ifdef _WIN32
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) Sleep(ns_to_ms(ns));
    else WaitForSingleObject(timer, INFINITE);
#else
    {   /* macOS: nanosleep is good to tens of microseconds, no timer object needed */
        struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
        (void)due;
        cm_block_begin();                                 /* the core goes to another thread meanwhile (core_model.c) */
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
        cm_block_end();
    }
#endif
    if (lg) {
        extern long long nvn_front_frame_next(void);
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart, asked = (double)ns / 1e6;
        {   /* per thread and frame: how many sleeps, asked and slept in all; a line when a frame after 1900 had over 8 ms of them */
            static __declspec(thread) long long fr0, cnt; static __declspec(thread) double a_sum, s_sum, a_max; static LONG nf;
            long long fr = nvn_front_frame_next();
            if (fr != fr0) {
                if (fr0 > 1900 && s_sum > 8.0 && InterlockedIncrement(&nf) <= 300) printf("sleep_log: frame %lld, thread %lu: %lld sleeps in the frame, asked %.3f ms in all (longest asked %.3f), slept %.3f ms\n", fr0, (unsigned long)GetCurrentThreadId(), cnt, a_sum, a_max, s_sum);
                fr0 = fr; cnt = 0; a_sum = s_sum = a_max = 0;
            }
            cnt++; a_sum += asked; s_sum += ms; if (asked > a_max) a_max = asked;
        }
        if (ms > asked + 2.0 && InterlockedIncrement(&nl) <= 600) { printf("sleep_log: frame %lld, thread %lu: asked %.3f ms, slept %.3f ms (%s)\n", nvn_front_frame_next(), (unsigned long)GetCurrentThreadId(), asked, ms, timer ? "high-resolution timer" : "Sleep"); fflush(stdout); }
    }
}

/* nn::os thread priorities run from 0 (highest) to 31 (lowest), 16 is the default; the application's main thread
 * starts at the priority of its NPDM (44 - 28 = 16 in nn::os terms). Windows has seven levels per process class:
 * the order is kept, the spacing is not. */
#define OS_DEFAULT_PRIORITY 16
#define OS_DEFAULT_CORE 0                    /* main.npdm: default core id 0 */
/* Console scheduling model (on by default; SMO_OS_CONSOLE_CORES=0 turns it off). The game creates its threads on one
 * core of the console's three (mask 1, 2 or 4) or on all three (os_thread_report lists them at the end of every run),
 * and the console runs on each core only the highest-priority ready thread of that core. With the model:
 *   - three host processors stand for the console's cores 0..2 and every guest thread's affinity is its nn::os core mask
 *     mapped onto them (set at StartThread and at every SetThreadCoreMask), so two threads the console keeps on one core
 *     never run at the same time here either;
 *   - six priority levels (<= 4, 5..8, 9..15, 16, 17, 18..31), Windows' dynamic priority boost off: the game
 *     uses 0, 7, 8, 15, 16, 17, 18, 19, 20 and
 *     22; what must stay apart is 17 against 18 on core 2: the model draw buffer update (描画バッファ更新, 17) that every
 *     scene starts at the end of a frame and waits for at the start of the next, and the archive decompression
 *     (DecompThread, 18) busy while a stage loads. At one level the update waited behind the decompression for a whole
 *     time slice, long enough for the scene being left to be deleted under it (HakoniwaStateDeleteScene runs a frame or
 *     two after the scene's last frame): "pure virtual function called" in the update's list of actors on entering a
 *     stage from the title menu, 2 times in 91 runs of the stage stress test (tests/stress_stages.py, two games on the
 *     same three processors); with 18..31 a level lower, 0 times in 276 runs of it. 17 is the last level above
 *     Windows' lowest;
 *   - GetCurrentCoreNumber / GetCurrentProcessorNumber answer the core the thread is really on.
 * NOT the console's scheduler all the same: Windows time-slices equal priorities, other processes of the PC take the
 * three processors too, and host-side threads (graphics driver, audio device, movie decoding) are not part of it.
 * ON BY DEFAULT (SMO_OS_CONSOLE_CORES=0 turns it off). Measured on 20 s boots, 6 runs each: with free
 * scheduling 4 of 6 runs failed in sead heap code on the loading path (sead::ExpHeap::tryAlloc failing inside
 * ParallelSZSDecompressor, or a sead::Heap destructor reading address -1); with the console core model 0 of 6 failed.
 * SMO depends on its threads being scheduled the console's way, so that is the default.
 * The three processors are taken from the top of a slot of six (three cores with their second hardware threads): every
 * game running at once on the PC takes the first slot no other game holds (a named mutex per slot, held while the
 * process lives), so games side by side (tests, or a test beside the copy being played) do not share processors. One
 * game alone takes slot 0, the top of the affinity mask: the processors it always had. Before the slots, four test
 * games at once all ran their console cores on the same three processors. SMO_OS_HOST_CPUS=<hexadecimal mask> chooses
 * from those processors instead (and takes no slot). */
static int g_cc = -1;
static DWORD_PTR g_cc_core[3];
static DWORD_PTR core_slot(DWORD_PTR proc) {           /* the first free slot of six processors from the top of proc */
    int bits[64], nb = 0;
    for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0; b--) if (proc >> b & 1) bits[nb++] = b;
    for (int k = 0; k < nb / 6; k++) {
        char name[64];
        snprintf(name, sizeof name, "Local\\smo_console_cores_%d", k);
        HANDLE m = CreateMutexA(NULL, FALSE, name);     /* kept open: the slot is this process's until it ends */
        if (!m) break;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); continue; }
        DWORD_PTR mask = 0;
        for (int i = 6 * k; i < 6 * k + 6; i++) mask |= (DWORD_PTR)1 << bits[i];
        printf("nn::os: console cores in processor slot %d of %d (mask %llx; other games running at once take other slots)\n", k, nb / 6, (unsigned long long)mask);
        return mask;
    }
    printf("nn::os: every processor slot is held by other games: sharing the top of the mask\n");
    return proc;
}
static int console_cores(void) {
#ifndef _WIN32
    /* macOS has no processor affinity: the model there is core_model.c, a token per console core that a guest thread holds
     * while it runs lifted code (docs/macos-port.md "The console core model on macOS"). SMO_OS_CONSOLE_CORES=0 off. */
    if (g_cc < 0) {
        g_cc = cm_enabled();
        if (g_cc) recomp_set_block_hooks(cm_block_begin, cm_block_end);
        printf("nn::os: console core model %s (macOS: core tokens; SMO_OS_CONSOLE_CORES=0 turns it off)\n", g_cc ? "ON" : "OFF");
    }
    return g_cc;
#endif
    if (g_cc < 0) {
        int n = 0, on = 0;
        const char *cc = getenv("SMO_OS_CONSOLE_CORES"), *cpus = getenv("SMO_OS_HOST_CPUS");
        if (!cc || atoi(cc) != 0) {
            DWORD_PTR proc = 0, sys = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
            if (cpus && cpus[0]) {
                DWORD_PTR want = (DWORD_PTR)_strtoui64(cpus, NULL, 16);
                printf("nn::os: SMO_OS_HOST_CPUS=%s: the console's cores on these processors only (%llx of %llx)\n", cpus, (unsigned long long)(want & proc), (unsigned long long)proc);
                if (want & proc) proc &= want;
            } else proc = core_slot(proc);
            for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0 && n < 3; b--)      /* from the top, every second processor: not two hardware threads of one core */
                if (proc >> b & 1) { g_cc_core[n++] = (DWORD_PTR)1 << b; b--; }
            on = n == 3;
            printf("nn::os: console core model %s (SMO_OS_CONSOLE_CORES): host processor masks %llx %llx %llx for cores 0 1 2\n", on ? "ON" : "NOT possible (fewer than 3 usable processors)",
                   (unsigned long long)g_cc_core[0], (unsigned long long)g_cc_core[1], (unsigned long long)g_cc_core[2]);
        }
        g_cc = on;
    }
    return g_cc;
}
/* The processors for the runtime's own busy threads (the render thread, the pipeline workers and the helper threads of the
 * renderer): the process's minus the console cores' processors and the second hardware threads of their cores, so that
 * no game thread pinned there (the main thread, the presentation thread at priority 7 ...) preempts them and they do not
 * share a core with one. Without this the render thread was found "not on a CPU" in frames that missed the 60 Hz refresh
 * (SMO_HITCH). 0 when that would leave nothing, or with SMO_HOST_PIN=0; SMO_HOST_CPUS=<hex mask> chooses them. */
uint64_t os_host_cpu_mask(void) {
    static uint64_t mask = 1;
    if (mask == 1) {
        const char *pin = getenv("SMO_HOST_PIN"), *cpus = getenv("SMO_HOST_CPUS");
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        mask = 0;
        if (pin && atoi(pin) == 0) {}
        else if (cpus && cpus[0]) mask = (uint64_t)_strtoui64(cpus, NULL, 16) & proc;
        else if (console_cores()) {
            uint64_t game = 0;
            for (int i = 0; i < 3; i++) game |= g_cc_core[i] | (g_cc_core[i] & 0xaaaaaaaaaaaaaaaaull ? g_cc_core[i] >> 1 : g_cc_core[i] << 1);
            mask = (uint64_t)proc & ~game;
        }
        printf("nn::os: the runtime's own threads (render, pipeline workers, helpers) on processors %llx%s\n", (unsigned long long)mask,
               mask ? " (away from the console cores and their second hardware threads; SMO_HOST_PIN=0: anywhere)" : ": anywhere");
    }
    return mask;
}
/* SMO_HOST_LAYOUT=1: the render thread gets the lowest core of that mask to itself (one processor of it, the core's second
 * hardware thread left idle) and the runtime's other threads the rest, so that no helper shares a core with it. */
static int host_layout(void) { static int on = -1; if (on < 0) { const char *e = getenv("SMO_HOST_LAYOUT"); on = e && atoi(e) > 0; } return on; }
static uint64_t render_core_mask(void) {          /* both processors of the lowest core in the host mask, or 0 */
    uint64_t m = os_host_cpu_mask();
    if (!m) return 0;
    int b = 0;
    while (!(m >> b & 1)) b++;
    return (3ull << (b & ~1)) & m;
}
void os_pin_host_thread(void) {
    uint64_t m = os_host_cpu_mask();
    if (m && host_layout() && (m & ~render_core_mask())) m &= ~render_core_mask();
    if (m) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m);
}
void os_pin_render_thread(void) {
    uint64_t rc = render_core_mask();
    if (!host_layout() || !rc) { os_pin_host_thread(); return; }
    uint64_t one = rc & (~rc + 1);                 /* the core's first processor only */
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)one);
    printf("nn::os: SMO_HOST_LAYOUT=1: the render thread alone on processor mask %llx, the runtime's other threads on %llx\n", (unsigned long long)one, (unsigned long long)(os_host_cpu_mask() & ~rc));
}
static void apply_sched(HANDLE h, uint64_t core_mask) {
    DWORD_PTR m = 0;
    if (!console_cores() || !h) return;
    for (int i = 0; i < 3; i++) if (core_mask >> i & 1) m |= g_cc_core[i];
    SetThreadAffinityMask(h, m ? m : g_cc_core[0]);
    SetThreadPriorityBoost(h, TRUE);
}
static int host_priority(int prio) {
    static int split = -1;                                     /* TEST ONLY, SMO_OS_PRIO17=0: 17 shares its level with 18..27 again */
    if (split < 0) { const char *e = getenv("SMO_OS_PRIO17"); split = !e || atoi(e) != 0; }
    if (console_cores()) {
        if (prio <= 4) return THREAD_PRIORITY_TIME_CRITICAL;
        if (prio <= 8) return THREAD_PRIORITY_HIGHEST;
        if (prio >= 18 && (split || prio >= 28)) return THREAD_PRIORITY_LOWEST;     /* below the draw buffer update (17), see above */
    }
    if (prio <= 4) return THREAD_PRIORITY_HIGHEST;
    if (prio <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (prio == 16) return THREAD_PRIORITY_NORMAL;
    if (prio <= 27) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

/* ---------------------------------------------------------------- threads */
static __declspec(thread) uint64_t t_thread;          /* guest ThreadType* of the running thread */
static __declspec(thread) uint64_t t_tls[64];
static uint64_t g_tls_dtor[64];                       /* guest destructor of each slot, 0 = none */
static volatile LONG g_tls_used[64];
static void tls_run_destructors(Ctx *c);
static uint8_t g_main_thread[0x400];
uint64_t os_main_thread_type(void) { return (uint64_t)(uintptr_t)g_main_thread; }   /* boot.c: stored at thread block +0x1f8, as [sdk 0x3349dc] does */
uint64_t g_main_stack_base, g_main_stack_size;        /* set by boot.c */
static volatile LONG g_thread_count;

static Ctx *new_ctx(uint64_t stack_top) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    c->x[31] = stack_top & ~15ull;
    c->tpidr = (uint64_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return c;
}

/* Every guest thread's context, so a report can say where each one is. */
static struct { Obj *o; Ctx *c; } g_threads[256];
static volatile LONG g_threads_n;

void dump_guest_threads(FILE *out) {
    fprintf(out, "\nguest threads (last function entered, link register):\n");
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        Ctx *c = g_threads[i].c;
        fprintf(out, "  %-20s %s  f_%llx  lr=%llx  %s%s (from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
                o->finished ? "finished" : "running ", (unsigned long long)c->fn, (unsigned long long)c->x[30],
                c->in_import ? "in " : "", c->in_import ? c->in_import : "guest code", (unsigned long long)c->import_from);
    }
}

/* hitch.c (SMO_HITCH): guest thread i -> context, name, host handle; 0 past the end. */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h) {
    if (i < 0 || i >= g_threads_n || i >= 256 || !g_threads[i].o || !g_threads[i].c) return 0;
    *c = g_threads[i].c; *name = g_threads[i].o->name ? (const char *)(uintptr_t)g_threads[i].o->name : NULL; *h = g_threads[i].o->finished ? NULL : g_threads[i].o->thread;
    return 1;
}
/* Name of the guest thread running on host thread `tid`, or NULL (nvn_gl.c: CPU time per thread). */
const char *guest_thread_name_of(DWORD tid) {
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        if (o && o->thread && GetThreadId(o->thread) == tid) return o->name ? (const char *)(uintptr_t)o->name : "?";
    }
    return NULL;
}

static DWORD WINAPI thread_main(LPVOID arg) {
    Obj *o = arg;
    t_thread = o->key;
    Ctx *c = new_ctx(o->stack + o->stack_size);
    wr64(c->tpidr + 0x1f8, o->key);                   /* Horizon thread-local region: the ThreadType pointer lives at +0x1f8 */
    LONG slot = InterlockedIncrement(&g_threads_n) - 1;
    if (slot < 256) { g_threads[slot].o = o; g_threads[slot].c = c; }
    c->x[0] = o->arg;
    trace_printf("--- thread %llx started (entry %llx, name %s)\n", (unsigned long long)o->key, (unsigned long long)o->fn,
                 o->name ? (const char *)(uintptr_t)o->name : "?");
#ifndef _WIN32
    if (o->cm) cm_enter(o->cm);                       /* lifted code from here on runs on a console core (core_model.c) */
#endif
    call_guest(c, o->fn);
    tls_run_destructors(c);
#ifndef _WIN32
    if (o->cm) cm_leave();
#endif
    trace_printf("--- thread %llx finished\n", (unsigned long long)o->key);
    EnterCriticalSection(&o->cs);
    o->finished = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
    return 0;
}

/* the fields of nn::os::ThreadType (SDK 3.5.1) that the game reads itself. [sdk 0x8a680]
 * detail::ThreadManager::CreateThread(this, ThreadType *t, fn, arg, stack, size, priority, core) stores priority at +0x44,
 * the stack at +0x48, the stack the thread RUNS on at +0x50 (a mapped alias of +0x48 made by CreateAliasStackUnsafe; here
 * the thread runs on the stack it was given, so the same address), the size at +0x58, arg at +0x60 and fn at +0x68.
 * sead::MemUtil::isStack (main 0xb5dad0) compares a pointer with [+0x50, +0x50 + size) of every sead thread:
 * al::createStringIfInStack (main 0x815f84) copies a string to the heap only when that says "on a stack" (the opening
 * demo's DemoActionList::initSerial keeps its action names this way). */
static void thread_type_fill(uint64_t t, uint64_t fn, uint64_t arg, uint64_t stack, uint64_t size, int priority) {
    if (!t) return;
    wr32(t + 0x44, (uint32_t)priority);
    wr64(t + 0x48, stack); wr64(t + 0x50, stack); wr64(t + 0x58, size);
    wr64(t + 0x60, arg); wr64(t + 0x68, fn);
}
static void os_CreateThread(Ctx *c) {                 /* (ThreadType*, fn, arg, stack, stackSize, priority[, core]) */
    thread_type_fill(A0, A1, A2, A3, A4, (int)c->x[5]);
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4; o->priority = (int)c->x[5];
    o->finished = 0; o->thread = NULL; o->name = 0;
    o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; o->core_set = 1;
    RET(0);
}
static void os_CreateThreadOnCore(Ctx *c) {           /* (ThreadType*, fn, arg, stack, stackSize, priority, idealCore) */
    uint64_t t = A0;
    int core = (int)c->x[6];
    os_CreateThread(c);
    if (core >= 0 && core < 64) { Obj *o = obj(t); o->ideal_core = core; o->core_mask = 1ull << core; }
}
/* The thread log: every thread the game
 * starts, with the nn::os arguments it was given, and every later change of priority or core mask. Printed at the end
 * of a run by os_thread_report (hostrt.c report_and_exit). */
static char g_oslog[24000];
static volatile LONG g_oslog_n, g_oslog_lock;
static void oslog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    while (InterlockedCompareExchange(&g_oslog_lock, 1, 0)) YieldProcessor();
    if (g_oslog_n + n < (LONG)sizeof g_oslog - 1) { memcpy(g_oslog + g_oslog_n, line, (size_t)n); g_oslog_n += n; }
    g_oslog_lock = 0;
}
void os_thread_report(FILE *out) {
#ifndef _WIN32
    cm_report(out);
#endif
    if (!g_oslog_n) return;
    fprintf(out, "nn::os threads of this run (priority 0 highest .. 31 lowest; cores 0..2 exist on the console):\n%.*s", (int)g_oslog_n, g_oslog);
}
static void os_StartThread(Ctx *c) {
    Obj *o = obj(A0);
    oslog("  start  %-28s entry %llx priority %2d ideal core %d mask %llx stack 0x%llx (called from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
          (unsigned long long)o->fn, o->priority, o->ideal_core, (unsigned long long)o->core_mask, (unsigned long long)o->stack_size, (unsigned long long)c->x[30]);
    InterlockedIncrement(&g_thread_count);
#ifndef _WIN32
    if (console_cores() && !o->cm) o->cm = cm_new(o->priority, o->core_mask, o->name ? (const char *)(uintptr_t)o->name : "?");
#endif
    o->thread = CreateThread(NULL, (SIZE_T)256 << 20, thread_main, o, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (o->thread) { SetThreadPriority(o->thread, host_priority(o->priority)); apply_sched(o->thread, o->core_mask); ResumeThread(o->thread); }
}
/* The process runs in the ABOVE_NORMAL priority class (SMO_PRIORITY_CLASS=normal: the NORMAL class, as before). With the
 * console core model every guest thread may run on one host processor only, and the game's helpers at nn::os priority
 * 17..27 (view update, clipping, draw buffer update: the main thread waits for them every frame) are BELOW_NORMAL
 * threads: in the NORMAL class any normal thread of another program on one of those three processors comes first. One
 * class up every level of the mapping is two higher (the six levels stay apart: 8 9 10 11 12 15), above the normal
 * threads of other programs (8); HIGH would put HIGHEST and TIME_CRITICAL together. Measured at Cascade Kingdom's first
 * checkpoint: with up to 8 busy programs (11 of 32 processors busy) Windows keeps them off those three processors and
 * the class changes nothing; with every processor busy (30 busy programs) the game in the NORMAL class got 44% of one
 * processor and had not reached play after 120 s, in ABOVE_NORMAL it played at 38-39 fps. */
static void process_priority(void) {
    const char *e = getenv("SMO_PRIORITY_CLASS");
    if (e && !_stricmp(e, "normal")) { printf("nn::os: process priority class NORMAL (SMO_PRIORITY_CLASS=normal)\n"); return; }
    BOOL ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    printf("nn::os: process priority class ABOVE_NORMAL%s (SMO_PRIORITY_CLASS=normal for the NORMAL class)\n", ok ? "" : ": REFUSED");
}
/* boot.c, on the main guest thread before the game starts: main.npdm gives it core 0 and priority 44 - 28 = 16 */
static Obj *thread_obj(uint64_t key);
void os_apply_main_sched(void) {
    process_priority();
    if (!console_cores()) return;
#ifndef _WIN32
    {   /* the guest's main thread (boot.c) takes core 0, as main.npdm says, before the game's first instruction */
        Obj *m = thread_obj(0);
        if (!m->cm) m->cm = cm_new(OS_DEFAULT_PRIORITY, 1ull << OS_DEFAULT_CORE, "main");
        cm_enter(m->cm);
    }
#endif
    apply_sched(GetCurrentThread(), 1ull << OS_DEFAULT_CORE);
    SetThreadPriority(GetCurrentThread(), host_priority(OS_DEFAULT_PRIORITY));
}
static void os_WaitThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) WaitForSingleObject(o->thread, INFINITE); }
static void os_DestroyThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) { CloseHandle(o->thread); o->thread = NULL; } }
static void os_SetThreadName(Ctx *c) {               /* the guest string may be a temporary: keep a copy */
    char *copy = _strdup((const char *)(uintptr_t)A1);
    obj(A0)->name = (uint64_t)(uintptr_t)copy;
#ifndef _WIN32
    cm_set_name(obj(A0)->cm, copy);
#endif
}
static void os_GetCurrentThread(Ctx *c) {
    if (!t_thread && !rd64((uint64_t)(uintptr_t)g_main_thread + 0x50) && g_main_stack_base)      /* the main thread's ThreadType, once its stack is known (boot.c) */
        thread_type_fill((uint64_t)(uintptr_t)g_main_thread, 0, 0, g_main_stack_base, g_main_stack_size, OS_DEFAULT_PRIORITY);
    RET(t_thread ? t_thread : (uint64_t)g_main_thread);
}
static void os_GetThreadPriority(Ctx *c) { RET((int64_t)thread_obj(A0 == (uint64_t)(uintptr_t)g_main_thread ? 0 : A0)->priority); }
static void os_ChangeThreadPriority(Ctx *c) {
    Obj *o = obj(A0);
    int old = o->priority;
    oslog("  change priority of %s: %d -> %d (called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?", old, (int)A1, (unsigned long long)c->x[30]);
    o->priority = (int)A1;
#ifndef _WIN32
    cm_set_priority(o->cm, o->priority);
#endif
    if (o->thread) SetThreadPriority(o->thread, host_priority(o->priority));
    else if (A0 == (uint64_t)(uintptr_t)g_main_thread || A0 == t_thread) SetThreadPriority(GetCurrentThread(), host_priority(o->priority));
    RET((int64_t)old);
}
static void os_GetThreadId(Ctx *c) { Obj *o = obj(A0); RET(o->thread ? GetThreadId(o->thread) : GetCurrentThreadId()); }
#ifdef _WIN32
static void os_YieldThread(Ctx *c) { (void)c; SwitchToThread(); }
#else
static void os_YieldThread(Ctx *c) { (void)c; if (console_cores()) cm_yield(); else SwitchToThread(); }
#endif
static void os_SleepThread(Ctx *c) { sleep_ns((int64_t)A0); }
static void os_GetCurrentStackInfo(Ctx *c) {
    Obj *o = t_thread ? obj(t_thread) : NULL;
    if (A0) wr64(A0, o ? o->stack : g_main_stack_base);
    if (A1) wr64(A1, o ? o->stack_size : g_main_stack_size);
}
/* Core numbers and affinity masks. The console runs the application on cores 0..2 (GetThreadAvailableCoreMask = 7);
 * a thread is created on one core (the NPDM default core 0, or the one given to CreateThread) and stays there until
 * SetThreadCoreMask widens its mask. The values are kept per thread and returned as set; the Windows scheduler is
 * not told to pin anything (a PC has more than three cores and the game's threads would pile up on host CPU 0). */
static Obj *thread_obj(uint64_t key) {
    Obj *o = obj(key ? key : (uint64_t)(uintptr_t)g_main_thread);
    if (!o->core_set) { o->core_set = 1; o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; if (!o->priority) o->priority = OS_DEFAULT_PRIORITY; }
    return o;
}
static void os_SetThreadCoreMask(Ctx *c) {            /* (ThreadType*, int idealCore, Bit64 affinityMask) */
    Obj *o = thread_obj(A0);
    int ideal = (int)A1;
    uint64_t mask = A2;
    oslog("  core mask of %s: ideal %d mask %llx (was ideal %d mask %llx; %s; called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread || !A0 ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?",
          ideal, (unsigned long long)mask, o->ideal_core, (unsigned long long)o->core_mask, o->thread ? "running" : "not started", (unsigned long long)c->x[30]);
    int is_self = t_thread ? A0 == t_thread : (!A0 || A0 == (uint64_t)(uintptr_t)g_main_thread);      /* the calling thread names itself */
    HANDLE h = o->thread ? o->thread : is_self ? GetCurrentThread() : NULL;                             /* NULL: not started yet (applied at StartThread) */
    if (ideal == -2) { o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; }   /* IdealCoreUseDefaultValue: mask is ignored */
    else if (ideal == -3) o->core_mask = mask;                                                        /* IdealCoreNoUpdate */
    else {
        o->core_mask = mask;
        if (ideal == -1) { if (!(mask >> (o->ideal_core & 63) & 1)) o->ideal_core = mask ? (int)__builtin_ctzll(mask) : OS_DEFAULT_CORE; }   /* IdealCoreDontCare */
        else o->ideal_core = ideal;
    }
    apply_sched(h, o->core_mask);
#ifndef _WIN32
    cm_set_mask(o->cm, o->core_mask);               /* a running thread moves at its next safe point */
#endif
}
static void os_GetThreadCoreMask(Ctx *c) {            /* (int *outIdealCore, Bit64 *outMask, const ThreadType*) */
    Obj *o = thread_obj(A2);
    if (A0) wr32(A0, (uint32_t)o->ideal_core);
    if (A1) wr64(A1, o->core_mask);
}
static void os_GetCurrentCoreNumber(Ctx *c) {
#ifndef _WIN32
    if (console_cores() && cm_current_core() >= 0) { RET(cm_current_core()); return; }   /* the core whose token it holds */
#endif
    if (console_cores()) {                                /* the core the thread is on now, as the console answers */
        DWORD_PTR here = (DWORD_PTR)1 << GetCurrentProcessorNumber();
        for (int i = 0; i < 3; i++) if (g_cc_core[i] == here) { RET(i); return; }
    }
    RET((int64_t)thread_obj(t_thread)->ideal_core);
}
static void os_AvailableCoreMask(Ctx *c) { RET(7); }
static void nothing(Ctx *c) { (void)c; }

/* thread-local storage slots */
static volatile LONG g_tls_next;
/* (TlsSlot *out, void (*destructor)(uintptr_t value)) -> Result. The destructor of a slot runs when a thread ends
 * with a non-zero value in it; FreeTlsSlot makes the slot available again. */
static void os_AllocateTlsSlot(Ctx *c) {
    (void)g_tls_next;
    for (int i = 0; i < 64; i++)
        if (InterlockedCompareExchange(&g_tls_used[i], 1, 0) == 0) { g_tls_dtor[i] = A1; wr32(A0, (uint32_t)i); RET(0); return; }
    guest_trap("nn::os::AllocateTlsSlot: all 64 slots are in use (the console has 32)", c->x[30] - 4);
}
static void os_FreeTlsSlot(Ctx *c) { uint32_t i = (uint32_t)A0 & 63; g_tls_dtor[i] = 0; InterlockedExchange(&g_tls_used[i], 0); }
static void tls_run_destructors(Ctx *c) {
    for (int i = 0; i < 64; i++) {
        uint64_t v = t_tls[i], fn = g_tls_dtor[i];
        t_tls[i] = 0;
        if (v && fn && g_tls_used[i]) { c->x[0] = v; call_guest(c, fn); }
    }
}
static void os_GetTlsValue(Ctx *c) { RET(t_tls[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { t_tls[A0 & 63] = A1; }

/* ---------------------------------------------------------------- mutexes */
static void mtx_init(Ctx *c) { (void)obj(A0); }
static void mtx_lock(Ctx *c) { EnterCriticalSection(&obj(A0)->cs); }
static void mtx_unlock(Ctx *c) { LeaveCriticalSection(&obj(A0)->cs); }
static void mtx_trylock(Ctx *c) { RET(TryEnterCriticalSection(&obj(A0)->cs) ? 1 : 0); }
/* hitch.c (SMO_HITCH): host thread id that owns the guest mutex `key` now, 0 = nobody. Diagnostics only. */
DWORD os_mutex_owner(uint64_t key) { return key ? (DWORD)(uintptr_t)obj(key)->cs.OwningThread : 0; }

/* ---------------------------------------------------------------- events (Event, LightEvent, SystemEvent) */
/* nn::os::EventClearMode: 0 = ManualClear, 1 = AutoClear */
static void ev_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->signaled = (int)(A1 & 1); o->autoclear = A2 == 1; }
static void ev_signal(Ctx *c) {
    Obj *o = obj(A0);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
}
/* Host-side access to guest events (used for the display's vsync event). */
void os_init_event(uint64_t key, int autoclear) { obj_reset(key); Obj *o = obj(key); o->signaled = 0; o->autoclear = autoclear; }
void os_signal_event(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
}
void os_clear_event(uint64_t key) { Obj *o = obj(key); EnterCriticalSection(&o->cs); o->signaled = 0; LeaveCriticalSection(&o->cs); }
static void ev_clear(Ctx *c) { Obj *o = obj(A0); EnterCriticalSection(&o->cs); o->signaled = 0; LeaveCriticalSection(&o->cs); }
static int ev_wait_ms(Obj *o, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (!o->signaled) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && o->autoclear) o->signaled = 0;
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void ev_wait(Ctx *c) { ev_wait_ms(obj(A0), INFINITE); }
static void ev_trywait(Ctx *c) { RET(ev_wait_ms(obj(A0), 0)); }
static void ev_timedwait(Ctx *c) { RET(ev_wait_ms(obj(A0), ns_to_ms((int64_t)A1))); }

/* ---- system events shared through kernel handles.
 * [doc] A SystemEvent is a kernel event: CreateSystemEvent makes it with a readable and a writable handle; another
 * SystemEventType (in this or another process) can be attached to those handles, and a signal through one is seen by
 * every waiter on the event. SMO's audio server (lifted SDK code) creates the renderer's event and the client attaches
 * the game's SystemEvent to its readable handle. Model: handles name host event objects (g_handles); an attached
 * SystemEventType resolves to the event it shares (Obj.alias, followed by obj()). Each SystemEventType keeps ITS OWN
 * clear mode for its waits (AutoClear = a successful wait clears the shared event), as on the console. */
#define MAX_HANDLES 1024
static Obj *g_handles[MAX_HANDLES];
static LONG g_handle_next = 1;
static uint32_t handle_new(Obj *o) {
    LONG i = InterlockedIncrement(&g_handle_next) - 1;
    if (i >= MAX_HANDLES) guest_trap("os: too many system event handles (os_threads.c MAX_HANDLES)", 0);
    g_handles[i] = o;
    return 0x50000u + (uint32_t)i;
}
static Obj *handle_obj(uint32_t h) { uint32_t i = h - 0x50000u; return i < MAX_HANDLES ? g_handles[i] : NULL; }
static int sev_wait(uint64_t key, DWORD ms) {
    Obj *raw = obj_raw(key), *o = obj(key);
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (!o->signaled) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && raw->autoclear) o->signaled = 0;
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void sev_create(Ctx *c) {               /* (SystemEventType*, EventClearMode, bool interprocess) -> Result */
    Obj *raw = obj_raw(A0);
    raw->alias = NULL;
    obj_reset(A0);
    raw->autoclear = A1 == 1;
    raw->rhandle = handle_new(raw);
    raw->whandle = handle_new(raw);
    RET(0);
}
static void sev_attach_readable(Ctx *c) {      /* (SystemEventType*, Handle, bool managed, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) guest_trap("os: AttachReadableHandleToSystemEvent with a handle no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = A3 == 1;
}
/* Host side: a new system event with a handle (as a system module would create one and pass its readable handle);
 * returns the handle, *key receives a host key to signal it with os_signal_event. */
uint32_t os_system_event_new(int autoclear, uint64_t *key) {
    uint64_t k = (uint64_t)(uintptr_t)calloc(1, 64);            /* a unique key no guest object has */
    obj_reset(k);
    Obj *o = obj_raw(k);
    o->autoclear = autoclear;
    o->rhandle = handle_new(o);
    o->whandle = handle_new(o);
    if (key) *key = k;
    return o->rhandle;
}
static void sev_get_readable(Ctx *c) { Obj *o = obj(A0); if (!o->rhandle) o->rhandle = handle_new(o); RET(o->rhandle); }
static void sev_destroy(Ctx *c) { obj_raw(A0)->alias = NULL; }
static void sev_wait_h(Ctx *c) { sev_wait(A0, INFINITE); }
static void sev_trywait_h(Ctx *c) { RET(sev_wait(A0, 0)); }
static void sev_timedwait_h(Ctx *c) { RET(sev_wait(A0, ns_to_ms((int64_t)A1))); }

/* ---------------------------------------------------------------- condition variables */
static void cv_wait(Ctx *c) { SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, INFINITE); }
static void cv_timedwait(Ctx *c) {                    /* returns ConditionVariableStatus: 0 timeout, 1 signalled */
    RET(SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, ns_to_ms((int64_t)A2)) ? 1 : 0);
}
static void cv_signal(Ctx *c) { WakeConditionVariable(&obj(A0)->cv); }
static void cv_broadcast(Ctx *c) { WakeAllConditionVariable(&obj(A0)->cv); }

/* ---------------------------------------------------------------- message queues (of 64-bit values) */
static void mq_init(Ctx *c) {                         /* (queue, buffer, count): host-side storage is used */
    obj_reset(A0);
    Obj *o = obj(A0);
    free(o->buf);
    o->cap = A2 ? A2 : 1;
    o->buf = calloc((size_t)o->cap, 8);
}
static int mq_put(Obj *o, uint64_t v, int front, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == o->cap) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len < o->cap; break; }
    }
    if (ok) {
        if (front) { o->head = (o->head + o->cap - 1) % o->cap; o->buf[o->head] = v; }
        else o->buf[(o->head + o->len) % o->cap] = v;
        o->len++;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static int mq_get(Obj *o, uint64_t out, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == 0) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ok) {
        wr64(out, o->buf[o->head]);
        o->head = (o->head + 1) % o->cap;
        o->len--;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void mq_send(Ctx *c) { mq_put(obj(A0), A1, 0, INFINITE); }
static void mq_trysend(Ctx *c) { RET(mq_put(obj(A0), A1, 0, 0)); }
static void mq_jam(Ctx *c) { mq_put(obj(A0), A1, 1, INFINITE); }
static void mq_tryjam(Ctx *c) { RET(mq_put(obj(A0), A1, 1, 0)); }
static void mq_receive(Ctx *c) { mq_get(obj(A1), A0, INFINITE); }
static void mq_tryreceive(Ctx *c) { RET(mq_get(obj(A1), A0, 0)); }

/* ---------------------------------------------------------------- fibers: the game makes none (nn::os::InitializeFiber is
 * not one of its imports), so no fiber is ever running */
static void fib_current(Ctx *c) { RET(0); }

/* ---------------------------------------------------------------- misc */
static void os_ConvertToTick(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * (double)TICK_HZ / 1e9)); }
static void os_ConvertToTimeSpan(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * 1e9 / (double)TICK_HZ)); }
/* The console fills the buffer from a generator seeded with kernel entropy: unpredictable bytes. Same here, from the
 * system generator (RtlGenRandom). */
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
/* Test switch SMO_FIXED_SEED=<n>: the bytes come from a splitmix64 stream seeded with n instead, so
 * that a scripted run sees the same random choices in every run. Each call is logged (first 16). */
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) {
    static int mode = -1;
    static volatile LONG lock, calls;
    static uint64_t state;
    if (mode < 0) { const char *e = getenv("SMO_FIXED_SEED"); if (e && *e) { state = strtoull(e, NULL, 0) * 0x9E3779B97F4A7C15ull + 1; mode = 1; } else mode = 0; }
    if (!mode) return 0;
    while (InterlockedExchange(&lock, 1)) YieldProcessor();
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        memcpy((uint8_t *)buffer + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
    LONG n = ++calls;
    InterlockedExchange(&lock, 0);
    if (n <= 16) printf("fixed seed: %s call %ld, %llu bytes, caller %llx\n", who, (long)n, (unsigned long long)size, (unsigned long long)lr);
    return 1;
}
static void os_GenerateRandomBytes(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    if (host_fixed_random(p, A1, "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    for (uint64_t done = 0; done < A1;) {
        ULONG n = A1 - done > 0x10000000 ? 0x10000000 : (ULONG)(A1 - done);
        if (!SystemFunction036(p + done, n)) guest_trap("nn::os::GenerateRandomBytes: the system random generator failed", c->x[30] - 4);
        done += n;
    }
}

#define OS(n) "_ZN2nn2os" n
static const ImportDef thread_imports[] = {
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), os_CreateThread},
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"), os_CreateThreadOnCore},
    {OS("20GetCurrentCoreNumberEv"), os_GetCurrentCoreNumber},
    {OS("11StartThreadEPNS0_10ThreadTypeE"), os_StartThread}, {OS("10WaitThreadEPNS0_10ThreadTypeE"), os_WaitThread},
    {OS("13DestroyThreadEPNS0_10ThreadTypeE"), os_DestroyThread}, {OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"), os_SetThreadName},
    {OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"), os_SetThreadCoreMask},
    {OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"), os_GetThreadCoreMask},   /* not an import of the game: nnosGetThreadCoreMask reaches it (nnmisc.c FWD) */
    {OS("26GetThreadAvailableCoreMaskEv"), os_AvailableCoreMask},
    {OS("16GetCurrentThreadEv"), os_GetCurrentThread}, {OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi"), os_ChangeThreadPriority}, {OS("11GetThreadIdEPKNS0_10ThreadTypeE"), os_GetThreadId},
    {OS("11YieldThreadEv"), os_YieldThread}, {OS("11SleepThreadENS_8TimeSpanE"), os_SleepThread},
    {OS("19GetCurrentStackInfoEPmS1_"), os_GetCurrentStackInfo},
    {OS("15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot},
    {OS("11FreeTlsSlotENS0_7TlsSlotE"), os_FreeTlsSlot}, {OS("11GetTlsValueENS0_7TlsSlotE"), os_GetTlsValue}, {OS("11SetTlsValueENS0_7TlsSlotEm"), os_SetTlsValue},
    {OS("15InitializeMutexEPNS0_9MutexTypeEbi"), mtx_init}, {OS("13FinalizeMutexEPNS0_9MutexTypeE"), nothing},
    {OS("9LockMutexEPNS0_9MutexTypeE"), mtx_lock}, {OS("11UnlockMutexEPNS0_9MutexTypeE"), mtx_unlock}, {OS("12TryLockMutexEPNS0_9MutexTypeE"), mtx_trylock},
    {OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("13FinalizeEventEPNS0_9EventTypeE"), nothing},
    {OS("11SignalEventEPNS0_9EventTypeE"), ev_signal}, {OS("10ClearEventEPNS0_9EventTypeE"), ev_clear}, {OS("9WaitEventEPNS0_9EventTypeE"), ev_wait},
    {OS("20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("18FinalizeLightEventEPNS0_14LightEventTypeE"), nothing},
    {OS("16SignalLightEventEPNS0_14LightEventTypeE"), ev_signal}, {OS("15ClearLightEventEPNS0_14LightEventTypeE"), ev_clear},
    {OS("14WaitLightEventEPNS0_14LightEventTypeE"), ev_wait},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h}, {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"), mtx_init}, {OS("25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE"), nothing},
    {OS("21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"), cv_wait},
    {OS("26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"), cv_timedwait},
    {OS("23SignalConditionVariableEPNS0_21ConditionVariableTypeE"), cv_signal}, {OS("26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"), cv_broadcast},
    {OS("22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"), mq_init}, {OS("20FinalizeMessageQueueEPNS0_16MessageQueueTypeE"), nothing},
    {OS("16SendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_send}, {OS("19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_trysend},
    {OS("15JamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_jam}, {OS("18TryJamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_tryjam},
    {OS("19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_receive}, {OS("22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_tryreceive},
    {OS("15GetCurrentFiberEv"), fib_current},
    {OS("13ConvertToTickENS_8TimeSpanE"), os_ConvertToTick}, {OS("17ConvertToTimeSpanENS0_4TickE"), os_ConvertToTimeSpan},
    {OS("19GenerateRandomBytesEPvm"), os_GenerateRandomBytes},
};

/* SDK 3.5.1 forms the game imports. SetThreadNamePointer(ThreadType*, const char*): [doc] the thread's name is
 * the given string (kept by pointer); recorded for the thread reports. */
static void os_SetThreadNamePointer(Ctx *c) { obj(A0)->name = A1; }
/* GetThreadNamePointer(thread): the name set for it; a thread never named gets "Thread_0x<address>" as nn::os names it
 * (kept, so the pointer stays valid). Read by sead::nnsdk::PrintThreadInformation. */
static void os_GetThreadNamePointer(Ctx *c) {
    Obj *o = obj(A0);
    if (!o->name) { char *s = malloc(32); if (s) { snprintf(s, 32, "Thread_0x%llx", (unsigned long long)A0); o->name = (uint64_t)(uintptr_t)s; } }
    RET(o->name);
}
static const ImportDef smo_thread_imports[] = {
    {OS("19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20SetThreadNamePointerEPNS0_10ThreadTypeEPKc"), os_SetThreadNamePointer},
    {"nnosSetThreadNamePointer", os_SetThreadNamePointer},
    /* TryWaitEvent: the movie player (MovieDecoderPlayer::update
     * polls its decoder's buffer events with it) and the sound library's loaders (nn::atk ...Loader::IsInUse, Finalize). */
    {OS("12TryWaitEventEPNS0_9EventTypeE"), ev_trywait},
    {OS("20GetThreadNamePointerEPKNS0_10ThreadTypeE"), os_GetThreadNamePointer},
};
/* the system-event entry points, also for lifted SDK callers (sdklift_rules.txt cuts them; installed below with
 * sdk_host_override). Create, AttachReadableHandle, Signal, Clear and GetReadableHandle are not imports of the game:
 * only the lifted SDK code calls them (the audio renderer's events). */
static const ImportDef sev_all[] = {
    {OS("17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), sev_create},
    {OS("33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"), sev_attach_readable},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("17SignalSystemEventEPNS0_15SystemEventTypeE"), ev_signal},
    {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"), sev_get_readable},
};
void sdk_host_override(const char *name, void (*fn)(Ctx *));   /* sdklift_rt.c */
void register_thread_imports(void) {
    register_imports(thread_imports, (int)(sizeof thread_imports / sizeof thread_imports[0]));
    register_imports(sev_all, (int)(sizeof sev_all / sizeof sev_all[0]));
    register_imports(smo_thread_imports, (int)(sizeof smo_thread_imports / sizeof smo_thread_imports[0]));
    for (int i = 0; i < (int)(sizeof sev_all / sizeof sev_all[0]); i++) sdk_host_override(sev_all[i].name, sev_all[i].fn);
}
int guest_thread_count(void) { return (int)g_thread_count; }
