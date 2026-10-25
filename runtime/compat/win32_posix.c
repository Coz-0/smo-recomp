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
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <copyfile.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <pwd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioServices.h>
#include <CoreGraphics/CoreGraphics.h>

static void win_path(char *p);

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

#define MAX_WIN 8
static struct { uintptr_t base; size_t size; } g_win[MAX_WIN];
static int g_nwin;
int recomp_reserve_window(uint64_t base, uint64_t size) {
    if (g_nwin == MAX_WIN) return 0;
    size = page_up(size);
    void *p = mmap((void *)(uintptr_t)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 0;
    if (p != (void *)(uintptr_t)base) { munmap(p, size); return 0; }
    g_win[g_nwin].base = (uintptr_t)base; g_win[g_nwin].size = size; g_nwin++;
    return 1;
}
static int in_window(uintptr_t a, size_t n) {
    for (int i = 0; i < g_nwin; i++) if (a >= g_win[i].base && a + n <= g_win[i].base + g_win[i].size) return 1;
    return 0;
}
static int res_overlaps(uintptr_t a, size_t n) {
    for (int i = 0; i < g_nres; i++) if (a < g_res[i].base + g_res[i].size && g_res[i].base < a + n) return 1;
    return 0;
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
    int fixed = 0;
    if (addr && in_window((uintptr_t)addr, size)) {         /* ours: map over the reservation unless already given out */
        os_unfair_lock_lock(&g_res_lock);
        int taken = res_overlaps((uintptr_t)addr, size);
        os_unfair_lock_unlock(&g_res_lock);
        if (taken) { SetLastError(487); return NULL; }        /* ERROR_INVALID_ADDRESS, as Windows says */
        fixed = MAP_FIXED;
    }
    void *p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | fixed, -1, 0);
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
        if (n && in_window((uintptr_t)addr, n))                 /* back to the reservation */
            return mmap(addr, n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) != MAP_FAILED;
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
    if ((DWORD)(uintptr_t)__atomic_load_n(&cs->OwningThread, __ATOMIC_RELAXED) == me) { cs->depth++; return; }
    os_unfair_lock_lock(&cs->l);
    __atomic_store_n(&cs->OwningThread, (HANDLE)(uintptr_t)me, __ATOMIC_RELAXED);
    cs->depth = 1;
}
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) {
    DWORD me = GetCurrentThreadId();
    if ((DWORD)(uintptr_t)__atomic_load_n(&cs->OwningThread, __ATOMIC_RELAXED) == me) { cs->depth++; return TRUE; }
    if (!os_unfair_lock_trylock(&cs->l)) return FALSE;
    __atomic_store_n(&cs->OwningThread, (HANDLE)(uintptr_t)me, __ATOMIC_RELAXED);
    cs->depth = 1;
    return TRUE;
}
void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (--cs->depth) return;
    __atomic_store_n(&cs->OwningThread, (HANDLE)0, __ATOMIC_RELAXED);
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
/* every handle starts with its kind */
typedef struct { uint32_t kind; int fd; } FileObj;
enum { H_FILE = 0x46494c45, H_FIND = 0x46494e44, H_MUTEX = 0x4d555458, H_PROCESS = 0x50524f43 };
typedef struct { uint32_t kind; int name_fd, own_fd, owned; } MutexObj;
typedef struct { uint32_t kind; pid_t pid; } ProcObj;
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
static DWORD mutex_wait(MutexObj *o, DWORD ms);
static DWORD process_wait(ProcObj *o, DWORD ms);
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (h && h != PSEUDO_THREAD && h != PSEUDO_PROCESS) {
        uint32_t kind = *(const uint32_t *)h;
        if (kind == H_MUTEX) return mutex_wait(h, ms);
        if (kind == H_PROCESS) return process_wait(h, ms);
    }
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
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD || h == PSEUDO_PROCESS) return TRUE;
    switch (*(const uint32_t *)h) {
    case H_FILE: close(((FileObj *)h)->fd); free(h); return TRUE;
    case H_MUTEX: { MutexObj *m = h; if (m->name_fd >= 0) close(m->name_fd); if (m->own_fd >= 0) close(m->own_fd); free(m); return TRUE; }
    case H_PROCESS: free(h); return TRUE;
    case H_FIND: return FindClose(h);
    default: obj_release(h); return TRUE;
    }
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
    if (!realpath(di.dli_fname, out) || strlen(out) + 1 > size) snprintf(out, size, "%s", di.dli_fname);
    win_path(out);
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

/* ---------------------------------------------------------------- C runtime, strings */
int recomp_putenv(const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq) return -1;
    char name[256];
    size_t n = (size_t)(eq - kv) < sizeof name - 1 ? (size_t)(eq - kv) : sizeof name - 1;
    memcpy(name, kv, n); name[n] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}
int _write(int fd, const void *p, unsigned n) { return (int)write(fd, p, n); }
int _fileno(FILE *f) { return fileno(f); }

/* UTF-8 <-> UTF-32 (WCHAR is wchar_t, 32 bits). n / wn of -1: up to and with the terminating zero. As on Windows the
 * return value counts the zero when it was part of the input, and a NULL or 0-sized output asks for the size. */
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, WCHAR *w, int wn) {
    (void)cp; (void)flags;
    const uint8_t *p = (const uint8_t *)s, *e = n < 0 ? NULL : p + n;
    int out = 0;
    for (;;) {
        if (e ? p >= e : 0) break;
        uint32_t c = *p, len = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 1;
        if (len > 1) {
            c &= 0x3f >> (len - 1);
            for (uint32_t k = 1; k < len; k++) {
                if ((e && p + k >= e) || (p[k] & 0xc0) != 0x80) { c = 0xfffd; len = k; break; }
                c = c << 6 | (p[k] & 0x3f);
            }
        } else if (c >= 0x80) c = 0xfffd;
        if (w && wn) { if (out >= wn) { SetLastError(122); return 0; } w[out] = (WCHAR)c; }
        out++;
        p += len;
        if (!e && c == 0) break;
    }
    return out;
}
int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *w, int wn, char *s, int n, const char *def, BOOL *used) {
    (void)cp; (void)flags; (void)def;
    if (used) *used = FALSE;
    int out = 0;
    for (int i = 0; wn < 0 || i < wn; i++) {
        uint32_t c = (uint32_t)w[i];
        uint8_t b[4];
        int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (c >= 0x110000) { c = 0xfffd; len = 3; }
        if (len == 1) b[0] = (uint8_t)c;
        else {
            for (int k = len - 1; k > 0; k--) { b[k] = (uint8_t)(0x80 | (c & 0x3f)); c >>= 6; }
            b[0] = (uint8_t)((0xf00 >> len) | c);
        }
        if (s && n) { if (out + len > n) { SetLastError(122); return 0; } memcpy(s + out, b, (size_t)len); }
        out += len;
        if (wn < 0 && w[i] == 0) break;
    }
    return out;
}

BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID arg, PVOID *ctx) {
    for (;;) {
        int st = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (st == 2) return TRUE;
        if (st == 0 && __sync_bool_compare_and_swap(&once->state, 0, 1)) {
            BOOL ok = fn(once, arg, ctx);
            __atomic_store_n(&once->state, ok ? 2 : 0, __ATOMIC_RELEASE);
            return ok;
        }
        sched_yield();
    }
}

/* ---------------------------------------------------------------- last error */
static _Thread_local DWORD t_last_error;
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }
static DWORD win_error_of(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: case EISDIR: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case ENOSPC: return ERROR_DISK_FULL;
    case EBUSY: case ETXTBSY: return ERROR_SHARING_VIOLATION;
    default: return ERROR_INVALID_PARAMETER;
    }
}
static BOOL fail_errno(void) { SetLastError(win_error_of(errno)); return FALSE; }

/* ---------------------------------------------------------------- files */
void recomp_path(char *out, size_t n, const char *in) {
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++) out[i] = in[i] == '\\' ? '/' : in[i];
    out[i] = 0;
}
static void win_path(char *p) { for (; *p; p++) if (*p == '/') *p = '\\'; }      /* a path given to the runtime */
char *_fullpath(char *out, const char *path, size_t n) {
    char in[4096], buf[4096];
    recomp_path(in, sizeof in, path);
    if (in[0] == '/') snprintf(buf, sizeof buf, "%s", in);
    else { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) return NULL; snprintf(buf, sizeof buf, "%s/%s", cwd, in); }
    char *parts[512], *save = NULL;
    int np = 0;
    for (char *t = strtok_r(buf, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np < 512) parts[np++] = t;
    }
    char res[4096] = "";
    size_t len = 0;
    for (int i = 0; i < np; i++) len += (size_t)snprintf(res + len, sizeof res - len, "/%s", parts[i]);
    if (!np) snprintf(res, sizeof res, "/");
    if (!out) out = malloc(strlen(res) + 1), n = strlen(res) + 1;
    if (strlen(res) + 1 > n) return NULL;
    memcpy(out, res, strlen(res) + 1);
    win_path(out);
    return out;
}
static int access_ok(const char *p) { return access(p, F_OK) == 0; }
#undef fopen
FILE *recomp_fopen(const char *path, const char *mode) {
    char p[4096];
    recomp_path(p, sizeof p, path);
    return fopen(p, mode);
}


HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD attrs, HANDLE tmpl) {
    (void)share; (void)sa; (void)attrs; (void)tmpl;
    char p[4096];
    recomp_path(p, sizeof p, path);
    int fl = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: fl |= O_CREAT; break;
    case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
    default: break;
    }
    int existed = disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS ? access_ok(p) : 0;
    int fd = open(p, fl | O_CLOEXEC, 0644);
    if (fd < 0) { fail_errno(); return INVALID_HANDLE_VALUE; }
    FileObj *o = calloc(1, sizeof *o);
    o->kind = H_FILE; o->fd = fd;
    SetLastError(existed ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return o;
}
static FileObj *file_of(HANDLE h) { FileObj *o = h; return o && h != INVALID_HANDLE_VALUE && o->kind == H_FILE ? o : NULL; }
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, void *overlapped) {
    (void)overlapped;
    FileObj *o = file_of(h);
    if (got) *got = 0;
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(o->fd, (uint8_t *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return fail_errno(); }
        if (r == 0) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, void *overlapped) {
    (void)overlapped;
    FileObj *o = file_of(h);
    if (put) *put = 0;
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(o->fd, (const uint8_t *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; return fail_errno(); }
        done += (size_t)r;
    }
    if (put) *put = (DWORD)done;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD method) {
    FileObj *o = file_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    off_t r = lseek(o->fd, (off_t)to.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (now) now->QuadPart = (LONGLONG)r;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    FileObj *o = file_of(h);
    struct stat st;
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (fstat(o->fd, &st)) return fail_errno();
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    FileObj *o = file_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    off_t at = lseek(o->fd, 0, SEEK_CUR);
    return at >= 0 && !ftruncate(o->fd, at) ? TRUE : fail_errno();
}
/* F_FULLFSYNC: the data on the disk itself, which is what FlushFileBuffers promises (fsync stops at the drive's cache). */
BOOL FlushFileBuffers(HANDLE h) {
    FileObj *o = file_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return !fcntl(o->fd, F_FULLFSYNC) || !fsync(o->fd) ? TRUE : fail_errno();
}
intptr_t _get_osfhandle(int fd) {
    static _Thread_local FileObj wrap[4];     /* a few at once, as FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f))) needs */
    static _Thread_local int next;
    FileObj *o = &wrap[next++ & 3];
    o->kind = H_FILE; o->fd = fd;
    return (intptr_t)o;
}
#define PATH_ARG(var, in) char var[4096]; recomp_path(var, sizeof var, (in))
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa) { (void)sa; PATH_ARG(p, path); return !mkdir(p, 0755) ? TRUE : fail_errno(); }
BOOL RemoveDirectoryA(LPCSTR path) { PATH_ARG(p, path); return !rmdir(p) ? TRUE : fail_errno(); }
BOOL DeleteFileA(LPCSTR path) { PATH_ARG(p, path); return !unlink(p) ? TRUE : fail_errno(); }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    PATH_ARG(a, from); PATH_ARG(b, to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && access_ok(b)) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    if (rename(a, b)) return fail_errno();
    if (flags & MOVEFILE_WRITE_THROUGH) {      /* the rename itself on disk: sync the folder that holds it */
        char dir[4096];
        snprintf(dir, sizeof dir, "%s", b);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
        int fd = open(dir, O_RDONLY);
        if (fd >= 0) { fcntl(fd, F_FULLFSYNC); close(fd); }
    }
    return TRUE;
}
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    PATH_ARG(a, from); PATH_ARG(b, to);
    return !copyfile(a, b, NULL, COPYFILE_DATA | (fail_if_exists ? COPYFILE_EXCL : 0)) ? TRUE : fail_errno();
}
static DWORD attrs_of(const struct stat *st, const char *name) {
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    const char *base = strrchr(name, '/');
    if ((base ? base[1] : name[0]) == '.') a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}
DWORD GetFileAttributesA(LPCSTR path) {
    PATH_ARG(p, path);
    struct stat st;
    if (stat(p, &st)) { fail_errno(); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st, p);
}
BOOL SetFileAttributesA(LPCSTR path, DWORD attrs) {
    PATH_ARG(p, path);
    struct stat st;
    if (stat(p, &st)) return fail_errno();
    mode_t m = attrs & FILE_ATTRIBUTE_READONLY ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return !chmod(p, m) ? TRUE : fail_errno();
}
static void ft_of_timespec(FILETIME *f, struct timespec t) {
    uint64_t v = ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100;
    f->dwLowDateTime = (DWORD)v; f->dwHighDateTime = (DWORD)(v >> 32);
}
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, void *out) {
    (void)level;
    PATH_ARG(p, path);
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA *a = out;
    if (stat(p, &st)) return fail_errno();
    a->dwFileAttributes = attrs_of(&st, p);
    ft_of_timespec(&a->ftCreationTime, st.st_birthtimespec);
    ft_of_timespec(&a->ftLastAccessTime, st.st_atimespec);
    ft_of_timespec(&a->ftLastWriteTime, st.st_mtimespec);
    a->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); a->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
DWORD GetTempPathA(DWORD n, LPSTR out) {
    const char *t = getenv("TMPDIR");
    char buf[4096];
    snprintf(buf, sizeof buf, "%s%s", t && t[0] ? t : "/tmp/", t && t[0] && t[strlen(t) - 1] != '/' ? "/" : "");
    win_path(buf);
    size_t len = strlen(buf);
    if (len + 1 > n) return (DWORD)(len + 1);
    memcpy(out, buf, len + 1);
    return (DWORD)len;
}
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file_part) {
    char buf[4096];
    if (!_fullpath(buf, path, sizeof buf)) return 0;
    size_t len = strlen(buf);
    if (len + 1 > n) return (DWORD)(len + 1);
    memcpy(out, buf, len + 1);
    if (file_part) { char *sl = strrchr(out, '\\'); *file_part = sl ? sl + 1 : out; }
    return (DWORD)len;
}
DWORD GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size) {
    char u[4096];
    if (!GetModuleFileNameA(m, u, sizeof u)) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)size);
    return n ? (DWORD)(n - 1) : 0;
}

/* FindFirstFile over opendir: the folder part of the pattern is listed, the last part matched with fnmatch. */
typedef struct { uint32_t kind; DIR *d; char dir[4096], pat[256]; } FindObj;
static int find_next(FindObj *o, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(o->d))) {
        if (fnmatch(o->pat, e->d_name, 0)) continue;
        char full[8192];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", o->dir, e->d_name);
        memset(fd, 0, sizeof *fd);
        if (!stat(full, &st)) {
            fd->dwFileAttributes = attrs_of(&st, e->d_name);
            ft_of_timespec(&fd->ftCreationTime, st.st_birthtimespec);
            ft_of_timespec(&fd->ftLastAccessTime, st.st_atimespec);
            ft_of_timespec(&fd->ftLastWriteTime, st.st_mtimespec);
            fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size;
        }
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return 1;
    }
    return 0;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    FindObj *o = calloc(1, sizeof *o);
    o->kind = H_FIND;
    recomp_path(o->dir, sizeof o->dir, pattern);
    char *sl = strrchr(o->dir, '/');
    if (sl) { snprintf(o->pat, sizeof o->pat, "%s", sl + 1); if (sl == o->dir) sl[1] = 0; else *sl = 0; }
    else { snprintf(o->pat, sizeof o->pat, "%s", o->dir); snprintf(o->dir, sizeof o->dir, "."); }
    if (!strcmp(o->pat, "*.*")) snprintf(o->pat, sizeof o->pat, "*");
    o->d = opendir(o->dir);
    if (!o->d) { fail_errno(); free(o); return INVALID_HANDLE_VALUE; }
    if (!find_next(o, fd)) { closedir(o->d); free(o); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return o;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    FindObj *o = h;
    if (!o || h == INVALID_HANDLE_VALUE || o->kind != H_FIND) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (find_next(o, fd)) return TRUE;
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
static void find_widen(WIN32_FIND_DATAW *w, const WIN32_FIND_DATAA *a) {
    memcpy(w, a, offsetof(WIN32_FIND_DATAW, cFileName));
    MultiByteToWideChar(CP_UTF8, 0, a->cFileName, -1, w->cFileName, MAX_PATH);
    w->cAlternateFileName[0] = 0;
}
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd) {
    char p[4096];
    WIN32_FIND_DATAA a;
    if (!WideCharToMultiByte(CP_UTF8, 0, pattern, -1, p, sizeof p, NULL, NULL)) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    HANDLE h = FindFirstFileA(p, &a);
    if (h != INVALID_HANDLE_VALUE) find_widen(fd, &a);
    return h;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) {
    WIN32_FIND_DATAA a;
    if (!FindNextFileA(h, &a)) return FALSE;
    find_widen(fd, &a);
    return TRUE;
}
BOOL FindClose(HANDLE h) {
    FindObj *o = h;
    if (!o || h == INVALID_HANDLE_VALUE || o->kind != H_FIND) return FALSE;
    closedir(o->d);
    free(o);
    return TRUE;
}

/* ---------------------------------------------------------------- named mutexes and processes
 * A named mutex is two lock files in $TMPDIR: "<name>.name", held shared by every open handle (a create that cannot
 * take it exclusively finds the name in use: ERROR_ALREADY_EXISTS), and "<name>.own", held exclusively by the owner. */
static int lock_file(const char *name, const char *ext) {
    char tmp[1024], path[2048], clean[512];
    size_t k = 0;
    for (const char *s = name; *s && k + 1 < sizeof clean; s++) clean[k++] = (*s == '/' || *s == '\\' || *s == ':') ? '_' : *s;
    clean[k] = 0;
    GetTempPathA(sizeof tmp, tmp);
    snprintf(path, sizeof path, "%ssmo-mutex-%s.%s", tmp, clean, ext);
    return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
}
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name) {
    (void)sa;
    MutexObj *o = calloc(1, sizeof *o);
    o->kind = H_MUTEX; o->name_fd = o->own_fd = -1;
    DWORD err = ERROR_SUCCESS;
    if (name) {
        o->name_fd = lock_file(name, "name");
        o->own_fd = lock_file(name, "own");
        if (o->name_fd < 0 || o->own_fd < 0) { fail_errno(); free(o); return NULL; }
        if (flock(o->name_fd, LOCK_EX | LOCK_NB)) err = ERROR_ALREADY_EXISTS;
        flock(o->name_fd, LOCK_SH);
    }
    if (owned && o->own_fd >= 0) { flock(o->own_fd, LOCK_EX); o->owned = 1; }
    SetLastError(err);
    return o;
}
BOOL ReleaseMutex(HANDLE h) {
    MutexObj *o = h;
    if (!o || o->kind != H_MUTEX || !o->owned) return FALSE;
    o->owned = 0;
    return o->own_fd < 0 || !flock(o->own_fd, LOCK_UN);
}
static DWORD mutex_wait(MutexObj *o, DWORD ms) {
    if (o->own_fd < 0) { o->owned = 1; return WAIT_OBJECT_0; }
    uint64_t end = ms == INFINITE ? UINT64_MAX : now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        if (!flock(o->own_fd, LOCK_EX | (ms == INFINITE ? 0 : LOCK_NB))) { o->owned = 1; return WAIT_OBJECT_0; }
        if (now_ns() >= end) return WAIT_TIMEOUT;
        Sleep(1);
    }
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (kill((pid_t)pid, 0) && errno == ESRCH) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    ProcObj *o = calloc(1, sizeof *o);
    o->kind = H_PROCESS; o->pid = (pid_t)pid;
    return o;
}
static DWORD process_wait(ProcObj *o, DWORD ms) {
    uint64_t end = ms == INFINITE ? UINT64_MAX : now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        if (kill(o->pid, 0) && errno == ESRCH) return WAIT_OBJECT_0;
        if (now_ns() >= end) return WAIT_TIMEOUT;
        Sleep(5);
    }
}
BOOL GetUserNameW(LPWSTR out, DWORD *n) {
    struct passwd *pw = getpwuid(getuid());
    const char *name = pw && pw->pw_name ? pw->pw_name : getenv("USER");
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    int need = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    if ((DWORD)need > *n) { *n = (DWORD)need; SetLastError(122); return FALSE; }
    *n = (DWORD)MultiByteToWideChar(CP_UTF8, 0, name, -1, out, (int)*n);
    return TRUE;
}
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) { return !(value ? setenv(name, value, 1) : unsetenv(name)); }
DWORD GetCurrentProcessorNumber(void) {
    size_t n = 0;
    return pthread_cpu_number_np(&n) ? 0 : (DWORD)n;
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    DWORD_PTR m = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    if (proc) *proc = m;
    if (sys) *sys = m;
    return TRUE;
}
BOOL SetPriorityClass(HANDLE p, DWORD cls) { (void)p; return !setpriority(PRIO_PROCESS, 0, cls == ABOVE_NORMAL_PRIORITY_CLASS || cls == HIGH_PRIORITY_CLASS ? -5 : 0) || TRUE; }
BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable) { (void)h; (void)disable; return TRUE; }

/* ---------------------------------------------------------------- memory probes */
BOOL IsBadReadPtr(const void *p, uintptr_t n) {
    uint8_t b;
    if (!n) return FALSE;
    return !host_try_read(p, &b, 1) || !host_try_read((const uint8_t *)p + n - 1, &b, 1);
}
BOOL IsBadWritePtr(void *p, uintptr_t n) {
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = (uintptr_t)p, e = (uintptr_t)p + (n ? n : 1); a < e; ) {
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return TRUE;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- time */
static void systemtime_of(SYSTEMTIME *t, const struct tm *tm, long ns) {
    t->wYear = (WORD)(tm->tm_year + 1900); t->wMonth = (WORD)(tm->tm_mon + 1); t->wDayOfWeek = (WORD)tm->tm_wday;
    t->wDay = (WORD)tm->tm_mday; t->wHour = (WORD)tm->tm_hour; t->wMinute = (WORD)tm->tm_min; t->wSecond = (WORD)tm->tm_sec;
    t->wMilliseconds = (WORD)(ns / 1000000);
}
void GetSystemTime(SYSTEMTIME *t) { struct timespec ts; struct tm tm; clock_gettime(CLOCK_REALTIME, &ts); gmtime_r(&ts.tv_sec, &tm); systemtime_of(t, &tm, ts.tv_nsec); }
void GetLocalTime(SYSTEMTIME *t) { struct timespec ts; struct tm tm; clock_gettime(CLOCK_REALTIME, &ts); localtime_r(&ts.tv_sec, &tm); systemtime_of(t, &tm, ts.tv_nsec); }
void GetSystemTimeAsFileTime(FILETIME *t) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ft_of_timespec(t, ts); }

BOOLEAN SystemFunction036(PVOID buf, ULONG n) { arc4random_buf(buf, n); return 1; }

/* ---------------------------------------------------------------- dynamic libraries */
HMODULE LoadLibraryA(LPCSTR name) {
    size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".dll")) { SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
    void *h = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    if (!h) SetLastError(ERROR_FILE_NOT_FOUND);
    return h;
}
HMODULE LoadLibraryW(LPCWSTR name) {
    char u[4096];
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL)) return NULL;
    return LoadLibraryA(u);
}
/* m is a dlopen handle (LoadLibrary) or an image base (GetModuleHandle, GetModuleHandleEx): the latter is reopened by
 * its path, which finds the image already loaded. */
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    Dl_info di;
    void *h = m;
    if (m && dladdr(m, &di) && di.dli_fbase == m && di.dli_fname) h = dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = RTLD_DEFAULT;
    return (FARPROC)dlsym(h, name);
}
BOOL FreeLibrary(HMODULE m) { return m && !dlclose(m); }

/* ---------------------------------------------------------------- the keyboard */
int recomp_mac_keycode(int vk) {
    /* kVK_ANSI_A.. of HIToolbox/Events.h: the keys of the US layout's positions */
    static const signed char letters[26] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                            0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};
    static const signed char digits[10] = {0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};
    static const signed char numpad[10] = {0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C};
    static const signed char fkeys[12] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return numpad[vk - VK_NUMPAD0];
    if (vk >= VK_F1 && vk < VK_F1 + 12) return fkeys[vk - VK_F1];
    switch (vk) {
    case VK_BACK: return 0x33; case VK_TAB: return 0x30; case VK_RETURN: return 0x24; case VK_SHIFT: return 0x38;
    case VK_CONTROL: return 0x3B; case VK_MENU: return 0x3A; case VK_ESCAPE: return 0x35; case VK_SPACE: return 0x31;
    case VK_LEFT: return 0x7B; case VK_UP: return 0x7E; case VK_RIGHT: return 0x7C; case VK_DOWN: return 0x7D;
    default: return -1;
    }
}
static int (*g_key_source)(int vk);
void recomp_set_key_source(int (*down)(int vk)) { g_key_source = down; }
SHORT GetAsyncKeyState(int vk) {
    if (g_key_source) return g_key_source(vk) ? (SHORT)0x8000 : 0;
    int k = recomp_mac_keycode(vk);
    if (k < 0) return 0;
    int down = CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, (CGKeyCode)k);
    if (!down && (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU))   /* the right-hand modifier too, as Windows' generic code */
        down = CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, (CGKeyCode)(vk == VK_SHIFT ? 0x3C : vk == VK_CONTROL ? 0x3E : 0x3D));
    return down ? (SHORT)0x8000 : 0;
}

/* ---------------------------------------------------------------- message boxes */
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type) {
    (void)owner;
    CFStringRef t = CFStringCreateWithCString(NULL, text ? text : "", kCFStringEncodingUTF8);
    CFStringRef h = CFStringCreateWithCString(NULL, caption ? caption : "", kCFStringEncodingUTF8);
    CFOptionFlags level = (type & 0xf0) == MB_ICONERROR ? kCFUserNotificationStopAlertLevel
                        : (type & 0xf0) == MB_ICONWARNING ? kCFUserNotificationCautionAlertLevel
                        : (type & 0xf0) == MB_ICONINFORMATION ? kCFUserNotificationNoteAlertLevel : kCFUserNotificationPlainAlertLevel;
    int yesno = (type & 0xf) == MB_YESNO, okcancel = (type & 0xf) == MB_OKCANCEL;
    CFOptionFlags answer = kCFUserNotificationDefaultResponse;
    SInt32 r = CFUserNotificationDisplayAlert(0, level, NULL, NULL, NULL, h, t, yesno ? CFSTR("Yes") : CFSTR("OK"),
                                              yesno ? CFSTR("No") : okcancel ? CFSTR("Cancel") : NULL, NULL, &answer);
    CFRelease(t); CFRelease(h);
    int first = r != 0 || (answer & 3) == kCFUserNotificationDefaultResponse;
    return yesno ? (first ? IDYES : IDNO) : okcancel ? (first ? IDOK : IDCANCEL) : IDOK;
}
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type) {
    char t[4096], h[512];
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof t, NULL, NULL)) t[0] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, caption, -1, h, sizeof h, NULL, NULL)) h[0] = 0;
    return MessageBoxA(owner, t, h, type);
}
BOOL MessageBeep(UINT type) { (void)type; AudioServicesPlayAlertSound(kSystemSoundID_UserPreferredAlert); return TRUE; }

/* ---------------------------------------------------------------- __try / __except (1) */
_Thread_local RecompTry *recomp_try_top;
void recomp_try_push(RecompTry *t) { recomp_try_install(); t->prev = recomp_try_top; recomp_try_top = t; }
void recomp_try_pop(RecompTry *t) { if (recomp_try_top == t) recomp_try_top = t->prev; }
void recomp_try_catch(void) {
    RecompTry *t = recomp_try_top;
    if (!t) return;
    recomp_try_top = t->prev;                     /* a fault in the __except block goes to the block around this one */
    siglongjmp(t->jb, 1);
}
static void try_signal(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    recomp_try_catch();
    signal(sig, SIG_DFL);                         /* not in a block: the fault as it would be without this handler */
}
void recomp_try_install(void) {
    static volatile int done;
    if (done || __atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST)) return;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = try_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s = SIGBUS; s <= SIGSEGV; s += SIGSEGV - SIGBUS) {       /* keep a handler installed before (hostrt.c's) */
        sigaction(s, NULL, &old);
        if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL) sigaction(s, &sa, NULL);
    }
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
__attribute__((noinline)) int host_backtrace_here(uint64_t *pcs, int max) {
    uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0), rec[2];
    if (!host_try_read((const void *)(uintptr_t)fp, rec, sizeof rec)) return 0;
    return host_frames_from(rec[1] & 0x0000ffffffffffffull, 0, rec[0], pcs, max);
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
