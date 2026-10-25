/* The Win32 calls the runtime uses, on macOS (POSIX and Mach), so the runtime's sources build unchanged there.
 *
 * Included in place of <windows.h> on every system but Windows. Only what the runtime calls is here, with the
 * Windows meaning it relies on; a call outside that subset is a compile error, not a silent stub. What cannot be
 * given its Windows meaning (structured exception handling, thread contexts, unwinding) is not here: the files that
 * use it have a macOS version of that code under #ifndef _WIN32.
 *
 * Locks and condition variables are valid when zero-filled, as on Windows (a static SRWLOCK needs no initialiser). */
#ifndef RECOMP_WIN32_POSIX_H
#define RECOMP_WIN32_POSIX_H
#ifndef _WIN32

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <os/lock.h>

/* ---- types */
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD, UINT, ULONG;
typedef int32_t LONG;
typedef int64_t LONG64, LONGLONG;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, DWORD_PTR;
typedef size_t SIZE_T;
typedef void *LPVOID, *PVOID, *HANDLE, *HMODULE;
typedef const void *LPCVOID;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
#define WINAPI
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* MSVC spellings the runtime uses */
#define __declspec(x) RECOMP_DECLSPEC_##x
#define RECOMP_DECLSPEC_thread _Thread_local
#define RECOMP_DECLSPEC_dllexport __attribute__((visibility("default")))
#define RECOMP_DECLSPEC_dllimport
#define RECOMP_DECLSPEC_noinline __attribute__((noinline))
#define RECOMP_DECLSPEC_noreturn __attribute__((noreturn))
void *_aligned_malloc(size_t size, size_t align);
void *_aligned_realloc(void *p, size_t size, size_t align);
void _aligned_free(void *p);
extern int __argc;              /* the process's arguments, as the MSVC C runtime has them (set before main) */
extern char **__argv;

/* ---- virtual memory (granularity: the host page, 16 KB on Apple silicon) */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
typedef struct {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, DWORD *old);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
size_t recomp_page_size(void);
/* Copies n bytes from p into out; 0 when any of them is not readable. No signal is raised: this is the macOS form of
 * the runtime's __try { read } __except (1) { ... } probes. */
int host_try_read(const void *p, void *out, size_t n);

/* ---- interlocked operations (sequentially consistent, return values as on Windows) */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement64(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchange64(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchangePointer(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define YieldProcessor() __builtin_arm_yield()
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* ---- locks and condition variables */
typedef struct { os_unfair_lock l; } SRWLOCK;
#define SRWLOCK_INIT {OS_UNFAIR_LOCK_INIT}
typedef struct { os_unfair_lock l; volatile uint32_t owner; int depth; } CRITICAL_SECTION;
typedef struct { volatile uint64_t seq; } CONDITION_VARIABLE;     /* waits on the address of seq */
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1u
void InitializeSRWLock(SRWLOCK *l);
void AcquireSRWLockExclusive(SRWLOCK *l);
void ReleaseSRWLockExclusive(SRWLOCK *l);
void AcquireSRWLockShared(SRWLOCK *l);        /* exclusive: correct, less parallel */
void ReleaseSRWLockShared(SRWLOCK *l);
BOOL TryAcquireSRWLockExclusive(SRWLOCK *l);
void InitializeCriticalSection(CRITICAL_SECTION *cs);
void DeleteCriticalSection(CRITICAL_SECTION *cs);
void EnterCriticalSection(CRITICAL_SECTION *cs);
BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs);
void LeaveCriticalSection(CRITICAL_SECTION *cs);
void InitializeConditionVariable(CONDITION_VARIABLE *cv);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
void WakeConditionVariable(CONDITION_VARIABLE *cv);
void WakeAllConditionVariable(CONDITION_VARIABLE *cv);

/* ---- clocks */
ULONGLONG GetTickCount64(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);       /* nanoseconds */
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);     /* 1e9 */
void Sleep(DWORD ms);
BOOL SwitchToThread(void);

/* ---- threads and events (HANDLE: a thread or an event made here) */
#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define DUPLICATE_SAME_ACCESS 0x2u
HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *tid);
HANDLE GetCurrentThread(void);               /* a pseudo handle, as on Windows: DuplicateHandle makes it a real one */
HANDLE GetCurrentProcess(void);
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD options);
DWORD GetCurrentThreadId(void);              /* the Mach thread port name: unique in the process while the thread lives */
DWORD GetThreadId(HANDLE h);
DWORD SuspendThread(HANDLE h);
DWORD ResumeThread(HANDLE h);
BOOL SetThreadPriority(HANDLE h, int priority);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);   /* macOS has none: accepted, ignored */
BOOL QueryThreadCycleTime(HANDLE h, ULONG64 *t);    /* CPU time used by the thread, in nanoseconds (not cycles) */
HANDLE CreateEventA(void *sec, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
BOOL CloseHandle(HANDLE h);
__attribute__((noreturn)) void ExitProcess(UINT code);

/* ---- process and modules */
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4u
HMODULE GetModuleHandleA(LPCSTR name);       /* NULL: the executable */
BOOL GetModuleHandleExA(DWORD flags, LPCSTR addr, HMODULE *m);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size);
BOOL GetSystemTimes(FILETIME *idle, FILETIME *kernel, FILETIME *user);    /* kernel includes idle, as on Windows */
BOOL GetProcessTimes(HANDLE p, FILETIME *creation, FILETIME *exit, FILETIME *kernel, FILETIME *user);
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, PagefileUsage, PrivateUsage; } PROCESS_MEMORY_COUNTERS;
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);

/* ---- macOS only: a thread's frames, for the crash report and the samplers (pcs[0] = where it is) */
int host_thread_backtrace(HANDLE h, uint64_t *pcs, int max);           /* suspends and resumes the thread */
int host_frames_from(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t *pcs, int max);

#endif
#endif
