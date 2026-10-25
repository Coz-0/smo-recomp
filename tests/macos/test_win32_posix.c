/* Checks runtime/compat/win32_posix.c against the Windows meaning the runtime relies on. Run by check_core.sh. */
#include <stdio.h>
#include <stdint.h>
#include "win32_posix.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fixed guest ranges hostrt.c and sdklift_rt.c map */
static void test_guest_ranges(void) {
    static const uint64_t bases[] = {0x7100000000ull, 0x7300000000ull, 0x7400000000ull, 0x7500000000ull};
    for (int i = 0; i < 4; i++) {
        void *p = VirtualAlloc((void *)(uintptr_t)bases[i], 0x6000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(p == (void *)(uintptr_t)bases[i], "map at 0x%llx gave %p", (unsigned long long)bases[i], p);
    }
    void *again = VirtualAlloc((void *)(uintptr_t)bases[0], 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(again == NULL, "a second map at a taken address must fail, got %p", again);
    uint8_t *g = (uint8_t *)(uintptr_t)bases[0];
    CHECK(g[0x5fffff0] == 0, "fresh pages read zero");
    g[0x123] = 0x5a;
    size_t pg = recomp_page_size();
    CHECK(pg == 0x4000, "host page is 16 KB on Apple silicon (got 0x%zx)", pg);
    /* the image protection of hostrt.c: read-only up to a 4 KB boundary, rounded down to the host page */
    uint64_t ro_end = 0x4f16000ull & ~(uint64_t)(pg - 1);
    DWORD old = 0;
    CHECK(VirtualProtect(g, ro_end, PAGE_READONLY, &old), "protect the image's text");
    CHECK(old == PAGE_READWRITE, "old protection was read-write (got 0x%x)", old);
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery(g + 0x1000, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READONLY, "query text: state 0x%x protect 0x%x", mi.State, mi.Protect);
    CHECK(VirtualQuery(g + 0x4f16000, &mi, sizeof mi) && mi.Protect == PAGE_READWRITE, "the data segment stays writable (protect 0x%x)", mi.Protect);
    g[0x4f16000] = 1;
    CHECK(mi.AllocationBase == g, "allocation base is the reservation (got %p)", mi.AllocationBase);
}

static void test_reserve_commit_query(void) {
    uint8_t *r = VirtualAlloc(NULL, 0x40000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(r != NULL, "reserve");
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery(r, &mi, sizeof mi);
    CHECK(mi.State == MEM_RESERVE, "reserved state (got 0x%x)", mi.State);
    uint8_t *c = VirtualAlloc(r + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == r + 0x10000, "commit inside the reservation");
    c[5] = 7;
    VirtualQuery(r + 0x10000, &mi, sizeof mi);
    CHECK(mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && mi.RegionSize == 0x10000, "committed: state 0x%x protect 0x%x size 0x%zx", mi.State, mi.Protect, mi.RegionSize);
    uint8_t b = 0;
    CHECK(host_try_read(c + 5, &b, 1) && b == 7, "guarded read of mapped memory");
    CHECK(!host_try_read(r, &b, 1), "guarded read of reserved memory fails");
    CHECK(!host_try_read((void *)(uintptr_t)0x10, &b, 1), "guarded read of page zero fails");
    CHECK(VirtualFree(c, 0x10000, MEM_DECOMMIT), "decommit");
    VirtualQuery(c, &mi, sizeof mi);
    CHECK(mi.State == MEM_RESERVE, "decommitted is reserved again (got 0x%x)", mi.State);
    VirtualAlloc(c, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c[5] == 0, "recommitted pages read zero");
    CHECK(VirtualFree(r, 0, MEM_RELEASE), "release");
    VirtualQuery(r, &mi, sizeof mi);
    CHECK(mi.State == MEM_FREE, "released is free (got 0x%x)", mi.State);
}

static void test_interlocked(void) {
    volatile LONG l = 5;
    volatile LONG64 q = 1;
    CHECK(InterlockedIncrement(&l) == 6, "increment returns the new value");
    CHECK(InterlockedExchange(&l, 9) == 6, "exchange returns the old value");
    CHECK(InterlockedCompareExchange(&l, 1, 9) == 9 && l == 1, "compare-exchange swaps on a match, returns the old value");
    CHECK(InterlockedCompareExchange(&l, 2, 9) == 1 && l == 1, "compare-exchange leaves a mismatch");
    CHECK(InterlockedIncrement64(&q) == 2 && InterlockedAdd64(&q, 10) == 12 && InterlockedExchangeAdd64(&q, 1) == 12, "64-bit add forms");
}

/* ---- locks, condition variables, threads, events */
static SRWLOCK g_srw;                       /* zero-filled, no initialiser: valid as on Windows */
static CONDITION_VARIABLE g_cv;
static volatile LONG g_counter, g_ready;
static CRITICAL_SECTION g_cs;

static DWORD WINAPI adder(LPVOID arg) {
    for (int i = 0; i < 100000; i++) { AcquireSRWLockExclusive(&g_srw); g_counter++; ReleaseSRWLockExclusive(&g_srw); }
    (void)arg;
    return 0;
}
static DWORD WINAPI waker(LPVOID arg) {
    Sleep(30);
    AcquireSRWLockExclusive(&g_srw);
    g_ready = 1;
    ReleaseSRWLockExclusive(&g_srw);
    WakeAllConditionVariable(&g_cv);
    (void)arg;
    return 0;
}
static DWORD WINAPI cs_user(LPVOID arg) {
    EnterCriticalSection(&g_cs);
    g_counter++;
    LeaveCriticalSection(&g_cs);
    (void)arg;
    return 0;
}
static DWORD WINAPI spinner(LPVOID arg) { volatile int *stop = arg; while (!*stop) { } return 0; }

static void test_threads(void) {
    HANDLE t[4];
    g_counter = 0;
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, adder, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(4, t, TRUE, 10000) == WAIT_OBJECT_0, "four threads finish");
    for (int i = 0; i < 4; i++) CloseHandle(t[i]);
    CHECK(g_counter == 400000, "SRW lock excludes (counter %d)", (int)g_counter);

    ULONGLONG t0 = GetTickCount64();
    AcquireSRWLockExclusive(&g_srw);
    BOOL woke = SleepConditionVariableSRW(&g_cv, &g_srw, 50, 0);
    ReleaseSRWLockExclusive(&g_srw);
    ULONGLONG dt = GetTickCount64() - t0;
    CHECK(!woke && dt >= 45 && dt < 500, "condition variable times out after 50 ms (woke %d after %llu ms)", woke, dt);

    HANDLE w = CreateThread(NULL, 0, waker, NULL, 0, NULL);
    AcquireSRWLockExclusive(&g_srw);
    while (!g_ready) if (!SleepConditionVariableSRW(&g_cv, &g_srw, 5000, 0)) break;
    ReleaseSRWLockExclusive(&g_srw);
    CHECK(g_ready, "condition variable is woken by another thread");
    CHECK(WaitForSingleObject(w, 1000) == WAIT_OBJECT_0, "waker finishes");
    CloseHandle(w);

    InitializeCriticalSection(&g_cs);
    g_counter = 0;
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);                 /* recursive, as on Windows */
    HANDLE c = CreateThread(NULL, 0, cs_user, NULL, CREATE_SUSPENDED, NULL);
    Sleep(20);
    CHECK(g_counter == 0, "a CREATE_SUSPENDED thread does not run");
    CHECK(ResumeThread(c) == 1, "ResumeThread releases it");
    Sleep(20);
    CHECK(g_counter == 0, "the critical section held twice keeps it out");
    LeaveCriticalSection(&g_cs);
    Sleep(20);
    CHECK(g_counter == 0, "still held once");
    LeaveCriticalSection(&g_cs);
    CHECK(WaitForSingleObject(c, 1000) == WAIT_OBJECT_0 && g_counter == 1, "enters once released");
    CloseHandle(c);

    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(WaitForSingleObject(e, 10) == WAIT_TIMEOUT, "unset event times out");
    SetEvent(e);
    CHECK(WaitForSingleObject(e, 0) == WAIT_OBJECT_0, "set event is signalled");
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT, "auto-reset event resets after one wait");
    CloseHandle(e);

    volatile int stop = 0;
    HANDLE s = CreateThread(NULL, (SIZE_T)1 << 30, spinner, (void *)&stop, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    CHECK(s != NULL, "a thread with a 1 GB stack reservation (boot.c's guest main thread)");
    Sleep(20);
    uint64_t pcs[16];
    int n = host_thread_backtrace(s, pcs, 16);
    HMODULE m = NULL;
    char name[MAX_PATH];
    CHECK(n >= 1 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(uintptr_t)pcs[0], &m) && m == GetModuleHandleA(NULL),
          "a sampled thread's pc is in the executable (%d frames)", n);
    CHECK(GetModuleFileNameA(m, name, sizeof name) && strstr(name, "test_win32_posix"), "module name (%s)", name);
    ULONG64 cyc = 0;
    CHECK(QueryThreadCycleTime(s, &cyc) && cyc > 5000000, "a spinning thread used CPU (%llu ns)", (unsigned long long)cyc);
    stop = 1;
    CHECK(WaitForSingleObject(s, 1000) == WAIT_OBJECT_0, "spinner finishes");
    CloseHandle(s);

    HANDLE self;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS) && GetThreadId(self) == GetCurrentThreadId(),
          "a duplicated pseudo handle names this thread");
    CloseHandle(self);
}

static void test_misc(void) {
    void *a = _aligned_malloc(100, 64);
    CHECK(a && ((uintptr_t)a & 63) == 0, "aligned allocation");
    memset(a, 3, 100);
    a = _aligned_realloc(a, 200, 64);
    CHECK(a && ((uintptr_t)a & 63) == 0 && ((uint8_t *)a)[99] == 3, "aligned reallocation keeps the contents");
    _aligned_free(a);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Sleep(20);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    CHECK(ms >= 19 && ms < 200, "Sleep(20) and the performance counter agree (%.1f ms)", ms);
    CHECK(__argc >= 1 && __argv && strstr(__argv[0], "test_win32_posix"), "__argc / __argv");
    FILETIME i0, k0, u0;
    PROCESS_MEMORY_COUNTERS pmc;
    CHECK(GetSystemTimes(&i0, &k0, &u0) && GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) && pmc.WorkingSetSize > 0, "system times and memory counters");
}

int main(void) {
    test_guest_ranges();
    test_reserve_commit_query();
    test_interlocked();
    test_threads();
    test_misc();
    printf("win32_posix: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail != 0;
}
