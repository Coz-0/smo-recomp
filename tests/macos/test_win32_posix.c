/* Checks runtime/compat/win32_posix.c against the Windows meaning the runtime relies on. Run by check_core.sh. */
#include <stdio.h>
#include <stdint.h>
#include "win32_posix.h"
#include <unistd.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fixed guest ranges hostrt.c and sdklift_rt.c map */
static void test_guest_ranges(void) {
    /* hostrt.h's macOS layout, inside the window hostrt.c reserves at load (0x7100000000.. on Windows collides with
     * macOS's malloc heap range in about half of all launches: measured, docs/macos-port.md) */
    static const uint64_t bases[] = {0x8100000000ull, 0x8300000000ull, 0x8400000000ull, 0x8500000000ull};
    CHECK(recomp_reserve_window(0x8100000000ull, 0x500000000ull), "reserve the guest window");
    for (int i = 0; i < 4; i++) {
        void *p = VirtualAlloc((void *)(uintptr_t)bases[i], 0x6000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(p == (void *)(uintptr_t)bases[i], "map at 0x%llx gave %p", (unsigned long long)bases[i], p);
    }
    void *again = VirtualAlloc((void *)(uintptr_t)bases[0], 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(again == NULL, "a second map at a taken address must fail, got %p", again);
    void *outside = VirtualAlloc((void *)(uintptr_t)0x9000000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(outside == (void *)(uintptr_t)0x9000000000ull, "a free fixed address outside the window maps too");
    void *tmp = VirtualAlloc((void *)(uintptr_t)0x8200000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(tmp && VirtualFree(tmp, 0, MEM_RELEASE), "map and release inside the window");
    MEMORY_BASIC_INFORMATION wi;
    CHECK(VirtualQuery(tmp, &wi, sizeof wi) && wi.State == MEM_RESERVE, "released inside the window: reserved again, not free (0x%x)", wi.State);
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

    HANDLE sem = CreateSemaphoreA(NULL, 1, 2, NULL);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 10) == WAIT_TIMEOUT, "a semaphore of 1 is taken once");
    CHECK(ReleaseSemaphore(sem, 2, NULL) && !ReleaseSemaphore(sem, 1, NULL) && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "released by 2 (its maximum), not beyond; taken twice");
    CloseHandle(sem);
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


/* ---- part 2: files, folders, strings, named mutexes, processes, loader */
__attribute__((visibility("default"))) int test_exported_symbol(void) { return 7; }
static const char *tmp_dir(void) { static char t[1024]; GetTempPathA(sizeof t, t); return t; }
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID arg, PVOID *ctx) { (void)o; (void)ctx; (*(int *)arg)++; return TRUE; }

static void test_files(void) {
    char tmp[1024], dir[1100], path[1200], path2[1200];
    GetTempPathA(sizeof tmp, tmp);
    CHECK(tmp[strlen(tmp) - 1] == '\\' && tmp[0] == '\\', "temp path is given Windows-style, with a final separator (%s)", tmp);
    snprintf(dir, sizeof dir, "%ssmo_shim_test_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CHECK(CreateDirectoryA(dir, NULL), "create folder");
    CHECK(!CreateDirectoryA(dir, NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "create an existing folder: ERROR_ALREADY_EXISTS (%u)", GetLastError());
    snprintf(path, sizeof path, "%s\\a.bin", dir);                  /* a Windows-style separator */
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "create a file through a backslash path");
    DWORD n = 0;
    CHECK(WriteFile(h, "hello world", 11, &n, NULL) && n == 11, "write");
    LARGE_INTEGER to = {.QuadPart = 6}, now, size;
    CHECK(SetFilePointerEx(h, to, &now, FILE_BEGIN) && now.QuadPart == 6, "seek");
    char buf[32] = {0};
    CHECK(ReadFile(h, buf, sizeof buf, &n, NULL) && n == 5 && !memcmp(buf, "world", 5), "read to the end gives the rest (%u bytes)", n);
    /* positional transfers through an OVERLAPPED: at its offset whatever the file pointer says (nnfs.c reads every
     * game file this way, from several threads on one handle), and the file pointer stands after the transfer */
    OVERLAPPED ov = {0};
    ov.Offset = 6;
    memset(buf, 0, sizeof buf);
    CHECK(ReadFile(h, buf, 3, &n, &ov) && n == 3 && !memcmp(buf, "wor", 3), "a read at OVERLAPPED offset 6 at the end of the file gives \"wor\" (%u bytes: %.3s)", n, buf);
    ov.Offset = 0;
    CHECK(ReadFile(h, buf, 5, &n, &ov) && n == 5 && !memcmp(buf, "hello", 5), "a read at OVERLAPPED offset 0 gives \"hello\"");
    LARGE_INTEGER zero = {.QuadPart = 0}, after;
    CHECK(SetFilePointerEx(h, zero, &after, FILE_CURRENT) && after.QuadPart == 5, "the file pointer stands after the positional read (%lld)", (long long)after.QuadPart);
    ov.Offset = 6;
    CHECK(WriteFile(h, "WORLD", 5, &n, &ov) && n == 5, "a write at OVERLAPPED offset 6");
    ov.Offset = 0;
    CHECK(ReadFile(h, buf, 11, &n, &ov) && n == 11 && !memcmp(buf, "hello WORLD", 11), "the write landed at its offset (%.11s)", buf);
    ov.Offset = 6;
    WriteFile(h, "world", 5, &n, &ov);                            /* back to "hello world" for the checks below */
    to.QuadPart = 5;
    SetFilePointerEx(h, to, NULL, FILE_BEGIN);
    CHECK(SetEndOfFile(h) && GetFileSizeEx(h, &size) && size.QuadPart == 5, "truncate at the file pointer");
    CHECK(FlushFileBuffers(h), "flush to disk");
    CloseHandle(h);
    FILE *f = fopen(path, "rb");
    CHECK(f && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "hello", 5), "fopen takes the backslash path too");
    if (f) { CHECK(FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f))), "FlushFileBuffers through _get_osfhandle"); fclose(f); }
    CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && !(GetFileAttributesA(path) & FILE_ATTRIBUTE_DIRECTORY), "file attributes");
    CHECK(GetFileAttributesA(dir) & FILE_ATTRIBUTE_DIRECTORY, "folder attributes");
    CHECK(GetFileAttributesA("/nonexistent/x") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND, "missing file: ERROR_FILE_NOT_FOUND");
    WIN32_FILE_ATTRIBUTE_DATA ad;
    CHECK(GetFileAttributesExA(path, GetFileExInfoStandard, &ad) && ad.nFileSizeLow == 5 && ad.ftLastWriteTime.dwHighDateTime > 0x01d00000u, "attributes with size and a 1601-based write time");
    snprintf(path2, sizeof path2, "%s\\b.bin", dir);
    CHECK(CopyFileA(path, path2, TRUE) && !CopyFileA(path, path2, TRUE), "copy, and refuse to replace when asked");
    CHECK(!MoveFileExA(path, path2, 0) && MoveFileExA(path, path2, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "move: replaces only when asked");
    snprintf(path, sizeof path, "%s\\c.txt", dir);
    CloseHandle(CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL));
    CHECK(CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE, "CREATE_NEW fails on an existing file");
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    int files = 0, dots = 0;
    HANDLE fh = FindFirstFileA(pat, &fd);
    CHECK(fh != INVALID_HANDLE_VALUE, "find in a folder");
    if (fh != INVALID_HANDLE_VALUE) { do { if (fd.cFileName[0] == '.') dots++; else files++; } while (FindNextFileA(fh, &fd)); FindClose(fh); }
    CHECK(files == 2 && dots == 2 && GetLastError() == ERROR_NO_MORE_FILES, "\"*\" lists the two files, . and .. (%d files, %d dots)", files, dots);
    snprintf(pat, sizeof pat, "%s\\*.txt", dir);
    WCHAR wpat[1200];
    WIN32_FIND_DATAW wfd;
    MultiByteToWideChar(CP_ACP, 0, pat, -1, wpat, 1200);
    fh = FindFirstFileW(wpat, &wfd);
    CHECK(fh != INVALID_HANDLE_VALUE && !wcscmp(wfd.cFileName, L"c.txt") && !FindNextFileW(fh, &wfd), "wide find with an extension pattern");
    if (fh != INVALID_HANDLE_VALUE) FindClose(fh);
    DeleteFileA(path); snprintf(path, sizeof path, "%s/b.bin", dir); DeleteFileA(path);
    CHECK(RemoveDirectoryA(dir), "remove the emptied folder");
}

static void test_strings_and_misc(void) {
    const char *u = "Mario \xc3\xa9t\xc3\xa9 \xf0\x9f\x8d\x84";        /* é and a mushroom emoji */
    WCHAR w[64];
    char back[64];
    int wn = MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 64);
    CHECK(wn == 12 && w[6] == 0xe9 && w[10] == 0x1f344 && w[11] == 0, "UTF-8 to wide counts the zero (%d)", wn);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0) == wn, "size query");
    int bn = WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof back, NULL, NULL);
    CHECK(bn == (int)strlen(u) + 1 && !strcmp(back, u), "and back (%d)", bn);
    CHECK(!_stricmp("StageData", "stagedata") && _strnicmp("ABc", "abd", 2) == 0, "_stricmp / _strnicmp");
    _putenv("SMO_SHIM_TEST=1");
    CHECK(getenv("SMO_SHIM_TEST") && !strcmp(getenv("SMO_SHIM_TEST"), "1"), "_putenv sets");
    _putenv("SMO_SHIM_TEST=");
    CHECK(!getenv("SMO_SHIM_TEST"), "_putenv with nothing after = removes");
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    int runs = 0;
    InitOnceExecuteOnce(&once, once_fn, &runs, NULL);
    InitOnceExecuteOnce(&once, once_fn, &runs, NULL);
    CHECK(runs == 1, "InitOnceExecuteOnce runs once");

    HANDLE m1 = CreateMutexA(NULL, FALSE, "Local\\smo_shim_test");
    CHECK(m1 && GetLastError() == ERROR_SUCCESS, "first named mutex is new (%u)", GetLastError());
    HANDLE m2 = CreateMutexA(NULL, FALSE, "Local\\smo_shim_test");
    CHECK(m2 && GetLastError() == ERROR_ALREADY_EXISTS, "second one finds the name in use (%u)", GetLastError());
    CHECK(WaitForSingleObject(m1, 100) == WAIT_OBJECT_0, "take the mutex");
    CHECK(WaitForSingleObject(m2, 50) == WAIT_TIMEOUT, "another handle waits while it is owned");
    CHECK(ReleaseMutex(m1) && WaitForSingleObject(m2, 100) == WAIT_OBJECT_0, "released, the other handle takes it");
    ReleaseMutex(m2);
    CloseHandle(m1); CloseHandle(m2);
    HANDLE m3 = CreateMutexA(NULL, FALSE, "Local\\smo_shim_test");
    CHECK(m3 && GetLastError() == ERROR_SUCCESS, "with every handle closed the name is free again (%u)", GetLastError());
    CloseHandle(m3);
    char lockf[1200], lockh[1200];
    snprintf(lockf, sizeof lockf, "%ssmo-mutex-Local_smo_shim_test.name", tmp_dir());
    recomp_path(lockh, sizeof lockh, lockf);
    CHECK(access(lockh, F_OK) == 0, "the mutex's lock file is in the temp folder (%s)", lockh);
    FILE *nul = fopen("NUL", "w");
    CHECK(nul && fputs("x", nul) >= 0 && access("NUL", F_OK) != 0, "\"NUL\" is the null device, not a file");
    if (nul) fclose(nul);

    HANDLE me = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    CHECK(me && WaitForSingleObject(me, 0) == WAIT_TIMEOUT, "this process is alive");
    CloseHandle(me);
    CHECK(OpenProcess(SYNCHRONIZE, FALSE, 999999) == NULL, "no process 999999");

    FARPROC fp = GetProcAddress(GetModuleHandleA(NULL), "test_exported_symbol");
    CHECK(fp && ((int (*)(void))fp)() == 7, "GetProcAddress in the executable");
    CHECK(LoadLibraryA("xinput1_4.dll") == NULL, "a Windows DLL is not found");
    static const int ro = 5;
    static int rw = 5;
    char full[512];
    CHECK(_fullpath(full, "/Users\\x/./a/../b\\c", sizeof full) && !strcmp(full, "\\Users\\x\\b\\c"), "_fullpath resolves . and .. (%s)", full);
    CHECK(recomp_mac_keycode('A') == 0x00 && recomp_mac_keycode(VK_RETURN) == 0x24 && recomp_mac_keycode(VK_F1 + 11) == 0x6F && recomp_mac_keycode(0x5D) == -1,
          "virtual keys to macOS key codes");
    CHECK(IsBadWritePtr((void *)&ro, sizeof ro) && !IsBadWritePtr(&rw, sizeof rw) && !IsBadReadPtr(&ro, sizeof ro) && IsBadReadPtr((void *)(uintptr_t)8, 4),
          "IsBadReadPtr / IsBadWritePtr");
    SYSTEMTIME st;
    GetSystemTime(&st);
    CHECK(st.wYear >= 2026 && st.wMonth >= 1 && st.wMonth <= 12, "system time (%u-%u)", st.wYear, st.wMonth);
    uint8_t r1[16] = {0}, r2[16] = {0};
    CHECK(SystemFunction036(r1, 16) && SystemFunction036(r2, 16) && memcmp(r1, r2, 16), "random bytes differ");
    DWORD_PTR pm = 0, sm = 0;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm && GetCurrentProcessorNumber() < 64, "processor mask %llx, processor %u", (unsigned long long)pm, GetCurrentProcessorNumber());
}

/* ---- __try / __except (1) */
#pragma clang diagnostic ignored "-Wreturn-type"      /* as in smo_nerve.c: both branches return */
static int probe(const volatile int *p) {
    __try {
        return *p;
    } __except (1) { return -1; }
}
static int nested(const volatile int *bad, const volatile int *good) {
    volatile int r = 0;                     /* changed in the block and read after a fault: volatile (see win32_posix.h) */
    __try {
        r = probe(bad);                     /* the inner block catches its own fault */
        r += *good;
        (void)*bad;                         /* this one is the outer block's */
        r = 1000;
    } __except (1) { r += 100; }
    return r;
}
static int fault_in_handler(const volatile int *bad) {
    volatile int r = 0;
    __try {
        __try { (void)*bad; } __except (1) { r = 1; (void)*bad; }      /* a fault in an __except goes to the block around it */
    } __except (1) { r += 10; }
    return r;
}
static void test_try(void) {
    static int x = 41;
    const volatile int *bad = (const volatile int *)(uintptr_t)0x10;
    CHECK(probe(&x) == 41, "a readable probe returns from inside __try");
    CHECK(probe(bad) == -1, "a fault goes to __except");
    CHECK(recomp_try_top == NULL, "no block left behind after a return or a fault");
    CHECK(nested(bad, &x) == 140, "nested blocks: the inner fault inside, the outer one outside (%d)", nested(bad, &x));
    CHECK(fault_in_handler(bad) == 11, "a fault in __except goes to the block around it (%d)", fault_in_handler(bad));
    int n = 0;
    for (int i = 0; i < 100000; i++) n += probe(i & 1 ? bad : &x) < 0;
    CHECK(n == 50000 && recomp_try_top == NULL, "100000 probes, half of them faulting (%d faults)", n);
}

/* ---- condition variables: the zero timeout is a poll (the message queues' TryReceive / TrySend rely on it) */
static CONDITION_VARIABLE g_cv;
static CRITICAL_SECTION g_cv_cs;
static DWORD WINAPI cv_waker(LPVOID arg) { (void)arg; Sleep(20); EnterCriticalSection(&g_cv_cs); LeaveCriticalSection(&g_cv_cs); WakeConditionVariable(&g_cv); return 0; }
static void test_condvar(void) {
    InitializeConditionVariable(&g_cv);
    InitializeCriticalSection(&g_cv_cs);
    EnterCriticalSection(&g_cv_cs);
    ULONGLONG t0 = GetTickCount64();
    int n = 0;
    while (n < 1000 && SleepConditionVariableCS(&g_cv, &g_cv_cs, 0)) n++;       /* a poll on a quiet variable: FALSE at once */
    CHECK(n == 0, "SleepConditionVariableCS(0) on a quiet variable returned TRUE %d times (a zero wait is a poll, not a wake)", n);
    CHECK(GetTickCount64() - t0 < 50, "the polls took %llu ms", (unsigned long long)(GetTickCount64() - t0));
    t0 = GetTickCount64();
    CHECK(!SleepConditionVariableCS(&g_cv, &g_cv_cs, 30), "a 30 ms wait with no wake times out (FALSE)");
    ULONGLONG dt = GetTickCount64() - t0;
    CHECK(dt >= 25 && dt < 500, "the 30 ms wait took %llu ms", (unsigned long long)dt);
    HANDLE h = CreateThread(NULL, 0, cv_waker, NULL, 0, NULL);
    CHECK(SleepConditionVariableCS(&g_cv, &g_cv_cs, 5000), "a wake from another thread ends the wait (TRUE)");
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
    LeaveCriticalSection(&g_cv_cs);
    DeleteCriticalSection(&g_cv_cs);
}

/* ---- a new thread's id is its own from its first instruction (CreateThread used to store it after pthread_create, and
 * a thread that ran first saw 0: EnterCriticalSection then took a free lock as its own by the owner check alone) */
static CRITICAL_SECTION g_race_cs;
static volatile LONG g_race_zero_ids, g_race_counter;
static DWORD WINAPI race_thread(LPVOID arg) {
    (void)arg;
    if (GetCurrentThreadId() == 0) InterlockedIncrement(&g_race_zero_ids);
    for (int i = 0; i < 200; i++) { EnterCriticalSection(&g_race_cs); g_race_counter++; LeaveCriticalSection(&g_race_cs); }
    return 0;
}
static void test_thread_id_from_start(void) {
    InitializeCriticalSection(&g_race_cs);
    for (int round = 0; round < 20; round++) {
        HANDLE hs[16];
        for (int i = 0; i < 16; i++) hs[i] = CreateThread(NULL, 0, race_thread, NULL, 0, NULL);
        for (int i = 0; i < 16; i++) { WaitForSingleObject(hs[i], INFINITE); CloseHandle(hs[i]); }
    }
    CHECK(g_race_zero_ids == 0, "%ld thread(s) saw their own id as 0 at start", (long)g_race_zero_ids);
    CHECK(g_race_counter == 20 * 16 * 200, "the critical section kept the count exact (%ld of %d)", (long)g_race_counter, 20 * 16 * 200);
    DeleteCriticalSection(&g_race_cs);
}

int main(void) {
    test_guest_ranges();
    test_reserve_commit_query();
    test_interlocked();
    test_threads();
    test_condvar();
    test_thread_id_from_start();
    test_misc();
    test_files();
    test_strings_and_misc();
    test_try();
    printf("win32_posix: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail != 0;
}
