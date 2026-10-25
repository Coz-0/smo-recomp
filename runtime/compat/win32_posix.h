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
#include <errno.h>
#include <time.h>

/* ---- types */
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD, UINT;
/* LONG / ULONG are 32 bits on Windows and long here: the runtime only counts with them, and prints them with %ld, which
 * would read 8 bytes of a variadic 4-byte int on Apple arm64. DWORD keeps 32 bits (layouts); its prints cast. */
typedef long LONG;
typedef unsigned long ULONG;
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
#define RECOMP_DECLSPEC_align(n) __attribute__((aligned(n)))
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
/* Reserves [base, base + size) with no access, so nothing else of the process is placed there. VirtualAlloc at a fixed
 * address inside it then maps over the reservation, and MEM_RELEASE gives the range back to it. 0 if it is taken. */
int recomp_reserve_window(uint64_t base, uint64_t size);
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
typedef struct { os_unfair_lock l; HANDLE volatile OwningThread; int depth; } CRITICAL_SECTION;   /* OwningThread: the owner's thread id, as on Windows */
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
typedef PROCESS_MEMORY_COUNTERS PROCESS_MEMORY_COUNTERS_EX;     /* PrivateUsage is already in it here */
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);
#define K32GetProcessMemoryInfo GetProcessMemoryInfo

/* ---- C runtime and string spellings */
#include <strings.h>
#include <stdio.h>
#include <wchar.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#define _strdup strdup
#define _strtoui64 strtoull
#define _strtoi64 strtoll
#define _putenv(s) recomp_putenv(s)
#define _putenv_s(k, v) setenv((k), (v), 1)
int recomp_putenv(const char *kv);               /* "NAME=value", "NAME=" removes it (as the MSVC runtime does) */
int _write(int fd, const void *p, unsigned n);
int _fileno(FILE *f);
intptr_t _get_osfhandle(int fd);                 /* a file HANDLE over the descriptor (FlushFileBuffers takes it) */
typedef uint8_t BOOLEAN;
typedef int16_t SHORT;
typedef wchar_t WCHAR;                           /* 32-bit here: text goes through MultiByteToWideChar and back, never to disk */
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
#define NTAPI
#define CALLBACK
#define CP_ACP 0u                                /* the ANSI code page is UTF-8 here */
#define CP_UTF8 65001u
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, WCHAR *w, int wn);
int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *w, int wn, char *s, int n, const char *def, BOOL *used);

/* ---- once */
typedef struct { volatile int state; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE, PVOID, PVOID *);
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID arg, PVOID *ctx);

/* ---- last error: set by the calls of this file that fail, from errno */
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_DISK_FULL 112u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
DWORD GetLastError(void);
void SetLastError(DWORD e);

/* ---- files. The runtime builds Windows paths ("%s\\..\\config", strrchr(path, '\\')), so the paths given to it
 * here (GetModuleFileName, GetTempPath, GetFullPathName, _fullpath) are written with backslashes, and every path taken
 * here, and fopen's, has its backslashes turned back into slashes. "\\Users\\x" is "/Users/x". */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_HIDDEN 0x2u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_TEMPORARY 0x100u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define MOVEFILE_REPLACE_EXISTING 1u
#define MOVEFILE_COPY_ALLOWED 2u
#define MOVEFILE_WRITE_THROUGH 8u
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { uintptr_t Internal, InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD attrs, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, void *overlapped);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, void *overlapped);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD method);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
DWORD GetFileAttributesA(LPCSTR path);
BOOL SetFileAttributesA(LPCSTR path, DWORD attrs);
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, void *out);
DWORD GetTempPathA(DWORD n, LPSTR out);          /* $TMPDIR, with a trailing separator */
DWORD GetFullPathNameA(LPCSTR path, DWORD n, LPSTR out, LPSTR *file_part);
DWORD GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size);
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH], cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH], cAlternateFileName[14];
} WIN32_FIND_DATAW;
/* The pattern is a folder and a last part with * and ? (as the runtime uses it: "dir\\*", "dir\\*.ext"). */
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL FindClose(HANDLE h);
FILE *recomp_fopen(const char *path, const char *mode);
char *_fullpath(char *out, const char *path, size_t n);      /* absolute, "." and ".." resolved, backslashes; need not exist */
#define fopen(p, m) recomp_fopen((p), (m))
void recomp_path(char *out, size_t n, const char *in);     /* a Windows-style path as a macOS one */

/* ---- named mutexes (across processes: lock files in $TMPDIR) and processes */
#define SYNCHRONIZE 0x00100000u
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name);   /* ERROR_ALREADY_EXISTS when another handle has the name */
BOOL ReleaseMutex(HANDLE h);
DWORD GetCurrentProcessId(void);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);              /* WaitForSingleObject: signalled once it has ended */
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
BOOL GetUserNameW(LPWSTR out, DWORD *n);       /* the login name; *n in characters with the zero, as on Windows */
DWORD GetCurrentProcessorNumber(void);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);   /* every processor, as one bit each */
#define NORMAL_PRIORITY_CLASS 0x20u
#define ABOVE_NORMAL_PRIORITY_CLASS 0x8000u
#define HIGH_PRIORITY_CLASS 0x80u
BOOL SetPriorityClass(HANDLE p, DWORD cls);
BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable);
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_IDLE (-15)

/* ---- memory probes */
BOOL IsBadReadPtr(const void *p, uintptr_t n);
BOOL IsBadWritePtr(void *p, uintptr_t n);

/* ---- time */
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
void GetSystemTime(SYSTEMTIME *t);
void GetLocalTime(SYSTEMTIME *t);
void GetSystemTimeAsFileTime(FILETIME *t);       /* 100 ns since 1601-01-01 UTC */

/* ---- random */
BOOLEAN SystemFunction036(PVOID buf, ULONG n);   /* RtlGenRandom */

/* ---- dynamic libraries (Windows DLL names are not found: callers take their fallback) */
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
typedef void (*FARPROC)(void);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);

/* ---- BMP headers: file layouts, so with fixed widths (LONG is long here) */
#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct {
    DWORD biSize; int32_t biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
    int32_t biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
_Static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40, "BMP header layouts");
#define BI_RGB 0u

/* ---- the keyboard: GetAsyncKeyState with Windows virtual-key codes (the runtime's key names and remapping files use
 * them). The display sets where key states come from (its window's keys); before that, the system's key state. */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD0 0x60
#define VK_F1 0x70
#define VK_F11 0x7A
#define VK_F12 0x7B
SHORT GetAsyncKeyState(int vk);                   /* 0x8000 while the key is down */
int recomp_mac_keycode(int vk);                   /* the macOS key code (kVK_*) of a virtual key, -1 if none */
void recomp_set_key_source(int (*down)(int vk));  /* display: key states from its window (NULL: the system's) */

/* ---- message boxes: a CoreFoundation alert, modal and blocking on any thread as on Windows. Without a user to
 * answer (no window server) it returns at once with the default button. */
typedef void *HWND;
#define MB_OK 0x0u
#define MB_OKCANCEL 0x1u
#define MB_YESNO 0x4u
#define MB_ICONERROR 0x10u
#define MB_ICONQUESTION 0x20u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_TASKMODAL 0x2000u
#define MB_SETFOREGROUND 0x10000u
#define MB_TOPMOST 0x40000u
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
BOOL MessageBeep(UINT type);

/* ---- __try { ... } __except (1) { ... }: the runtime's probes of memory that may not be readable. A fault (SIGSEGV,
 * SIGBUS) inside the block goes to the __except block, as with structured exception handling; only that form (the
 * filter always 1) is supported. The block is a loop of one pass: a break or continue in it, outside an inner loop,
 * would leave the block, not the loop around it (none of the runtime's blocks has one). A return from it is fine.
 * As with setjmp, a local variable changed inside the block and read after a fault must be volatile (structured
 * exception handling keeps it; none of the runtime's blocks reads one). */
#include <setjmp.h>
typedef struct RecompTry { sigjmp_buf jb; struct RecompTry *prev; int pass; } RecompTry;
extern _Thread_local RecompTry *recomp_try_top;
void recomp_try_push(RecompTry *t);
void recomp_try_pop(RecompTry *t);                    /* the cleanup of the block's frame */
void recomp_try_catch(void);                          /* in a SIGSEGV / SIGBUS handler: to the innermost block, if any */
void recomp_try_install(void);                        /* a handler that does only that (hostrt.c's crash handler calls it too) */
#define __try for (RecompTry _recomp_try __attribute__((cleanup(recomp_try_pop))) = {.pass = 0}; !_recomp_try.pass; _recomp_try.pass = 1) \
                  if (recomp_try_push(&_recomp_try), sigsetjmp(_recomp_try.jb, 1) == 0)
#define __except(filter) else

/* ---- macOS only: a thread's frames, for the crash report and the samplers (pcs[0] = where it is) */
int host_thread_backtrace(HANDLE h, uint64_t *pcs, int max);           /* suspends and resumes the thread */
int host_frames_from(uint64_t pc, uint64_t lr, uint64_t fp, uint64_t *pcs, int max);
int host_backtrace_here(uint64_t *pcs, int max);                        /* the calling thread, from its caller up */

#endif
#endif
