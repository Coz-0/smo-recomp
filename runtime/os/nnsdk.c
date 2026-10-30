/* Host implementations of the Nintendo SDK functions the game imports.
 * Grown one function at a time from what the boot trace asks for. Each entry
 * names the mangled symbol; behaviour follows the public SDK semantics. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

static void keep(Ctx *c) { (void)c; }                   /* returns its first argument (this) unchanged */
static void ret0(Ctx *c) { RET(0); }                    /* nn::Result success, false, or null */

/* ---- nn::os: program arguments */
static uint64_t g_argv[2];
static void os_GetHostArgc(Ctx *c) { RET(0); }
static void os_GetHostArgv(Ctx *c) { RET(g_argv); }

/* ---- nn::os: time. The Switch system tick runs at 19.2 MHz. */
#define TICK_HZ 19200000ULL
/* Test aid SMO_TICK_CALLERS=1: every distinct return address of a caller of the clock functions is printed once (to find
 * what seeds the game's random numbers). */
void host_clock_caller(const char *what, uint64_t lr) {
    static int on = -1, n;
    static uint64_t seen[512];
    if (on < 0) on = getenv("SMO_TICK_CALLERS") != NULL;
    if (!on) return;
    for (int i = 0; i < n && i < 512; i++) if (seen[i] == lr) return;
    if (n < 512) { seen[n++] = lr; printf("clock caller: %s from %llx\n", what, (unsigned long long)lr); }
}
static void os_GetSystemTick(Ctx *c) {
    LARGE_INTEGER now, freq;
    host_clock_caller("GetSystemTick", c->x[30]);
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    RET((uint64_t)((double)now.QuadPart * (double)TICK_HZ / (double)freq.QuadPart));
}
static void os_GetSystemTickFrequency(Ctx *c) { RET(TICK_HZ); }

/* ---- nn::os: thread-local storage slots (single guest thread for now) */
static uint64_t g_tls_values[64];
static uint32_t g_tls_next;
static void os_AllocateTlsSlot(Ctx *c) { wr32(A0, g_tls_next < 63 ? g_tls_next++ : 63); RET(0); }
static void os_GetTlsValue(Ctx *c) { RET(g_tls_values[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { g_tls_values[A0 & 63] = A1; }

/* ---- nn::os: mutexes. One guest thread runs, so locking always succeeds. */
static void os_InitializeMutex(Ctx *c) { memset(P(0), 0, 0x20); }
static void os_TryLock(Ctx *c) { RET(1); }

/* ---- libc bits that live in the SDK */
static int g_errno;
static void c_errno_location(Ctx *c) { RET(&g_errno); }

/* ---- more libc */
static void c_strtol(Ctx *c) {
    char *end = NULL;
    long long v = strtoll(P(0), &end, (int)A2);
    if (A1) wr64(A1, (uint64_t)end);
    RET(v);
}

/* ---- nn::os: current thread (one guest thread for now) */
static uint8_t g_main_thread[0x400];
static void os_GetCurrentThread(Ctx *c) { RET(g_main_thread); }

/* ---- nn::oe: operating environment. Fixed answers: docked, Italian. */
/* 0 handheld, 1 console (docked). console_mode = handheld in config\graphics.ini (gfx_options.c) answers 0. */
static void oe_GetOperationMode(Ctx *c) {
    static int mode = -1;
    if (mode < 0) { mode = gfx_options()->handheld ? 0 : 1; printf("nn::oe::GetOperationMode: %s (graphics.ini console_mode), first asked from %llx\n", mode ? "docked" : "handheld", (unsigned long long)c->x[30]); }
    RET(mode);
}
static void oe_GetDesiredLanguage(Ctx *c) { RET(0x7469); }           /* LanguageCode "it" */
static void settings_LanguageEquals(Ctx *c) { RET(rd32(A1) == 4); }   /* nn::settings::Language_Italian */

/* ---- nn::fs: nothing is mounted yet; calls succeed so start-up can go on */
static void fs_QueryMountRomCacheSize(Ctx *c) { wr64(A0, 0x10000); RET(0); }

/* True when a printf format has a floating-point conversion (looks only at conversion
 * specifiers, so letters in literal text such as "uTexture%d" are ignored). */
static int format_has_float(const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*hlLqjzt", *p)) p++;
        if (*p && strchr("fFeEgGaA", *p)) return 1;
        if (!*p) break;
    }
    return 0;
}

/* ---- nn::util: NintendoWare binary files (BNSH shader archives and similar).
 * BinaryFileHeader (0x20): sig[8] @0, version u32 @8, BOM u16 @0xc, alignment(log2) u8 @0xe,
 * targetAddrSize u8 @0xf, nameOffset u32 @0x10, flags u16 @0x14 (bit0 = relocated),
 * firstBlockOffset u16 @0x16, relocationTableOffset u32 @0x18, fileSize u32 @0x1c.
 * RelocationTable: "_RLT" @0, position u32 @4, sectionCount u32 @8, pad @0xc; sections (0x18 each):
 * base u64, position u32, size u32, entryIndex u32, entryCount u32; entries (8 each):
 * position u32, structCount u16, offsetCount u8, padding u8. Relocation adds the section base
 * (the file address when 0) to each 8-byte pointer: structCount groups of offsetCount pointers,
 * skipping padding*8 bytes between groups. */
static void util_BinaryFileHeader_GetAlignment(Ctx *c) { RET(1u << rd8(A0 + 0xe)); }
static void util_BinaryFileHeader_IsRelocated(Ctx *c) { RET(rd16(A0 + 0x14) & 1); }
static void util_BinaryFileHeader_GetFirstBlock(Ctx *c) { uint32_t off = rd16(A0 + 0x16); RET(off ? A0 + off : 0); }
static void util_BinaryFileHeader_GetRelocationTable(Ctx *c) { uint32_t off = rd32(A0 + 0x18); RET(off ? A0 + off : 0); }
static void util_BinaryBlockHeader_GetBlockSize(Ctx *c) { RET(rd32(A0 + 0x8)); }
static void util_RelocationTable_Relocate(Ctx *c) {
    uint64_t rlt = A0;
    uint64_t file = rlt - rd32(rlt + 4);                                  /* table position is file-relative */
    uint32_t nsec = rd32(rlt + 8);
    uint64_t sections = rlt + 0x10, entries = sections + (uint64_t)nsec * 0x18;
    unsigned long long fixed = 0;
    for (uint32_t s = 0; s < nsec; s++) {
        uint64_t sec = sections + (uint64_t)s * 0x18;
        uint64_t base = rd64(sec);
        uint32_t first = rd32(sec + 0x10), count = rd32(sec + 0x14);
        if (base == 0) base = file;
        for (uint32_t e = first; e < first + count; e++) {
            uint64_t ent = entries + (uint64_t)e * 8;
            uint64_t p = file + rd32(ent);
            uint32_t structs = rd16(ent + 4), ptrs = rd8(ent + 6), pad = rd8(ent + 7);
            for (uint32_t k = 0; k < structs; k++) {
                for (uint32_t j = 0; j < ptrs; j++) {
                    uint64_t v = rd64(p);
                    if (v) { wr64(p, v + base); fixed++; }
                    p += 8;
                }
                p += (uint64_t)pad * 8;
            }
        }
    }
    wr16(file + 0x14, rd16(file + 0x14) | 1);                             /* mark relocated */
    trace_printf("     util relocate file %llx: %u sections, %llu pointers rebased\n", (unsigned long long)file, nsec, fixed);
}
static void util_ReferSymbol(Ctx *c) { (void)c; }                                      /* returns its argument */

/* ---- C formatting: integer/pointer arguments arrive in x2..x7 (variadic AArch64 ABI).
 * Formats with floating-point conversions would need v0..v7 and are refused loudly. */
static void c_sprintf(Ctx *c) {
    const char *fmt = (const char *)(uintptr_t)A1;
    if (format_has_float(fmt)) guest_trap("sprintf with a floating-point conversion is not supported", c->x[30] - 4);
    int n = snprintf((char *)(uintptr_t)A0, 4096, fmt, (unsigned long long)A2, (unsigned long long)A3,
                     (unsigned long long)A4, (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    RET((int64_t)n);
}
static void c_snprintf(Ctx *c) {
    const char *fmt = (const char *)(uintptr_t)A2;
    if (format_has_float(fmt)) guest_trap("snprintf with a floating-point conversion is not supported", c->x[30] - 4);
    int n = snprintf((char *)(uintptr_t)A0, (size_t)A1, fmt, (unsigned long long)A3, (unsigned long long)A4,
                     (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    RET((int64_t)n);
}

/* ---- nn::lmem frame heap: bump allocation over a caller-supplied region. The HeapHead lives at
 * the start of the region and is opaque to the game (it only passes it back). A negative alignment
 * would allocate from the tail; the game uses positive alignments only. */
typedef struct { uint64_t start, head, tail, end; } FrameHeap;
static void lmem_CreateFrameHeap(Ctx *c) {               /* (void *region, size_t size, int option) -> HeapHead* */
    FrameHeap *h = P(0);
    uint64_t start = A0, size = A1;
    if (!start || size < sizeof *h) { RET(0); return; }
    h->start = start;
    h->head = (start + sizeof *h + 15) & ~15ull;
    h->end = h->tail = start + size;
    RET(h);
}
static void lmem_AllocateFromFrameHeap(Ctx *c) {         /* (HeapHead *, size_t size, int alignment) -> void* */
    FrameHeap *h = P(0);
    uint64_t size = A1;
    int64_t align = (int64_t)(int32_t)A2;
    if (!h) { RET(0); return; }
    if (align < 0) {                                     /* from the tail */
        uint64_t a = (uint64_t)-align, p = (h->tail - size) & ~(a - 1);
        if (p < h->head) { RET(0); return; }
        h->tail = p;
        RET(p);
        return;
    }
    uint64_t a = align ? (uint64_t)align : 4, p = (h->head + a - 1) & ~(a - 1);
    if (p + size > h->tail) { RET(0); return; }
    h->head = p + size;
    RET(p);
}
static void lmem_DestroyFrameHeap(Ctx *c) { (void)c; }

/* ---- nn::Result helpers: value 0 is success; module = low 9 bits, description = next 13. */
static void result_IsFailure(Ctx *c) { RET((uint32_t)A0 != 0); }
static void result_IsSuccess(Ctx *c) { RET((uint32_t)A0 == 0); }
static void result_GetModule(Ctx *c) { RET((uint32_t)A0 & 0x1ff); }
static void result_GetDescription(Ctx *c) { RET(((uint32_t)A0 >> 9) & 0x1fff); }
#define RESULT_UNAVAILABLE 0x3e87aull                     /* some non-zero nn::Result; exact code not established */

/* ---- nn::bcat delivery cache, client side as in sdk.elf; the bcat system module is not in the dump.
 * [sdk] 0xc1dd0 MountDeliveryCacheStorage -> 0xc5380 ShimLibraryGlobal::MountDeliveryCacheStorage: a storage
 *   service already held (global +0x10) gives 0x87a (module 122, description 4); otherwise the Result of
 *   CreateDeliveryCacheStorageService, and the service is kept. [doc] suyu bcat/service_creator.cpp: the module
 *   answers success with the application's delivery cache directory, which is empty when nothing was ever
 *   delivered (a console that was never online).
 * [sdk] 0xc54c0 Unmount: aborts when no storage is held or a file / directory is still open, else releases it.
 * [sdk] 0xc5770 Enumerate: aborts when no storage is held; asks the service (command 10) for the names.
 *   [doc] suyu delivery_cache_storage_service.cpp: count = directories present = 0.
 * [sdk] 0xc2110 RequestSyncDeliveryCache(progress): aborts when the storage is mounted; when
 *   nn::nifm::IsAnyInternetRequestAccepted(GetClientId()) is false it returns 0x107a (module 122, description 8)
 *   WITHOUT asking the bcat module. No network request is ever accepted here (nifm: no network).
 * [sdk] 0xc1be0 DirectoryName::IsValid: 1..31 characters of A-Z a-z 0-9 _ - (nnonline.inc opens directories). */
#define BCAT_ALREADY_MOUNTED 0x87au
#define BCAT_INTERNET_REQUEST_NOT_ACCEPTED 0x107au
volatile LONG g_bcat_mounted;                             /* also read by nnonline.inc (DeliveryCacheDirectory / File Open) */
static void bcat_Mount(Ctx *c) {
    if (InterlockedCompareExchange(&g_bcat_mounted, 1, 0) != 0) { RET(BCAT_ALREADY_MOUNTED); return; }
    trace_printf("     nn::bcat::MountDeliveryCacheStorage -> mounted (empty delivery cache)\n");
    RET(0);
}
static void bcat_Unmount(Ctx *c) {
    if (InterlockedCompareExchange(&g_bcat_mounted, 0, 1) != 1)
        guest_trap("nn::bcat::UnmountDeliveryCacheStorage without a mounted storage (the SDK aborts, sdk 0xc5514)", c->x[30] - 4);
}
static void bcat_Enumerate(Ctx *c) {                         /* (int *outCount, DirectoryName *names, int max) */
    if (!g_bcat_mounted)
        guest_trap("nn::bcat::EnumerateDeliveryCacheDirectory without a mounted storage (the SDK aborts, sdk 0xc5800)", c->x[30] - 4);
    wr32(A0, 0);
    RET(0);
}
int bcat_dirname_valid(const char *s) {              /* [sdk] 0xc1be0 on the 32-byte copy */
    int n = 0;
    for (; n < 32 && s[n]; n++) {
        unsigned char ch = (unsigned char)s[n];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '-')) return 0;
    }
    return n != 0 && n != 32;
}
static uint32_t bcat_sync_common(Ctx *c) {
    if (g_bcat_mounted)
        guest_trap("nn::bcat::RequestSyncDeliveryCache while the delivery cache storage is mounted (the SDK aborts, sdk 0xc2218)", c->x[30] - 4);
    return BCAT_INTERNET_REQUEST_NOT_ACCEPTED;
}
static void bcat_RequestSync(Ctx *c) { RET(bcat_sync_common(c)); }               /* (DeliveryCacheProgress *) */
int bcat_selftest(Ctx *c) {
    static const struct { const char *name; int valid; } t[] = {
        {"", 0}, {"a", 1}, {"spirits_event-01", 1}, {"with space", 0}, {"dot.name", 0},
        {"abcdefghijklmnopqrstuvwxyz01234", 1}, {"abcdefghijklmnopqrstuvwxyz012345", 0},
    };
    int ok = 1;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++)
        if ((strnlen(t[i].name, 32) < 32 && bcat_dirname_valid(t[i].name)) != t[i].valid) ok = 0;
    printf("  bcat: directory name rule of sdk 0xc1be0 (7 names): %s\n", ok ? "ok" : "FAILED");
    {
        int32_t count = 7;
        int ok2 = 1;
        uint64_t lr = c->x[30];
        bcat_RequestSync(c); ok2 &= (uint32_t)c->x[0] == BCAT_INTERNET_REQUEST_NOT_ACCEPTED;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == 0;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == BCAT_ALREADY_MOUNTED;
        c->x[0] = (uint64_t)(uintptr_t)&count; c->x[1] = 0; c->x[2] = 100; bcat_Enumerate(c); ok2 &= (uint32_t)c->x[0] == 0 && count == 0;
        bcat_Unmount(c); ok2 &= g_bcat_mounted == 0;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == 0;
        bcat_Unmount(c);
        c->x[30] = lr;
        printf("  bcat: sync refused 0x107a, mount, second mount 0x87a, 0 directories, unmount, mount again: %s\n", ok2 ? "ok" : "FAILED");
        ok &= ok2;
    }
    return !ok;
}
static void bcat_DirectoryRead(Ctx *c) { if (A1) wr32(A1, 0); RET(0); }      /* (this, int *outCount, entries, max) */
static void bcat_unavailable(Ctx *c) { RET(RESULT_UNAVAILABLE); }

/* ---- nn::mem::StandardAllocator: backed by the host heap. The region the game hands to
 * Initialize is left untouched; its size is only remembered for GetTotalFreeSize. */
static uint64_t g_std_alloc_size = 0x10000000;
static void stdalloc_Initialize(Ctx *c) { g_std_alloc_size = A2; }
static void stdalloc_Allocate(Ctx *c) { RET(_aligned_malloc(A1 ? (size_t)A1 : 1, 16)); }
static void stdalloc_AllocateAligned(Ctx *c) { RET(_aligned_malloc(A1 ? (size_t)A1 : 1, A2 < 16 ? 16 : (size_t)A2)); }
static void stdalloc_Free(Ctx *c) { _aligned_free((void *)(uintptr_t)A1); }
static void stdalloc_Reallocate(Ctx *c) { RET(_aligned_realloc((void *)(uintptr_t)A1, A2 ? (size_t)A2 : 1, 16)); }

/* ---- nn::util string and bit helpers */
static void util_SNPrintf(Ctx *c) {                       /* (char *buf, size_t size, const char *fmt, ...) */
    const char *fmt = (const char *)(uintptr_t)A2;
    /* SMO_SNPRINTF_LOG=1: how often the game formats and what one call costs. A line per 2,000 calls:
     * the frame, the time those calls took, the slowest one with its format and caller. */
    static int lg = -1; static LONG64 n_calls, tk_sum, tk_max; static char max_fmt[64]; static uint64_t max_ra;
    if (lg < 0) lg = getenv("SMO_SNPRINTF_LOG") && atoi(getenv("SMO_SNPRINTF_LOG"));
    LARGE_INTEGER q0, q1;
    if (lg) QueryPerformanceCounter(&q0);
    if (format_has_float(fmt)) guest_trap("nn::util::SNPrintf with a floating-point conversion is not supported", c->x[30] - 4);
    int n = snprintf((char *)(uintptr_t)A0, (size_t)A1, fmt, (unsigned long long)A3, (unsigned long long)A4,
                     (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    if (lg) {
        extern long long nvn_front_frame_next(void);
        QueryPerformanceCounter(&q1);
        LONG64 d = q1.QuadPart - q0.QuadPart;
        tk_sum += d;
        if (d > tk_max) { tk_max = d; max_ra = c->x[30]; snprintf(max_fmt, sizeof max_fmt, "%.60s", fmt); }
        {   static LONG nslow; LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            if (d * 5000 > fq.QuadPart && InterlockedIncrement(&nslow) <= 300)      /* one call over 0.2 ms */
                printf("snprintf_log: SLOW call, frame %lld: %.3f ms, format \"%.60s\" (%llu characters), buffer size %llu, result %d, called from %llx\n", nvn_front_frame_next(), (double)d * 1000.0 / (double)fq.QuadPart, fmt, (unsigned long long)strlen(fmt), (unsigned long long)A1, n, (unsigned long long)c->x[30]);
        }
        if (++n_calls % 2000 == 0) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            printf("snprintf_log: frame %lld: %lld calls so far, the last 2000 took %.2f ms, slowest %.3f ms (format \"%s\", buffer size %llu, called from %llx); this one: \"%.40s\" size %llu from %llx\n", nvn_front_frame_next(), (long long)n_calls,
                   (double)tk_sum * 1000.0 / (double)fq.QuadPart, (double)tk_max * 1000.0 / (double)fq.QuadPart, max_fmt, (unsigned long long)A1, (unsigned long long)max_ra, fmt, (unsigned long long)A1, (unsigned long long)c->x[30]);
            tk_sum = tk_max = 0;
        }
    }
    RET((int64_t)n);
}
static void util_ConvertCharacterUtf8ToUtf32(Ctx *c) {    /* (uint32_t *out, const char *s) -> bytes consumed, or 0 */
    const uint8_t *s = (const uint8_t *)(uintptr_t)A1;
    uint32_t cp; int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xe0) == 0xc0) { cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); n = 2; }
    else if ((s[0] & 0xf0) == 0xe0) { cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); n = 3; }
    else if ((s[0] & 0xf8) == 0xf0) { cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f); n = 4; }
    else { RET(0); return; }
    wr32(A0, cp);
    RET(n);
}
static void util_PickOutCharacterFromUtf8String(Ctx *c) { /* (char out[4], const char **cursor) -> bytes, advances cursor */
    const uint8_t *s = (const uint8_t *)(uintptr_t)rd64(A1);
    int n = s[0] < 0x80 ? 1 : (s[0] & 0xe0) == 0xc0 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 ? 4 : 0;
    if (!n) { RET(0); return; }
    memcpy((void *)(uintptr_t)A0, s, (size_t)n);
    wr64(A1, (uint64_t)(uintptr_t)(s + n));
    RET(n);
}

static const ImportDef sdk_imports[] = {
    {"nnResultIsFailure", result_IsFailure}, {"nnResultIsSuccess", result_IsSuccess},
    {"nnResultGetModule", result_GetModule}, {"nnResultGetDescription", result_GetDescription},
    {"_ZN2nn4bcat10InitializeEv", ret0}, {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", bcat_Mount},
    {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", bcat_Unmount},
    {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", bcat_Enumerate},
    {"_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", keep}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", keep},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", bcat_unavailable},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", bcat_DirectoryRead},
    {"_ZN2nn4bcat17DeliveryCacheFileC1Ev", keep}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", keep},
    {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", keep},
    {"_ZN2nn4bcat21DeliveryCacheProgressC1Ev", keep}, {"_ZN2nn4bcat21DeliveryCacheProgressD1Ev", keep},
    {"_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", keep},
    {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", bcat_RequestSync},
    {"_ZN2nn7friends10InitializeEv", ret0}, {"_ZN2nn7friends7ProfileC1Ev", keep},
    {"_ZN2nn7friends12AsyncContextC1Ev", keep},
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvm", stdalloc_Initialize},
    /* the game imports this overload: the bool turns on a per-thread cache (stored at +1, a TLS slot
     * allocated, [sdk 7.3.2 0x2de1d0]); the host heap has no such cache, so it is the same call */
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvmb", stdalloc_Initialize},
    {"_ZN2nn3mem17StandardAllocator8FinalizeEv", keep},
    {"_ZN2nn3mem17StandardAllocator8AllocateEm", stdalloc_Allocate},
    {"_ZN2nn3mem17StandardAllocator8AllocateEmm", stdalloc_AllocateAligned},
    {"_ZN2nn3mem17StandardAllocator4FreeEPv", stdalloc_Free},
    {"_ZN2nn3mem17StandardAllocator10ReallocateEPvm", stdalloc_Reallocate},
    {"_ZN2nn4util8SNPrintfEPcmPKcz", util_SNPrintf},
    {"_ZN2nn4util27ConvertCharacterUtf8ToUtf32EPjPKc", util_ConvertCharacterUtf8ToUtf32},
    {"_ZN2nn4util30PickOutCharacterFromUtf8StringEPcPPKc", util_PickOutCharacterFromUtf8String},
    {"_ZN2nn4lmem15CreateFrameHeapEPvmi", lmem_CreateFrameHeap},
    {"_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi", lmem_AllocateFromFrameHeap},
    {"_ZN2nn4lmem16DestroyFrameHeapEPNS0_6detail8HeapHeadE", lmem_DestroyFrameHeap},
    {"_ZNK2nn4util16BinaryFileHeader12GetAlignmentEv", util_BinaryFileHeader_GetAlignment},
    {"_ZNK2nn4util16BinaryFileHeader11IsRelocatedEv", util_BinaryFileHeader_IsRelocated},
    {"_ZN2nn4util16BinaryFileHeader13GetFirstBlockEv", util_BinaryFileHeader_GetFirstBlock},
    {"_ZNK2nn4util16BinaryFileHeader13GetFirstBlockEv", util_BinaryFileHeader_GetFirstBlock},   /* const overload */
    {"_ZN2nn4util16BinaryFileHeader18GetRelocationTableEv", util_BinaryFileHeader_GetRelocationTable},
    {"_ZNK2nn4util17BinaryBlockHeader12GetBlockSizeEv", util_BinaryBlockHeader_GetBlockSize},
    {"_ZN2nn4util15RelocationTable8RelocateEv", util_RelocationTable_Relocate},
    {"_ZN2nn4util11ReferSymbolEPKv", util_ReferSymbol},
    {"sprintf", c_sprintf}, {"snprintf", c_snprintf},
    {"_ZN2nn2oe10InitializeEv", keep},
    {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", keep},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", keep},
    {"_ZN2nn2oe16DisableRecordingEv", keep},
    {"_ZN2nn2oe16GetOperationModeEv", oe_GetOperationMode},
    {"_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", settings_LanguageEquals},
    {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", fs_QueryMountRomCacheSize},
    {"_ZN2nn2fs8MountRomEPKcPvm", ret0},
    {"_ZN2nn4time13IsInitializedEv", ret0},
    {"_ZN2nn4time10InitializeEv", ret0},
    {"_ZN2nn2os16GetCurrentThreadEv", os_GetCurrentThread},
    {"_ZN2nn2os20GetCurrentCoreNumberEv", ret0},
    {"strtol", c_strtol},
    {"_ZN2nn3mem17StandardAllocatorC1Ev", keep},
    {"_ZN2nn2os11GetHostArgcEv", os_GetHostArgc},
    {"_ZN2nn2os11GetHostArgvEv", os_GetHostArgv},
    {"_ZN2nn2os13GetSystemTickEv", os_GetSystemTick},
    {"_ZN2nn2os22GetSystemTickFrequencyEv", os_GetSystemTickFrequency},
    {"_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE", os_AllocateTlsSlot},
    {"_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", os_GetTlsValue},
    {"_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", os_SetTlsValue},
    {"_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", os_InitializeMutex},
    {"_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE", os_TryLock},
    {"__errno_location", c_errno_location},
};

void register_sdk_imports(void) {
    register_imports(sdk_imports, (int)(sizeof sdk_imports / sizeof sdk_imports[0]));
}
