/* smo_nn.c - SDK functions of the game's imports that no other table handles.
 *
 * Each handler states its source. Tags: [doc] public SDK documentation / switchbrew; [game] read in SMO's main;
 * [sdk] read in the base game's sdk module (NintendoSDK 3.5.1); [sdk 7.3.2] in the game's. Registered by boot.c after every other table (register_smo_imports): these
 * names have no other handler, so nothing is replaced.
 *
 * ---- nn::os memory heap (sead's heaps sit on it)
 *   SetMemoryHeapSize(size)            [doc] size is a multiple of MemoryHeapUnitSize (2 MB); the heap is one range
 *                                      whose base does not move when it is resized. Result 0 on success.
 *   AllocateMemoryBlock(&addr, size)   [doc] size is a multiple of MemoryBlockUnitSize (2 MB); the block comes out of
 *                                      the heap set above. Failure: 0x1003 = os OutOfMemory (the value of the
 *                                      SDK's own allocator for the same failure, nnservices.c).
 *   FreeMemoryBlock(addr, size)        [doc] gives the block back to the heap.
 * Host: one region of HEAP_RESERVE bytes is RESERVED once; SetMemoryHeapSize commits [0, size). Blocks are handed out
 * first-fit on 2 MB granules from a bitmap. Fresh pages are zero (as the kernel's heap pages are when first mapped).
 * The region is a VIEW OF A PAGEFILE-BACKED SECTION (SEC_RESERVE), not plain VirtualAlloc memory, so that the same
 * physical pages can be shown at a second address: NVN virtual memory pools (nvnMemoryPoolMapVirtual, nvn_stub.c) map
 * views of it, the way the console's GPU address space maps physical pool pages. smo_heap_section_offset() tells
 * whether an address lies in the heap and where.
 *
 * ---- nn::oe
 *   EnableGamePlayRecording(buffer, size)   [doc] lets the system record gameplay video into the buffer the game gives.
 *                                      The runtime has no video capture: the call does nothing. It is a DO-NOTHING
 *                                      handler on purpose and stays listed as such by the strict audit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#ifdef __APPLE__
#include <IOKit/pwr_mgt/IOPMLib.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif
#include "hostrt.h"
#include "sdklift.h"

#define UNIT 0x200000ull                       /* MemoryHeapUnitSize = MemoryBlockUnitSize */
#define HEAP_RESERVE (16ull << 30)             /* address space reserved for the heap: more than the console has */
#define RESULT_OUT_OF_MEMORY 0x1003u

static SRWLOCK g_heap_lock = SRWLOCK_INIT;
static HANDLE g_heap_section;                  /* pagefile-backed section the heap is a view of */
static uint8_t *g_heap;                        /* base, fixed once reserved */
static uint64_t g_heap_size;                   /* committed size (the heap size the game set) */
static uint8_t g_used[HEAP_RESERVE / UNIT];    /* 1 = granule belongs to an allocated block */
static unsigned long long g_alloc_calls, g_alloc_fail, g_peak;

int smo_heap_section_handle(HANDLE *h) { *h = g_heap_section; return g_heap_section != NULL; }
#ifdef __linux__
static int g_heap_fd = -1;                     /* Linux: the memfd the heap is a shared mapping of */
int smo_heap_memfd(void) { return g_heap_fd; }
#endif
/* The committed heap: base and size (0 before SetMemoryHeapSize). */
int smo_heap_range(uint64_t *base, uint64_t *size) { *base = (uint64_t)(uintptr_t)g_heap; *size = g_heap ? g_heap_size : 0; return g_heap != NULL; }

/* The heap's section and the offset of [addr, addr+size) in it; 0 if the range is not inside the committed heap. */
int smo_heap_section_offset(uint64_t addr, uint64_t size, HANDLE *section, uint64_t *offset) {
    uint64_t base = (uint64_t)(uintptr_t)g_heap;
    if (!g_heap || addr < base || addr + size > base + g_heap_size || addr + size < addr) return 0;
    *section = g_heap_section;
    *offset = addr - base;
    return 1;
}

static void os_SetMemoryHeapSize(Ctx *c) {
    uint64_t size = c->x[0];
    uint32_t r = 0;
    AcquireSRWLockExclusive(&g_heap_lock);
    if (size % UNIT || size > HEAP_RESERVE) r = RESULT_OUT_OF_MEMORY;
    else {
        if (!g_heap) {
#ifdef _WIN32
            g_heap_section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE,
                                                (DWORD)(HEAP_RESERVE >> 32), (DWORD)HEAP_RESERVE, NULL);
            if (g_heap_section) g_heap = MapViewOfFile(g_heap_section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)HEAP_RESERVE);
#elif defined(__linux__)
            /* Linux: a shared mapping of a memfd as large as the reserve (sparse: no memory until a page is touched), so a
             * second view of its pages (graphics memory pools) is an mmap of the same file range, as a second view of a
             * section is on Windows; the "section" is the heap's base, as on macOS. Committing is mprotect. */
            g_heap_fd = memfd_create("smo-heap", MFD_CLOEXEC);
            if (g_heap_fd >= 0 && !ftruncate(g_heap_fd, (off_t)HEAP_RESERVE)) {
                void *p = mmap(NULL, (size_t)HEAP_RESERVE, PROT_NONE, MAP_SHARED | MAP_NORESERVE, g_heap_fd, 0);
                g_heap = p == MAP_FAILED ? NULL : p;
            }
            g_heap_section = g_heap;
#else
            /* macOS: plain reserved memory. A second view of its pages (graphics memory pools) is made with mach_vm_remap
             * without copying, which shares them as a second view of a section does; the "section" is the heap's base. */
            g_heap = VirtualAlloc(NULL, (SIZE_T)HEAP_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
            g_heap_section = g_heap;
#endif
            if (!g_heap) printf("nn::os::SetMemoryHeapSize: cannot create the heap section (error %lu)\n", (unsigned long)GetLastError());
        }
        if (!g_heap) r = RESULT_OUT_OF_MEMORY;
        else if (size > g_heap_size) {
            if (!VirtualAlloc(g_heap + g_heap_size, size - g_heap_size, MEM_COMMIT, PAGE_READWRITE)) r = RESULT_OUT_OF_MEMORY;
        } else if (size < g_heap_size) {
            for (uint64_t g = size / UNIT; g < g_heap_size / UNIT; g++) if (g_used[g]) r = RESULT_OUT_OF_MEMORY;   /* blocks still live above */
            /* pages of a section view cannot be decommitted (VirtualFree MEM_DECOMMIT fails on mapped views): they stay
             * committed; the heap's size shrinks as on the console, only host memory is not given back */
        }
        if (!r) g_heap_size = size;
    }
    ReleaseSRWLockExclusive(&g_heap_lock);
    if (!r && g_heap) { void smo_watch_arm(uint8_t *heap, uint64_t heap_size); smo_watch_arm(g_heap, g_heap_size); }      /* hostrt.c, debug aid SMO_WATCH */
    printf("nn::os::SetMemoryHeapSize(0x%llx): %s, heap at %p\n", (unsigned long long)size, r ? "FAILED" : "ok", (void *)g_heap);
    c->x[0] = r;
}

static void os_AllocateMemoryBlock(Ctx *c) {
    uint64_t out = c->x[0], size = c->x[1];
    uint64_t n = (size + UNIT - 1) / UNIT, total = g_heap_size / UNIT, run = 0, at = 0;
    int found = 0;
    AcquireSRWLockExclusive(&g_heap_lock);
    g_alloc_calls++;
    if (size && size % UNIT == 0)
        for (uint64_t g = 0; g < total; g++) {
            run = g_used[g] ? 0 : run + 1;
            if (run == n) { at = g + 1 - n; found = 1; break; }
        }
    if (found) {
        memset(g_used + at, 1, (size_t)n);
        uint64_t top = (at + n) * UNIT;
        if (top > g_peak) g_peak = top;
    } else g_alloc_fail++;
    ReleaseSRWLockExclusive(&g_heap_lock);
    if (found) { wr64(out, (uint64_t)(uintptr_t)(g_heap + at * UNIT)); c->x[0] = 0; }
    else {
        printf("nn::os::AllocateMemoryBlock(0x%llx): out of memory (heap 0x%llx)\n", (unsigned long long)size, (unsigned long long)g_heap_size);
        c->x[0] = RESULT_OUT_OF_MEMORY;
    }
}

/* ---- nn::diag / nn::os: debugger, user exception handler, abort observers.
 *   IsDebuggerAttached()       [doc] false: no debugger is attached to the program (true of a retail run).
 *   SetUserExceptionHandler(handler, stack, size, info)  [doc] the kernel calls the handler on a CPU exception. The runtime
 *                              reports host exceptions itself (hostrt.c crash report) and does NOT call the game's
 *                              handler: recorded only (SIMPLIFIED; the strict audit lists it).
 *   InitializeAbortObserverHolder(holder, fn) / RegisterAbortObserver(holder)  [doc] fn is called with the abort's
 *                              details when the program aborts. Recorded; the runtime's abort path (guest_trap) does NOT
 *                              call them (SIMPLIFIED). */
static uint64_t g_user_exception_handler, g_abort_observer[8];
static int g_abort_observers;
static void diag_IsDebuggerAttached(Ctx *c) { c->x[0] = 0; }
static void diag_NotNullImpl(Ctx *c) { c->x[0] = c->x[0] != 0; }      /* [sdk] 0x43f58: cmp x0, #0; cset w0, ne */
static void os_SetUserExceptionHandler(Ctx *c) { g_user_exception_handler = c->x[0]; }
static void diag_InitializeAbortObserverHolder(Ctx *c) { wr64(c->x[0], c->x[1]); }      /* holder +0: the function */
static void diag_RegisterAbortObserver(Ctx *c) { if (g_abort_observers < 8) g_abort_observer[g_abort_observers++] = c->x[0]; }

/* ---- nn::hid debug pad [doc]: a retail console has none. InitializeDebugPad does nothing visible;
 * GetDebugPadState(DebugPadState*) gives a state whose attributes say "not connected" (all zero). */
static void hid_InitializeDebugPad(Ctx *c) { (void)c; }
static void hid_GetDebugPadState(Ctx *c) { memset((void *)(uintptr_t)c->x[0], 0, 0x20); }   /* s64 sampling number, u32 attributes, u32 buttons, two sticks of 2 x s32 = 0x20 bytes */

/* ---- nn::oe performance mode [doc]: PerformanceMode Normal = 0 (handheld), Boost = 1 (docked); the console is docked
 * here (graphics.ini console_mode, the same source nnsdk.c's GetOperationMode uses).
 * GetPerformanceConfiguration(mode): the clock configuration of that mode. SMO decodes the answer in
 * al::setGpuPerformance [game 0xa28468]: 0x00020003 / 0x00020004 / 0x92220008 -> tier 0, 0x00010001 -> tier 1, others
 * -> tier 2. Answered: Boost 0x00020003, Normal 0x00010001 (values from the set the game knows; WHICH one a console
 * reports by default is NOT VERIFIED). SetPerformanceConfiguration / the two notification switches are recorded only
 * (the host has no clocks to set and sends no resume / mode-change notifications: SIMPLIFIED). */
#include "gfx_options.h"
static int docked(void) { return !gfx_options()->handheld; }   /* graphics.ini console_mode, as nnsdk.c GetOperationMode */
static uint32_t g_perf_config[2] = {0x00010001u, 0x00020003u};
static int g_resume_notify, g_perfmode_notify;
static void oe_GetPerformanceMode(Ctx *c) { c->x[0] = docked() ? 1 : 0; }
static void oe_GetPerformanceConfiguration(Ctx *c) { c->x[0] = g_perf_config[(uint32_t)c->x[0] == 1]; }
static void oe_SetPerformanceConfiguration(Ctx *c) { g_perf_config[(uint32_t)c->x[0] == 1] = (uint32_t)c->x[1]; }
static void oe_SetResumeNotificationEnabled(Ctx *c) { g_resume_notify = (int)(c->x[0] & 1); }
static void oe_SetPerformanceModeChangedNotificationEnabled(Ctx *c) { g_perfmode_notify = (int)(c->x[0] & 1); }

/* ---- nn::prepo::PlayReport(const char *event_id): the default constructor (nnmisc.c: byte @0 = 0, three 8-byte fields
 * @0x20..0x37 = 0) followed by SetEventId; play reports are accepted and discarded offline, as in nnmisc.c. */
static void prepo_PlayReport_ctor_id(Ctx *c) { wr8(c->x[0], 0); wr64(c->x[0] + 0x20, 0); wr64(c->x[0] + 0x28, 0); wr64(c->x[0] + 0x30, 0); }

/* ---- nn::vi::SetLayerScalingMode(Layer*, ScalingMode) [doc]: how the system scales the layer to the screen. The host
 * window scales the presented image itself (display.c): recorded only, Result 0 (SIMPLIFIED). */
static uint32_t g_layer_scaling_mode;
static void vi_SetLayerScalingMode(Ctx *c) { g_layer_scaling_mode = (uint32_t)c->x[1]; c->x[0] = 0; }

/* ---- nn::os transfer memory, below the SDK's own nn::os::CreateTransferMemory & co (cut in sdklift_rules.txt at
 * nn::os::detail::TransferMemoryImplByHorizon, the layer that makes the kernel calls). [doc] A transfer memory lends a
 * range of the owner's memory to another process, which maps it; while it is lent, the owner's access is restricted to
 * the given permission. Here the borrower is the in-process audio server (smo_audio.c), so "mapping" is the range
 * itself: Map returns the original address. NOT modelled: the owner's permission change (the client does not touch the
 * lent buffer meanwhile; SIMPLIFIED). Results: 0, or 0xca01 (os InvalidHandle, NOT VERIFIED) for an unknown handle. */
#define TM_MAX 64
static struct { uint64_t addr, size; int used; } g_tm[TM_MAX];
static SRWLOCK g_tm_lock = SRWLOCK_INIT;
static void tm_Create(Ctx *c) {                 /* (u32 *handle, void *addr, size_t size, MemoryPermission) */
    int h = -1;
    AcquireSRWLockExclusive(&g_tm_lock);
    for (int i = 1; i < TM_MAX; i++) if (!g_tm[i].used) { g_tm[i].used = 1; g_tm[i].addr = c->x[1]; g_tm[i].size = c->x[2]; h = i; break; }
    ReleaseSRWLockExclusive(&g_tm_lock);
    if (h < 0) guest_trap("transfer memory: too many at once (smo_nn.c TM_MAX)", c->x[30] - 4);
    wr32(c->x[0], 0xf000u + (uint32_t)h);
    c->x[0] = 0;
}
static int tm_index(uint32_t handle) { int i = (int)handle - 0xf000; return i > 0 && i < TM_MAX && g_tm[i].used ? i : -1; }
static void tm_Close(Ctx *c) {                  /* (u32 handle) */
    int i = tm_index((uint32_t)c->x[0]);
    if (i >= 0) g_tm[i].used = 0;
}
static void tm_Map(Ctx *c) {                    /* (void **out, u32 handle, void *hint, size_t size, MemoryPermission) */
    int i = tm_index((uint32_t)c->x[1]);
    if (i < 0) { c->x[0] = 0xca01; return; }
    if (c->x[3] && c->x[3] != g_tm[i].size) guest_trap("transfer memory: mapped with another size", c->x[30] - 4);
    wr64(c->x[0], g_tm[i].addr);
    c->x[0] = 0;
}
/* nn::os::MapTransferMemory first takes an address range from the AddressSpaceAllocator (AllocateSpace(this, size,
 * align): a free range found with svc QueryMemory), maps there, then checks that the guard pages around the mapping are
 * free (CheckGuardSpace(this, addr, size, guard)) and retries elsewhere if not [sdk 0x89a40]. With in-process aliasing
 * (tm_Map returns the lent range itself) the hint is not used and there are no guard pages: AllocateSpace answers a
 * fixed non-zero, page-aligned hint and CheckGuardSpace answers true. */
static void as_AllocateSpace(Ctx *c) { c->x[0] = 0x10000000ull; }
static void as_CheckGuardSpace(Ctx *c) { c->x[0] = 1; }
static void tm_Unmap(Ctx *c) { c->x[0] = tm_index((uint32_t)c->x[0]) >= 0 ? 0 : 0xca01; }   /* (u32 handle, void *addr, size_t size) */

void smo_heap_report(FILE *out) {
    if (!g_heap) return;
    fprintf(out, "memory heap: 0x%llx bytes set, %llu block allocations (%llu failed), highest use 0x%llx\n",
            (unsigned long long)g_heap_size, g_alloc_calls, g_alloc_fail, (unsigned long long)g_peak);
}

/* ---- nn::diag, the forms SMO imports that had no handler (a call returned 0 and the game went on) [doc]:
 *   AbortImpl(condition, function, file, line, const Result *, format, ...)  the program stops with that message.
 *   OnAssertionFailure(type, condition, function, file, line)  an SDK assertion failed: the program stops.
 *   LogImpl(meta, format, ...) / PutImpl(meta, text, length)  the program's debug log (NN_LOG; sead's print goes to
 *     PutImpl): written to the run log here (the first 500 lines, then counted).
 *   GetBacktrace(addresses, max) -> count  the return addresses of the calling frames: the caller's, then the guest
 *     frame-pointer chain (frame record: previous frame, return address).
 *   GetSymbolName(out, size, address) -> start of the symbol, or 0; GetSymbolSize(address) -> its size, or 0: the
 *     function that contains the address and the game's own dynamic symbol for it (a function without one: not found).
 *   GetRequiredBufferSizeForGetAllModuleInfo() / GetAllModuleInfo(out, buffer, size) -> count: the module list; the runtime
 *     reports none (SIMPLIFIED). sead calls these only to print an exception report, which the runtime writes itself
 *     (hostrt.c crash report). */
int clib_format_call(Ctx *c, int fmt_reg, char *out, size_t size);      /* clib.c */
static const char *sstr(uint64_t p) { return p ? (const char *)(uintptr_t)p : "(null)"; }
static void diag_AbortImpl_result(Ctx *c) {
    char msg[1024];
    clib_format_call(c, 5, msg, sizeof msg);
    printf("\nnn::diag abort: %s  in %s  (%s:%d), result 0x%x: %s\n", sstr(c->x[0]), sstr(c->x[1]), sstr(c->x[2]), (int)c->x[3],
           c->x[4] ? rd32(c->x[4]) : 0u, msg);
    fflush(stdout);
    guest_trap("guest called nn::diag::detail::AbortImpl", c->x[30] - 4);
}
static void diag_OnAssertionFailure(Ctx *c) {
    printf("\nnn::diag assertion failed (type %d): %s  in %s  (%s:%d)\n", (int)c->x[0], sstr(c->x[1]), sstr(c->x[2]), sstr(c->x[3]), (int)c->x[4]);
    fflush(stdout);
    guest_trap("guest called nn::diag::detail::OnAssertionFailure", c->x[30] - 4);
}
static volatile LONG g_diag_log_lines;
static void diag_log_line(const char *s, size_t n) {
    LONG k = InterlockedIncrement(&g_diag_log_lines);
    if (k > 500) { if (k == 501) printf("nn::diag log: more lines are not printed\n"); return; }
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    printf("nn::diag log: %.*s\n", (int)n, s);
}
static void diag_LogImpl(Ctx *c) { char msg[1024]; int n = clib_format_call(c, 1, msg, sizeof msg); diag_log_line(msg, n < 0 ? 0 : (size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1); }
static void diag_PutImpl(Ctx *c) { if (c->x[1]) diag_log_line((const char *)(uintptr_t)c->x[1], (size_t)c->x[2]); }
static int readable(uint64_t p, size_t n) {
    MEMORY_BASIC_INFORMATION mb;
    if (!p || VirtualQuery((void *)(uintptr_t)p, &mb, sizeof mb) != sizeof mb || mb.State != MEM_COMMIT) return 0;
    if (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return p + n <= (uint64_t)(uintptr_t)mb.BaseAddress + mb.RegionSize;
}
static void diag_GetBacktrace(Ctx *c) {
    uint64_t out = c->x[0], fp = c->x[29];
    int max = (int)c->x[1], n = 0;
    if (n < max && c->x[30]) wr64(out + 8 * (uint64_t)n++, c->x[30]);
    while (n < max && !(fp & 7) && readable(fp, 16)) {
        uint64_t next = rd64(fp), lr = rd64(fp + 8);
        if (!lr) break;
        wr64(out + 8 * (uint64_t)n++, lr);
        if (next <= fp) break;                       /* frames go up the stack */
        fp = next;
    }
    c->x[0] = (uint64_t)n;
}
static int fn_index(uint64_t addr) {                 /* the lifted function that contains a guest address, or -1 */
    int lo = 0, hi = fn_table_count - 1, best = -1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (fn_table[mid].addr <= addr) { best = mid; lo = mid + 1; } else hi = mid - 1; }
    return best;
}
static const char *sym_at(uint64_t start) {           /* the game's dynamic symbol at a function start */
    static uint64_t *addr;
    static const char **name;
    static int n = -1;
    if (n < 0) {
        n = 0;
        addr = malloc(sizeof *addr * (size_t)dyn_sym_count); name = malloc(sizeof *name * (size_t)dyn_sym_count);
        for (int i = 0; addr && name && i < dyn_sym_count; i++)
            if (dyn_syms[i].defined && dyn_syms[i].value) {      /* insertion into the sorted array would be slow: append, then sort */
                addr[n] = GUEST_BASE + dyn_syms[i].value; name[n] = dyn_syms[i].name; n++;
            }
        for (int gap = n / 2; gap > 0; gap /= 2)          /* shell sort by address, the names along */
            for (int i = gap; i < n; i++)
                for (int j = i; j >= gap && addr[j - gap] > addr[j]; j -= gap) {
                    uint64_t t = addr[j]; addr[j] = addr[j - gap]; addr[j - gap] = t;
                    const char *s = name[j]; name[j] = name[j - gap]; name[j - gap] = s;
                }
    }
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (addr[mid] == start) return name[mid]; if (addr[mid] < start) lo = mid + 1; else hi = mid - 1; }
    return NULL;
}
static void diag_GetSymbolName(Ctx *c) {             /* (char *out, size_t size, uintptr_t address) -> uintptr_t */
    int i = fn_index(c->x[2]);
    const char *s = i >= 0 ? sym_at(fn_table[i].addr) : NULL;
    if (c->x[1]) { size_t n = s ? strlen(s) : 0; if (n >= c->x[1]) n = (size_t)c->x[1] - 1; if (n) memcpy((void *)(uintptr_t)c->x[0], s, n); wr8(c->x[0] + n, 0); }
    c->x[0] = s ? fn_table[i].addr : 0;
}
static void diag_GetSymbolSize(Ctx *c) {             /* (uintptr_t address) -> size_t */
    int i = fn_index(c->x[0]);
    c->x[0] = i >= 0 && i + 1 < fn_table_count && sym_at(fn_table[i].addr) ? fn_table[i + 1].addr - fn_table[i].addr : 0;
}
static void diag_GetRequiredBufferSizeForGetAllModuleInfo(Ctx *c) { c->x[0] = 0; }
static void diag_GetAllModuleInfo(Ctx *c) { if (c->x[0]) wr64(c->x[0], 0); c->x[0] = 0; }

/* ---- nn::time library state [doc]: IsInitialized() is true between Initialize() and Finalize() (it used to answer
 * false always; sead::DateTime then initializes again, which was harmless). Replaces nnsdk.c's ret0 handlers. */
static volatile LONG g_time_init;
static void time_Initialize(Ctx *c) { InterlockedExchange(&g_time_init, 1); c->x[0] = 0; }
static void time_IsInitialized(Ctx *c) { c->x[0] = g_time_init != 0; }
static void time_Finalize(Ctx *c) { InterlockedExchange(&g_time_init, 0); c->x[0] = 0; }

/* ---- nn::oe [doc]
 *   SetScreenShotImageOrientation(orientation)  the orientation of screenshots taken from now on (0 normal, 1 rotated
 *     by 90, 2 by 180, 3 by 270 degrees): SMO's snapshot mode sets it when the camera is turned. Kept for the screenshot
 *     of the capture button (smo_screenshot_orientation).
 *   SetUserInactivityDetectionTimeExtended(on) / IsUserInactivityDetectionTimeExtended()  while on, the console waits
 *     much longer before it dims the screen (SMO turns it on for the ending): here the display is kept on meanwhile
 *     (a power request of its own, apart from the auto-sleep sections of nnmisc.c). */
static volatile LONG g_shot_orientation;
int smo_screenshot_orientation(void) { return (int)g_shot_orientation; }
static void oe_SetScreenShotImageOrientation(Ctx *c) { InterlockedExchange(&g_shot_orientation, (LONG)(c->x[0] & 3)); }
static HANDLE g_inact_req;
static int g_inact_ext;
static void oe_SetUserInactivityDetectionTimeExtended(Ctx *c) {
    int on = (int)(c->x[0] & 1);
#ifdef __linux__
    /* Linux: SDL keeps the screen saver off while the game's window is up (SDL_HINT_VIDEO_ALLOW_SCREENSAVER is 0 by
     * default), which is what the request is for */
    (void)on; (void)g_inact_req;
#elif !defined(_WIN32)
    /* macOS: a power assertion that keeps the display awake, as the power request does on Windows */
    static IOPMAssertionID assertion;
    if (on && !g_inact_ext && IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
            CFSTR("Super Mario Odyssey (smo-recomp): the game asked for a longer time before the screen dims"), &assertion) != kIOReturnSuccess) assertion = 0;
    if (!on && g_inact_ext && assertion) { IOPMAssertionRelease(assertion); assertion = 0; }
    (void)g_inact_req;
#else
    if (!g_inact_req) {
        REASON_CONTEXT rc;
        memset(&rc, 0, sizeof rc);
        rc.Version = POWER_REQUEST_CONTEXT_VERSION; rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        rc.Reason.SimpleReasonString = L"Super Mario Odyssey (smo-recomp): the game asked for a longer time before the screen dims";
        g_inact_req = PowerCreateRequest(&rc);
        if (g_inact_req == INVALID_HANDLE_VALUE) g_inact_req = NULL;
    }
    if (g_inact_req && on != g_inact_ext) { if (on) PowerSetRequest(g_inact_req, PowerRequestDisplayRequired); else PowerClearRequest(g_inact_req, PowerRequestDisplayRequired); }
#endif
    g_inact_ext = on;
}
static void oe_IsUserInactivityDetectionTimeExtended(Ctx *c) { c->x[0] = (uint64_t)g_inact_ext; }

/* ---- nn::nifm, offline [doc]: HandleNetworkRequestResult() shows the console's error screen for a network request
 * that failed and returns what the player did; the game ([game] al::NexNetworkStateHolder::exeConnectToInternet) takes
 * 0 as "connected now", the Result 0x17c6e (nifm 190: the error was shown and dealt with) as "not connected, stop", and
 * anything else as an error. This console has no network, so: 0x17c6e (it used to answer 0 and the game went on to its
 * account check as if connected). SetLocalNetworkMode(bool): local communication only; nothing to set. */
static void nifm_HandleNetworkRequestResult(Ctx *c) { c->x[0] = 0x17c6e; }

/* ---- Labo VR, as the game drives it ([game] 0x7b4c80: pl::BeginVrMode, and only when it succeeds
 * BeginBlockingHomeButton + DisableRecording; 0x7b4cc0: EndVrMode, EndBlockingHomeButton, EnableRecording):
 *   oe::BeginBlockingHomeButton / EndBlockingHomeButton [sdk 7.3.2 0x3218c0 / 0x321920]: one request each to the
 *       application functions (abort on failure). The PC has no HOME button to block: nothing to do.
 *   oe::EnableRecording [0x323640]: the same, to allow the video capture again; there is no video capture here (see
 *       EnableGamePlayRecording above; DisableRecording does nothing either, nnsdk.c).
 * Stereo vision (parental controls), as the game checks it before VR ([game] 0x568fe0 / 0x569050): it shows an
 * error and gives up if ConfirmStereoVisionRestrictionConfigurable fails; else, when GetStereoVisionRestriction differs
 * from what it wants, RequestStereoVisionRestrictionConfigurationPermission (0x16c8e: cancelled) and then
 * SetStereoVisionRestriction.
 *   pctl::ConfirmStereoVisionRestrictionConfigurable [sdk 7.3.2 0x330840]: the module's Result.
 *   pctl::RequestStereoVisionRestrictionConfigurationPermission [0x330880]: the same request; only when it fails, the
 *       PIN applet.
 *   pctl::GetStereoVisionRestriction [0x3308e0] / SetStereoVisionRestriction(bool) [0x330940]: the module's setting.
 * A console without parental controls: configurable without a PIN (0), no restriction set; the setting the game makes
 * is kept for the session. */
static void oe_HomeButtonBlocking(Ctx *c) { (void)c; }
static void oe_EnableRecording(Ctx *c) { (void)c; }
static volatile LONG g_stereo_restricted;
static void pctl_StereoConfigurable(Ctx *c) { c->x[0] = 0; }
static void pctl_GetStereoVisionRestriction(Ctx *c) { c->x[0] = g_stereo_restricted != 0; }
static void pctl_SetStereoVisionRestriction(Ctx *c) { InterlockedExchange(&g_stereo_restricted, (LONG)(c->x[0] & 1)); }

static const ImportDef smo_imports[] = {
    {"_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_HandleNetworkRequestResult},
    {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z", diag_AbortImpl_result},
    {"_ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_i", diag_OnAssertionFailure},
    {"_ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz", diag_LogImpl},
    {"_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", diag_PutImpl},
    {"_ZN2nn4diag12GetBacktraceEPmi", diag_GetBacktrace},
    {"_ZN2nn4diag13GetSymbolNameEPcmm", diag_GetSymbolName}, {"_ZN2nn4diag13GetSymbolSizeEm", diag_GetSymbolSize},
    {"_ZN2nn4diag40GetRequiredBufferSizeForGetAllModuleInfoEv", diag_GetRequiredBufferSizeForGetAllModuleInfo},
    {"_ZN2nn4diag16GetAllModuleInfoEPPNS0_10ModuleInfoEPvm", diag_GetAllModuleInfo},
    {"_ZN2nn4time10InitializeEv", time_Initialize}, {"_ZN2nn4time13IsInitializedEv", time_IsInitialized}, {"_ZN2nn4time8FinalizeEv", time_Finalize},
    {"_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", oe_SetScreenShotImageOrientation},
    {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", oe_SetUserInactivityDetectionTimeExtended},
    {"_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", oe_IsUserInactivityDetectionTimeExtended},
    {"_ZN2nn2os17SetMemoryHeapSizeEm", os_SetMemoryHeapSize},
    {"_ZN2nn2os19AllocateMemoryBlockEPmm", os_AllocateMemoryBlock},
    {"_ZN2nn4diag18IsDebuggerAttachedEv", diag_IsDebuggerAttached}, {"_ZN2nn4diag6detail11NotNullImplEm", diag_NotNullImpl},
    {"_ZN2nn2os23SetUserExceptionHandlerEPFvPNS0_17UserExceptionInfoEEPvmS2_", os_SetUserExceptionHandler},
    {"_ZN2nn4diag29InitializeAbortObserverHolderEPNS0_19AbortObserverHolderEPFvRKNS0_9AbortInfoEE", diag_InitializeAbortObserverHolder},
    {"_ZN2nn4diag21RegisterAbortObserverEPNS0_19AbortObserverHolderE", diag_RegisterAbortObserver},
    {"_ZN2nn3hid18InitializeDebugPadEv", hid_InitializeDebugPad},
    {"_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", hid_GetDebugPadState},
    {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_SetLayerScalingMode},
    {"_ZN2nn2oe18GetPerformanceModeEv", oe_GetPerformanceMode},
    {"_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", oe_GetPerformanceConfiguration},
    {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_SetPerformanceConfiguration},
    {"_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_SetResumeNotificationEnabled},
    {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_SetPerformanceModeChangedNotificationEnabled},
    {"_ZN2nn5prepo10PlayReportC1EPKc", prepo_PlayReport_ctor_id},
    /* Labo VR and stereo vision (see above) */
    {"_ZN2nn2oe23BeginBlockingHomeButtonEv", oe_HomeButtonBlocking}, {"_ZN2nn2oe21EndBlockingHomeButtonEv", oe_HomeButtonBlocking},
    {"_ZN2nn2oe15EnableRecordingEv", oe_EnableRecording},
    {"_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv", pctl_StereoConfigurable},
    {"_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv", pctl_StereoConfigurable},
    {"_ZN2nn4pctl26GetStereoVisionRestrictionEv", pctl_GetStereoVisionRestriction},
    {"_ZN2nn4pctl26SetStereoVisionRestrictionEb", pctl_SetStereoVisionRestriction},
};

void register_smo_imports(void) {
    register_imports(smo_imports, (int)(sizeof smo_imports / sizeof smo_imports[0]));
    { void register_smo_audio(void); register_smo_audio(); }      /* smo_audio.c */
    {   /* below lifted SDK code (sdklift_rules.txt `cut`) */
        sdk_host_override("_ZN2nn2os6detail27TransferMemoryImplByHorizon6CreateEPjPvmNS0_16MemoryPermissionE", tm_Create);
        sdk_host_override("_ZN2nn2os6detail27TransferMemoryImplByHorizon5CloseEj", tm_Close);
        sdk_host_override("_ZN2nn2os6detail27TransferMemoryImplByHorizon3MapEPPvjS3_mNS0_16MemoryPermissionE", tm_Map);
        sdk_host_override("_ZN2nn2os6detail27TransferMemoryImplByHorizon5UnmapEjPvm", tm_Unmap);
        sdk_host_override("_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm", as_AllocateSpace);
        sdk_host_override("_ZN2nn2os6detail21AddressSpaceAllocator15CheckGuardSpaceEmmm", as_CheckGuardSpace);
    }
}
