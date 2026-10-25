/* The Win32 subset of win32_posix.h on macOS: Mach for memory, threads and clocks, os_unfair_lock and
 * os_sync_wait_on_address for locks and condition variables, pthreads for threads. */
#ifndef _WIN32
#include "win32_posix.h"

#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <os/os_sync_wait_on_address.h>

/* ---------------------------------------------------------------- C runtime */
int __argc;
char **__argv;
__attribute__((constructor)) static void args_init(void) { __argc = *_NSGetArgc(); __argv = *_NSGetArgv(); }

/* The alignment and the size are stored in front of the block, so _aligned_realloc can copy. */
typedef struct { void *raw; size_t size; } AlignedHead;
void *_aligned_malloc(size_t size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    uint8_t *raw = malloc(size + align + sizeof(AlignedHead));
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(AlignedHead) + align - 1) & ~(uintptr_t)(align - 1);
    AlignedHead *h = (AlignedHead *)p - 1;
    h->raw = raw; h->size = size;
    return (void *)p;
}
void _aligned_free(void *p) { if (p) free(((AlignedHead *)p - 1)->raw); }
void *_aligned_realloc(void *p, size_t size, size_t align) {
    if (!p) return _aligned_malloc(size, align);
    if (!size) { _aligned_free(p); return NULL; }
    void *q = _aligned_malloc(size, align);
    if (!q) return NULL;
    size_t old = ((AlignedHead *)p - 1)->size;
    memcpy(q, p, old < size ? old : size);
    _aligned_free(p);
    return q;
}

/* ---------------------------------------------------------------- virtual memory */
size_t recomp_page_size(void) { return (size_t)vm_page_size; }
static uintptr_t page_down(uintptr_t a) { return a & ~(uintptr_t)(vm_page_size - 1); }
static uintptr_t page_up(uintptr_t a) { return (a + vm_page_size - 1) & ~(uintptr_t)(vm_page_size - 1); }

static int prot_of(DWORD p) {
    switch (p & 0xff) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
    }
}
static DWORD page_of(vm_prot_t p) {
    if (p & VM_PROT_EXECUTE) return p & VM_PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (p & VM_PROT_WRITE) return PAGE_READWRITE;
    if (p & VM_PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* Reservations, so MEM_RELEASE (size 0) knows the size and VirtualQuery the allocation base. */
#define MAX_RES 4096
static struct { uintptr_t base; size_t size; DWORD protect; } g_res[MAX_RES];
static int g_nres;
static os_unfair_lock g_res_lock = OS_UNFAIR_LOCK_INIT;
static int res_find(uintptr_t a) {
    for (int i = 0; i < g_nres; i++) if (a - g_res[i].base < g_res[i].size) return i;
    return -1;
}

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if (!size) return NULL;
    if (!(type & MEM_RESERVE)) {                     /* commit inside a reservation */
        if (!addr) return NULL;
        uintptr_t lo = page_down((uintptr_t)addr), hi = page_up((uintptr_t)addr + size);
        if (mprotect((void *)lo, hi - lo, prot_of(protect))) return NULL;
        return (void *)lo;
    }
    size = page_up(size);
    int prot = type & MEM_COMMIT ? prot_of(protect) : PROT_NONE;
    void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (addr && p != addr) { munmap(p, size); return NULL; }      /* a fixed address that is taken fails, as on Windows */
    os_unfair_lock_lock(&g_res_lock);
    if (g_nres < MAX_RES) { g_res[g_nres].base = (uintptr_t)p; g_res[g_nres].size = size; g_res[g_nres].protect = protect; g_nres++; }
    os_unfair_lock_unlock(&g_res_lock);
    return p;
}

BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        os_unfair_lock_lock(&g_res_lock);
        int i = res_find((uintptr_t)addr);
        size_t n = i >= 0 ? g_res[i].size : 0;
        if (i >= 0) g_res[i] = g_res[--g_nres];
        os_unfair_lock_unlock(&g_res_lock);
        return n && !munmap(addr, n);
    }
    if (type & MEM_DECOMMIT) {                       /* back to reserved; the pages read zero when committed again */
        uintptr_t lo = page_down((uintptr_t)addr), hi = page_up((uintptr_t)addr + size);
        return mmap((void *)lo, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) != MAP_FAILED;
    }
    return FALSE;
}

SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    mach_vm_address_t a = page_down((uintptr_t)addr);
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    mbi->BaseAddress = (void *)page_down((uintptr_t)addr);
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj);
    if (kr != KERN_SUCCESS || a > (uintptr_t)mbi->BaseAddress) {      /* not mapped: free up to the next region */
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        mbi->RegionSize = kr == KERN_SUCCESS ? (SIZE_T)(a - (uintptr_t)mbi->BaseAddress) : vm_page_size;
        return sizeof *mbi;
    }
    mbi->RegionSize = (SIZE_T)(a + sz - (uintptr_t)mbi->BaseAddress);
    mbi->State = info.protection == VM_PROT_NONE ? MEM_RESERVE : MEM_COMMIT;
    mbi->Protect = info.protection == VM_PROT_NONE ? 0 : page_of(info.protection);
    mbi->Type = MEM_PRIVATE;
    os_unfair_lock_lock(&g_res_lock);
    int i = res_find((uintptr_t)addr);
    mbi->AllocationBase = i >= 0 ? (void *)g_res[i].base : (void *)(uintptr_t)a;
    mbi->AllocationProtect = i >= 0 ? g_res[i].protect : page_of(info.max_protection);
    os_unfair_lock_unlock(&g_res_lock);
    return sizeof *mbi;
}

/* Every host page the range touches, as on Windows. With 16 KB pages that can be more than the caller meant: a
 * caller that must not touch the page past its range rounds its size down to recomp_page_size() itself. */
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old) {
    MEMORY_BASIC_INFORMATION mi;
    if (old) *old = VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_COMMIT ? mi.Protect : PAGE_NOACCESS;
    uintptr_t lo = page_down((uintptr_t)addr), hi = page_up((uintptr_t)addr + size);
    return !mprotect((void *)lo, hi - lo, prot_of(protect));
}

int host_try_read(const void *p, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)p, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

/* ---------------------------------------------------------------- locks and condition variables */
void InitializeSRWLock(SRWLOCK *l) { l->l = OS_UNFAIR_LOCK_INIT; }
void AcquireSRWLockExclusive(SRWLOCK *l) { os_unfair_lock_lock(&l->l); }
void ReleaseSRWLockExclusive(SRWLOCK *l) { os_unfair_lock_unlock(&l->l); }
void AcquireSRWLockShared(SRWLOCK *l) { os_unfair_lock_lock(&l->l); }
void ReleaseSRWLockShared(SRWLOCK *l) { os_unfair_lock_unlock(&l->l); }
BOOL TryAcquireSRWLockExclusive(SRWLOCK *l) { return os_unfair_lock_trylock(&l->l); }

void InitializeCriticalSection(CRITICAL_SECTION *cs) { memset(cs, 0, sizeof *cs); }
void DeleteCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
void EnterCriticalSection(CRITICAL_SECTION *cs) {
    DWORD me = GetCurrentThreadId();
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) == me) { cs->depth++; return; }
    os_unfair_lock_lock(&cs->l);
    __atomic_store_n(&cs->owner, me, __ATOMIC_RELAXED);
    cs->depth = 1;
}
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) {
    DWORD me = GetCurrentThreadId();
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) == me) { cs->depth++; return TRUE; }
    if (!os_unfair_lock_trylock(&cs->l)) return FALSE;
    __atomic_store_n(&cs->owner, me, __ATOMIC_RELAXED);
    cs->depth = 1;
    return TRUE;
}
void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (--cs->depth) return;
    __atomic_store_n(&cs->owner, 0, __ATOMIC_RELAXED);
    os_unfair_lock_unlock(&cs->l);
}

/* A condition variable is a sequence number: a sleeper reads it before unlocking and waits while it is unchanged, a
 * wake increments it. A wake between the unlock and the wait changes the number, so the wait returns at once. */
void InitializeConditionVariable(CONDITION_VARIABLE *cv) { cv->seq = 0; }
static int cv_wait(CONDITION_VARIABLE *cv, uint64_t seen, DWORD ms) {
    int r;
    if (ms == INFINITE) r = os_sync_wait_on_address((void *)&cv->seq, seen, 8, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    else r = os_sync_wait_on_address_with_timeout((void *)&cv->seq, seen, 8, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                  (uint64_t)ms * 1000000ull);             /* relative, in nanoseconds */
    return r < 0 && errno == ETIMEDOUT ? 0 : 1;
}
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) {
    (void)flags;
    uint64_t seen = __atomic_load_n(&cv->seq, __ATOMIC_ACQUIRE);
    ReleaseSRWLockExclusive(l);
    int ok = cv_wait(cv, seen, ms);
    AcquireSRWLockExclusive(l);
    return ok;
}
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) {
    uint64_t seen = __atomic_load_n(&cv->seq, __ATOMIC_ACQUIRE);
    int depth = cs->depth;
    cs->depth = 1;
    LeaveCriticalSection(cs);
    int ok = cv_wait(cv, seen, ms);
    EnterCriticalSection(cs);
    cs->depth = depth;
    return ok;
}
void WakeConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_RELEASE);
    os_sync_wake_by_address_any((void *)&cv->seq, 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}
void WakeAllConditionVariable(CONDITION_VARIABLE *cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_RELEASE);
    os_sync_wake_by_address_all((void *)&cv->seq, 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

/* ---------------------------------------------------------------- clocks */
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
ULONGLONG GetTickCount64(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000ull; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *t) { t->QuadPart = (LONGLONG)now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return TRUE; }
void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000l};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
BOOL SwitchToThread(void) { sched_yield(); return TRUE; }

/* ---------------------------------------------------------------- handles: threads and events */
enum { H_THREAD = 0x54485244, H_EVENT = 0x45564e54 };
typedef struct {
    uint32_t kind;
    volatile int refs;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signalled, manual;                   /* thread: finished; event: set */
    pthread_t thread;
    mach_port_t port;
    int gate;                                /* CREATE_SUSPENDED: 1 until the first ResumeThread */
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
} Obj;

static Obj *obj_new(uint32_t kind) {
    Obj *o = calloc(1, sizeof *o);
    o->kind = kind;
    o->refs = 1;
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->cv, NULL);
    return o;
}
static void obj_release(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    pthread_mutex_destroy(&o->m);
    pthread_cond_destroy(&o->cv);
    free(o);
}

static _Thread_local Obj *t_self;            /* the handle of this thread, made on first need for threads not made here */
static Obj *self_obj(void) {
    if (!t_self) {
        t_self = obj_new(H_THREAD);
        t_self->thread = pthread_self();
        t_self->port = pthread_mach_thread_np(pthread_self());
    }
    return t_self;
}

static void *thread_start(void *p) {
    Obj *o = p;
    t_self = o;
    pthread_mutex_lock(&o->m);
    while (o->gate) pthread_cond_wait(&o->cv, &o->m);
    pthread_mutex_unlock(&o->m);
    o->fn(o->arg);
    pthread_mutex_lock(&o->m);
    o->signalled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    obj_release(o);                          /* the thread's own reference */
    return NULL;
}

HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid) {
    (void)sec;
    Obj *o = obj_new(H_THREAD);
    o->refs = 2;
    o->fn = fn; o->arg = arg;
    o->gate = (flags & CREATE_SUSPENDED) != 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&at, page_up(stack));
    int r = pthread_create(&o->thread, &at, thread_start, o);
    pthread_attr_destroy(&at);
    if (r) { free(o); return NULL; }
    o->port = pthread_mach_thread_np(o->thread);
    if (tid) *tid = o->port;
    return o;
}

#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)
#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
static Obj *obj_of(HANDLE h) { return h == PSEUDO_THREAD ? self_obj() : (Obj *)h; }
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD options) {
    (void)sp; (void)tp; (void)access; (void)inherit; (void)options;
    if (src == PSEUDO_PROCESS) { *dst = PSEUDO_PROCESS; return TRUE; }
    Obj *o = obj_of(src);
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    *dst = o;
    return TRUE;
}
DWORD GetCurrentThreadId(void) { return t_self ? t_self->port : pthread_mach_thread_np(pthread_self()); }
DWORD GetThreadId(HANDLE h) { Obj *o = obj_of(h); return o && o->kind == H_THREAD ? o->port : 0; }

DWORD SuspendThread(HANDLE h) {
    Obj *o = obj_of(h);
    return thread_suspend(o->port) == KERN_SUCCESS ? 0 : (DWORD)-1;
}
DWORD ResumeThread(HANDLE h) {
    Obj *o = obj_of(h);
    pthread_mutex_lock(&o->m);
    int gated = o->gate;
    if (gated) { o->gate = 0; pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->m);
    if (gated) return 1;
    return thread_resume(o->port) == KERN_SUCCESS ? 1 : (DWORD)-1;
}
BOOL SetThreadPriority(HANDLE h, int priority) {
    Obj *o = obj_of(h);
    struct sched_param sp;
    int policy;
    if (pthread_getschedparam(o->thread, &policy, &sp)) return FALSE;
    int lo = sched_get_priority_min(policy), hi = sched_get_priority_max(policy), mid = (lo + hi) / 2;
    sp.sched_priority = priority >= THREAD_PRIORITY_TIME_CRITICAL ? hi : mid + priority * (hi - mid) / 4;
    return !pthread_setschedparam(o->thread, policy, &sp);
}
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) { (void)h; (void)mask; return 1; }
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *t) {
    Obj *o = obj_of(h);
    thread_basic_info_data_t bi;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(o->port, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS) return FALSE;
    *t = ((ULONG64)bi.user_time.seconds + (ULONG64)bi.system_time.seconds) * 1000000000ull +
         ((ULONG64)bi.user_time.microseconds + (ULONG64)bi.system_time.microseconds) * 1000ull;
    return TRUE;
}

HANDLE CreateEventA(void *sec, BOOL manual_reset, BOOL initial, LPCSTR name) {
    (void)sec; (void)name;
    Obj *o = obj_new(H_EVENT);
    o->manual = manual_reset;
    o->signalled = initial;
    return o;
}
BOOL SetEvent(HANDLE h) {
    Obj *o = h;
    pthread_mutex_lock(&o->m);
    o->signalled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    Obj *o = h;
    pthread_mutex_lock(&o->m);
    o->signalled = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}

static void deadline_of(DWORD ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);      /* pthread_cond_timedwait's clock on macOS */
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000l;
    if (ts->tv_nsec >= 1000000000l) { ts->tv_sec++; ts->tv_nsec -= 1000000000l; }
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    Obj *o = obj_of(h);
    if (!o || (o->kind != H_THREAD && o->kind != H_EVENT)) return WAIT_FAILED;
    struct timespec ts;
    if (ms != INFINITE) deadline_of(ms, &ts);
    pthread_mutex_lock(&o->m);
    int r = 0;
    while (!o->signalled && r != ETIMEDOUT) r = ms == INFINITE ? pthread_cond_wait(&o->cv, &o->m) : pthread_cond_timedwait(&o->cv, &o->m, &ts);
    int got = o->signalled;
    if (got && o->kind == H_EVENT && !o->manual) o->signalled = 0;
    pthread_mutex_unlock(&o->m);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
/* Polls every millisecond: the runtime calls it rarely, and never with more than a few handles. */
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    uint64_t end = ms == INFINITE ? UINT64_MAX : now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        DWORD done = 0;
        for (DWORD i = 0; i < n; i++) {
            if (WaitForSingleObject(hs[i], 0) != WAIT_OBJECT_0) continue;
            if (!all) return WAIT_OBJECT_0 + i;
            done++;
        }
        if (all && done == n) return WAIT_OBJECT_0;
        if (now_ns() >= end) return WAIT_TIMEOUT;
        Sleep(1);
    }
}
BOOL CloseHandle(HANDLE h) {
    if (!h || h == PSEUDO_THREAD || h == PSEUDO_PROCESS) return TRUE;
    obj_release(h);
    return TRUE;
}
void ExitProcess(UINT code) { fflush(NULL); exit((int)code); }

/* ---------------------------------------------------------------- process and modules */
HMODULE GetModuleHandleA(LPCSTR name) {
    if (name) return NULL;
    return (HMODULE)_dyld_get_image_header(0);
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR addr, HMODULE *m) {
    Dl_info di;
    *m = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) { *m = GetModuleHandleA(addr); return *m != NULL; }
    if (!dladdr(addr, &di) || !di.dli_fbase) return FALSE;
    *m = di.dli_fbase;
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size) {
    Dl_info di;
    if (!size) return 0;
    if (!m) m = GetModuleHandleA(NULL);
    if (!dladdr(m, &di) || !di.dli_fname) { out[0] = 0; return 0; }
    snprintf(out, size, "%s", di.dli_fname);
    return (DWORD)strlen(out);
}

static void ft_set(FILETIME *f, uint64_t hundred_ns) { f->dwLowDateTime = (DWORD)hundred_ns; f->dwHighDateTime = (DWORD)(hundred_ns >> 32); }
BOOL GetSystemTimes(FILETIME *idle, FILETIME *kernel, FILETIME *user) {
    host_cpu_load_info_data_t li;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&li, &n) != KERN_SUCCESS) return FALSE;
    uint64_t hz = (uint64_t)sysconf(_SC_CLK_TCK), k = 10000000ull / (hz ? hz : 100);   /* ticks -> 100 ns */
    uint64_t t_idle = li.cpu_ticks[CPU_STATE_IDLE] * k, t_sys = li.cpu_ticks[CPU_STATE_SYSTEM] * k;
    uint64_t t_user = (li.cpu_ticks[CPU_STATE_USER] + li.cpu_ticks[CPU_STATE_NICE]) * k;
    if (idle) ft_set(idle, t_idle);
    if (kernel) ft_set(kernel, t_sys + t_idle);
    if (user) ft_set(user, t_user);
    return TRUE;
}
BOOL GetProcessTimes(HANDLE p, FILETIME *creation, FILETIME *exit_, FILETIME *kernel, FILETIME *user) {
    (void)p;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) return FALSE;
    if (creation) ft_set(creation, 0);
    if (exit_) ft_set(exit_, 0);
    if (kernel) ft_set(kernel, (uint64_t)ru.ru_stime.tv_sec * 10000000ull + (uint64_t)ru.ru_stime.tv_usec * 10ull);
    if (user) ft_set(user, (uint64_t)ru.ru_utime.tv_sec * 10000000ull + (uint64_t)ru.ru_utime.tv_usec * 10ull);
    return TRUE;
}
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb) {
    (void)p; (void)cb;
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return FALSE;
    task_events_info_data_t ev;
    mach_msg_type_number_t ne = TASK_EVENTS_INFO_COUNT;
    pmc->PageFaultCount = task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev, &ne) == KERN_SUCCESS ? (DWORD)ev.faults : 0;
    pmc->WorkingSetSize = (SIZE_T)vi.resident_size;
    pmc->PeakWorkingSetSize = (SIZE_T)vi.resident_size_peak;
    pmc->PagefileUsage = pmc->PrivateUsage = (SIZE_T)vi.phys_footprint;
    return TRUE;
}

/* ---------------------------------------------------------------- frames
 * Every arm64 macOS binary keeps frame pointers (the platform ABI requires them): [fp] is the caller's fp, [fp + 8]
 * the return address. Reads go through host_try_read, so a broken chain ends the walk instead of faulting. */
int host_frames_from(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t *pcs, int max) {
    int n = 0;
    if (n < max) pcs[n++] = pc;
    if (n < max && lr) pcs[n++] = lr;          /* a leaf function has not stored lr yet; a duplicate costs nothing */
    while (n < max && fp && !(fp & 15)) {
        uint64_t rec[2];
        if (!host_try_read((const void *)(uintptr_t)fp, rec, sizeof rec) || !rec[1]) break;
        uint64_t ret = rec[1] & 0x0000ffffffffffffull;     /* without the pointer-authentication bits */
        if (ret != pcs[n - 1]) pcs[n++] = ret;
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    return n;
}
int host_thread_backtrace(HANDLE h, uint64_t *pcs, int max) {
    Obj *o = obj_of(h);
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_suspend(o->port) != KERN_SUCCESS) return 0;
    int got = 0;
    if (thread_get_state(o->port, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS)
        got = host_frames_from(arm_thread_state64_get_pc(st), arm_thread_state64_get_lr(st), arm_thread_state64_get_fp(st), pcs, max);
    thread_resume(o->port);
    return got;
}
#endif
