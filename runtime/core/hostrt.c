/* Host runtime for the fully lifted game: image loader, relocation, indirect
 * call dispatch, import dispatch with tracing, and crash reporting. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#include <signal.h>
#include <unistd.h>
#include <sys/ucontext.h>
#ifdef __APPLE__
#include <mach/thread_status.h>
#endif
#endif
#include "runtime.h"
#include "hostrt.h"
#include "audit_a64.h"
#include "core_model.h"

/* the image constants come from build/gen/image.h (generated from the dump) */
#define FILE_OFF 0x1000ULL
#define FILE_IMAGE GUEST_FILE_IMAGE
#define IMAGE_SIZE GUEST_IMAGE_SIZE
#define RELA GUEST_RELA
#define RELASZ GUEST_RELASZ
#define JMPREL GUEST_JMPREL
#define PLTRELSZ GUEST_PLTRELSZ

static ImportFn *g_handlers;
static unsigned long long *g_import_calls;
static int *g_import_order, g_import_order_count;
static FILE *g_trace;
static unsigned long long g_trace_seq;
static FnEntry *g_by_host;                 /* fn_table sorted by host code address */
void sdk_dispatch(Ctx *c, uint64_t addr);          /* sdklift_rt.c: code lifted from sdk.elf at 0x75xxxxxxxx */

void trace_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_trace) { vfprintf(g_trace, fmt, ap); fflush(g_trace); }
    va_end(ap);
}

/* ---------------------------------------------------------------- lookup */
/* guest address -> lifted function through a hash table built in runtime_init (open addressing, load below 1/2);
 * before that, and as the reference, the binary search over fn_table (sorted by address). Same answer either way. */
static struct { uint64_t addr; void (*fn)(Ctx *); } *g_fn_hash;
static unsigned g_fn_mask;
static inline unsigned fn_hash(uint64_t addr) { return (unsigned)(((addr >> 2) * 0x9E3779B97F4A7C15ULL) >> 40); }
static void fn_hash_build(void) {
    unsigned size = 1024;
    while (size < (unsigned)fn_table_count * 2u) size <<= 1;
    g_fn_hash = calloc(size, sizeof *g_fn_hash);
    for (int i = 0; i < fn_table_count; i++) {
        unsigned h = fn_hash(fn_table[i].addr) & (size - 1);
        while (g_fn_hash[h].addr) h = (h + 1) & (size - 1);
        g_fn_hash[h].addr = fn_table[i].addr; g_fn_hash[h].fn = fn_table[i].fn;
    }
    g_fn_mask = size - 1;
}
static void (*lookup_guest(uint64_t addr))(Ctx *) {
    if (g_fn_mask) {
        for (unsigned h = fn_hash(addr) & g_fn_mask;; h = (h + 1) & g_fn_mask) {
            if (g_fn_hash[h].addr == addr) return g_fn_hash[h].fn;
            if (!g_fn_hash[h].addr) return NULL;
        }
    }
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == addr) return fn_table[mid].fn;
        if (fn_table[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const FnEntry *)a)->fn, y = (uintptr_t)((const FnEntry *)b)->fn;
    return x < y ? -1 : x > y;
}

/* Guest function whose host code contains `rip`, or 0. */
uint64_t guest_function_of(uint64_t rip) {
    int lo = 0, hi = fn_table_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((uintptr_t)g_by_host[mid].fn <= rip) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0 || rip - (uintptr_t)g_by_host[best].fn > 0x400000) return 0;
    return g_by_host[best].addr;
}

/* ---------------------------------------------------------------- host functions handed to the guest
 * Function pointers the guest obtains at run time (for example through nvnBootstrapLoader) are
 * placeholder addresses in their own range; calling one runs the registered host handler. */
typedef struct { char name[72]; ImportFn fn; volatile LONG64 calls; LONG64 first; } HostFn;
#define MAX_HOSTFN 4096
static HostFn g_hostfn[MAX_HOSTFN];
static volatile LONG g_hostfn_count;
static volatile LONG64 g_hostfn_seq;
static SRWLOCK g_hostfn_lock = SRWLOCK_INIT;
static __declspec(thread) int t_hostfn;

static void audit_hostfn(const char *name, ImportFn fn);
static void audit_exit_counts(void);
uint64_t hostfn_address(const char *name, ImportFn fn) {
    AcquireSRWLockExclusive(&g_hostfn_lock);
    int i, fresh = 0;
    for (i = 0; i < g_hostfn_count; i++) if (strcmp(g_hostfn[i].name, name) == 0) break;
    if (i == g_hostfn_count && i < MAX_HOSTFN) {
        strncpy(g_hostfn[i].name, name, sizeof g_hostfn[i].name - 1);
        g_hostfn[i].fn = fn;
        g_hostfn_count = i + 1;
        fresh = 1;
    }
    ReleaseSRWLockExclusive(&g_hostfn_lock);
    if (fresh) audit_hostfn(name, fn);
    return HOSTFN_BASE + (uint64_t)i * 16;
}
const char *hostfn_current_name(void) { return g_hostfn[t_hostfn].name; }
unsigned long long hostfn_current_calls(void) { return (unsigned long long)g_hostfn[t_hostfn].calls; }

void print_hostfn_summary(FILE *out) {
    int n = g_hostfn_count, used = 0;
    int *order = malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) if (g_hostfn[i].calls) order[used++] = i;
    for (int i = 1; i < used; i++)
        for (int j = i; j > 0 && g_hostfn[order[j]].first < g_hostfn[order[j - 1]].first; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    fprintf(out, "\nrun-time host functions: %d handed out, %d called, in order of first call:\n", n, used);
    for (int i = 0; i < used; i++) fprintf(out, "  %8lld  %s\n", (long long)g_hostfn[order[i]].calls, g_hostfn[order[i]].name);
    free(order);
}

/* Called on every failed exclusive store; reports a loop that never succeeds. */
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    if (++c->ex_fail == 1000 || c->ex_fail == 100000) {
        printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)\n",
               (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
               (unsigned long long)seen, (unsigned long long)v, bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
        trace_printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx\n",
                     (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
                     (unsigned long long)seen, (unsigned long long)v);
    }
}

/* Debug aid: SMO_ICALLS=<start s>,<dur s> records every distinct (indirect call target, return address, context)
 * seen in that window with a count and the first x0..x2; icall_dump() prints them (boot.c, at the time limit). */
#define ICALL_MAX 16384
static struct { uint64_t target, lr, ctx, x0, x1, x2; long long n; } g_icall[ICALL_MAX];
static volatile LONG g_icall_lock;
static int g_icall_mode = -1;
static ULONGLONG g_icall_start, g_icall_end;
static unsigned long long g_poke_fn, g_poke_off;
static unsigned g_poke_val;
static void icall_record(Ctx *c, uint64_t addr) {
    if (g_icall_mode < 0) {
        int s = 0, d = 0;
        const char *e = getenv("SMO_ICALLS");
        g_icall_mode = e && sscanf(e, "%d,%d", &s, &d) == 2 && d > 0;
        g_icall_start = GetTickCount64() + (ULONGLONG)s * 1000;
        g_icall_end = g_icall_start + (ULONGLONG)d * 1000;
        /* Experiment only (changes game DATA, never use for normal runs): SMO_EXPERIMENT_POKE=<fn hex>:<off hex>=<byte hex>
         * writes that byte at x0+off each time fn is called indirectly. */
        e = getenv("SMO_EXPERIMENT_POKE");
        if (e && sscanf(e, "%llx:%llx=%x", &g_poke_fn, &g_poke_off, &g_poke_val) == 3) { if (!g_icall_mode) g_icall_mode = 2; }
        else g_poke_fn = 0;
    }
    if (g_poke_fn && addr == g_poke_fn && rd8(c->x[0] + g_poke_off) != (uint8_t)g_poke_val) {
        printf("[poke] f_%llx: byte at x0+0x%llx (%llx) %02x -> %02x\n", g_poke_fn, g_poke_off, (unsigned long long)(c->x[0] + g_poke_off),
               rd8(c->x[0] + g_poke_off), g_poke_val);
        wr8(c->x[0] + g_poke_off, (uint8_t)g_poke_val);
    }
    if (g_icall_mode != 1) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_icall_start || now > g_icall_end) return;
    uint64_t lr = c->x[30], key = addr * 31 + lr * 7 + (uint64_t)(uintptr_t)c;
    unsigned h = (unsigned)((key >> 2) * 2654435761u) % ICALL_MAX;
    while (InterlockedCompareExchange(&g_icall_lock, 1, 0)) YieldProcessor();
    for (int probe = 0; probe < ICALL_MAX; probe++, h = (h + 1) % ICALL_MAX) {
        if (g_icall[h].target == addr && g_icall[h].lr == lr && g_icall[h].ctx == (uint64_t)(uintptr_t)c) { g_icall[h].n++; break; }
        if (!g_icall[h].target) {
            g_icall[h].target = addr; g_icall[h].lr = lr; g_icall[h].ctx = (uint64_t)(uintptr_t)c;
            g_icall[h].x0 = c->x[0]; g_icall[h].x1 = c->x[1]; g_icall[h].x2 = c->x[2]; g_icall[h].n = 1;
            break;
        }
    }
    g_icall_lock = 0;
}
uint64_t icall_first_x0(uint64_t target) {
    for (int i = 0; i < ICALL_MAX; i++) if (g_icall[i].target == target) return g_icall[i].x0;
    return 0;
}
void icall_dump(FILE *out, uint64_t main_ctx) {
    if (g_icall_mode <= 0) return;
    fprintf(out, "\n[icall] indirect calls seen in the window (M = main thread)\n");
    for (int i = 0; i < ICALL_MAX; i++)
        if (g_icall[i].target)
            fprintf(out, "[icall] %c f_%llx lr=%llx n=%lld x0=%llx x1=%llx x2=%llx\n", g_icall[i].ctx == main_ctx ? 'M' : '-',
                    (unsigned long long)g_icall[i].target, (unsigned long long)g_icall[i].lr, g_icall[i].n,
                    (unsigned long long)g_icall[i].x0, (unsigned long long)g_icall[i].x1, (unsigned long long)g_icall[i].x2);
}

extern int g_nerve_mode;                              /* smo_nerve.c */
extern volatile uint64_t g_nerve_watch[2], g_stage_init, g_stage_play;
void nerve_call(Ctx *c, uint64_t addr);
void nerve_watch_hit(void);
void stage_init_hit(Ctx *c);
void stage_play_hit(Ctx *c);

void call_indirect(Ctx *c, uint64_t addr) {
    CM_SAFE_POINT();                                 /* macOS: the console core model may take the core here (os/core_model.c) */
    if (g_icall_mode) icall_record(c, addr);
    if (g_nerve_mode) nerve_call(c, addr);             /* smo_nerve.c: SMO_NERVELOG, SMO_ACTORPOS, SMO_SEQ @nerve: */
    if (g_nerve_watch[0] && (addr == g_nerve_watch[0] || addr == g_nerve_watch[1])) nerve_watch_hit();   /* smo_nerve.c: the amiibo menu's watch */
    if (g_stage_init && addr == g_stage_init) stage_init_hit(c);      /* smo_nerve.c: SMO_START_STAGE, SMO_STAGE_LIST (TEST ONLY): a stage scene starts */
    if (g_stage_play && addr == g_stage_play) stage_play_hit(c);      /* smo_nerve.c: SMO_STAGE_LIST (TEST ONLY): the stage change to the next stage */
    if (addr - GUEST_BASE < IMAGE_SIZE) {            /* the common case first (a function of main); the ranges tested below do not overlap it */
        void (*fn)(Ctx *) = lookup_guest(addr);
        if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
        fn(c);
        return;
    }
    if (addr >= HOSTFN_BASE && addr < HOSTFN_BASE + (uint64_t)g_hostfn_count * 16) {
        int i = (int)((addr - HOSTFN_BASE) / 16);
        if (g_hostfn[i].calls++ == 0) g_hostfn[i].first = InterlockedIncrement64(&g_hostfn_seq);      /* not atomic: 3 game threads made every NVN call a locked add on shared lines; the counts are for reports and traces */
        t_hostfn = i;
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = g_hostfn[i].name;             /* so thread dumps can name the host function */
        c->import_from = c->x[30] - 4;
        g_hostfn[i].fn(c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        return;
    }
    if (addr >= IMPORT_BASE && addr < IMPORT_BASE + (uint64_t)dyn_sym_count * IMPORT_SLOT) {
        host_import(c, (int)((addr - IMPORT_BASE) / IMPORT_SLOT));
        return;
    }
    if (addr >= SDK_BASE && addr < SDK_BASE + SDK_IMAGE_SIZE_GEN) { sdk_dispatch(c, addr); return; }   /* lifted SDK code (sdklift_rt.c), e.g. a vtable entry of an SDK object */
    void (*fn)(Ctx *) = lookup_guest(addr);
    if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
    fn(c);
}

/* Register context of the guest code running on this host thread, for crash reports. */
static __declspec(thread) Ctx *t_cur_ctx;

/* debug aid SMO_WATCH=<heap offset>[,<bytes>]: every write to that range of the nn::os memory heap is logged
 * (guest function last entered, its x30, host module + offset, the value after the write). The page is made read-only;
 * a write faults, is logged, runs once with the page writable (single step) and the page is protected again. Writes by
 * another thread during that one step are not seen. Armed by smo_nn.c when the heap is made. At most 400 lines. */
static uint64_t g_watch_lo, g_watch_hi, g_watch_page, g_watch_off;
static volatile LONG g_watch_lines;
static __declspec(thread) int t_watch_step, t_watch_hit;
#ifdef _WIN32
static void watch_where(const char *what, CONTEXT *cx) {
    Ctx *c = t_cur_ctx;
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (InterlockedIncrement(&g_watch_lines) > 400) return;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)cx->Rip, &m) && m) GetModuleFileNameA(m, mod, sizeof mod);
    const char *base = strrchr(mod, '\\');
    printf("watch: %s heap+0x%llx..: %016llx %016llx | guest f_%llx lr %llx | host %s+0x%llx | thread %lu\n", what,
           (unsigned long long)g_watch_off, (unsigned long long)rd64(g_watch_lo), (unsigned long long)rd64(g_watch_lo + 8),
           (unsigned long long)(c ? c->fn : 0), (unsigned long long)(c ? c->x[30] : 0), base ? base + 1 : mod, (unsigned long long)(cx->Rip - (uintptr_t)m), GetCurrentThreadId());
    fflush(stdout);
}
static LONG WINAPI watch_veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->ExceptionInformation[0] == 1 && (er->ExceptionInformation[1] & ~0xfffull) == g_watch_page) {
        uint64_t a = er->ExceptionInformation[1];
        t_watch_hit = a >= g_watch_lo && a < g_watch_hi;
        VirtualProtect((void *)(uintptr_t)g_watch_page, 0x1000, PAGE_READWRITE, &old);
        t_watch_step = 1;
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_step) {
        t_watch_step = 0;
        if (t_watch_hit) {                      /* only writes that change the watched bytes (a loop rewriting one value would fill the log) */
            static uint64_t last[2] = {~0ull, ~0ull};
            uint64_t now0 = rd64(g_watch_lo), now1 = rd64(g_watch_lo + 8);
            if (now0 != last[0] || now1 != last[1]) { last[0] = now0; last[1] = now1; watch_where("write", ep->ContextRecord); }
        }
        VirtualProtect((void *)(uintptr_t)g_watch_page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void smo_watch_arm(uint8_t *heap, uint64_t heap_size) {
    const char *e = getenv("SMO_WATCH");
    if (!e || g_watch_page) return;
    char *end;
    uint64_t off = strtoull(e, &end, 0), len = *end == ',' ? strtoull(end + 1, NULL, 0) : 8;
    if (off + len > heap_size || len == 0 || ((off + len - 1) & ~0xfffull) != (off & ~0xfffull)) { printf("watch: SMO_WATCH=%s is not one range inside one page of the heap\n", e); return; }
    g_watch_off = off; g_watch_lo = (uint64_t)(uintptr_t)heap + off; g_watch_hi = g_watch_lo + len; g_watch_page = g_watch_lo & ~0xfffull;
    AddVectoredExceptionHandler(1, watch_veh);
    DWORD old;
    VirtualProtect((void *)(uintptr_t)g_watch_page, 0x1000, PAGE_READONLY, &old);
    printf("watch: armed on heap+0x%llx, %llu bytes\n", (unsigned long long)off, (unsigned long long)len);
}
#else
/* The Windows version steps the faulting write once with the page writable (the trap flag); a signal handler on
 * macOS cannot single-step, so SMO_WATCH is not available here yet (on Linux it could, with the trap flag in the
 * signal context: not written yet). */
void smo_watch_arm(uint8_t *heap, uint64_t heap_size) {
    (void)heap; (void)heap_size;
    (void)g_watch_lo; (void)g_watch_hi; (void)g_watch_page; (void)g_watch_off; (void)g_watch_lines; (void)t_watch_step; (void)t_watch_hit;
    if (getenv("SMO_WATCH")) printf("watch: SMO_WATCH is only available on Windows yet\n");
}
#endif

void call_guest(Ctx *c, uint64_t addr) {
    Ctx *saved = t_cur_ctx;
    t_cur_ctx = c;
    call_indirect(c, addr);
    t_cur_ctx = saved;
}

static void dump_guest_registers(void) {
    Ctx *c = t_cur_ctx;
    if (!c) return;
    printf("last guest function entered: f_%llx\n", (unsigned long long)c->fn);
    printf("guest registers:\n");
    for (int i = 0; i < 31; i++)
        printf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 3 == 2 ? "\n" : "");
    printf("  sp  %016llx\n", (unsigned long long)c->x[31]);
}

/* ---------------------------------------------------------------- imports */
/* Every handler registered, whether the game imports its name or not: an SDK C wrapper may forward to a C++
 * name the game does not import itself (the game imports nnosGetThreadCoreMask, not nn::os::GetThreadCoreMask). The pairs
 * are copied (a caller may pass an ImportDef of its own stack); the names are the callers' lasting strings. */
static struct { const char *name; ImportFn fn; } *g_known;
static int g_known_count, g_known_cap;

void register_imports(const ImportDef *defs, int count) {
    for (int d = 0; d < count; d++) {
        if (!defs[d].name) continue;
        if (g_known_count == g_known_cap) {
            int cap = g_known_cap ? 2 * g_known_cap : 8192;
            void *p = realloc(g_known, (size_t)cap * sizeof *g_known);
            if (p) { g_known = p; g_known_cap = cap; }
        }
        if (g_known_count < g_known_cap) { g_known[g_known_count].name = defs[d].name; g_known[g_known_count].fn = defs[d].fn; g_known_count++; }
    }
    for (int d = 0; d < count; d++) {
        int found = 0;
        for (int i = 0; i < dyn_sym_count; i++) {
            if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, defs[d].name) == 0) { g_handlers[i] = defs[d].fn; found = 1; }
        }
        (void)found;
    }
}

/* Handler currently registered for an imported function, or NULL (not imported / still a stub). */
ImportFn import_lookup(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return g_handlers[i];
    return NULL;
}

/* The handler last registered under a name, imported by the game or not; NULL if no table has it. */
ImportFn handler_by_name(const char *name) {
    for (int k = g_known_count - 1; k >= 0; k--)          /* the last registration wins, as in register_imports */
        if (strcmp(g_known[k].name, name) == 0) return g_known[k].fn;
    return NULL;
}

static uint64_t *g_import_addr;                    /* per-symbol address override, 0 = use the placeholder */
void import_set_address(const char *name, void *addr) {
    if (!g_import_addr) g_import_addr = calloc((size_t)dyn_sym_count, sizeof *g_import_addr);
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) g_import_addr[i] = (uint64_t)(uintptr_t)addr;
}

/* Address of the placeholder that stands for a data symbol of another module (IMPORT_SLOT bytes,
 * zero-filled), so the host can give it its real value. NULL if the game does not import it. */
void *import_data(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0)
            return (void *)(uintptr_t)(IMPORT_BASE + (uint64_t)i * IMPORT_SLOT);
    return NULL;
}

/* What the relocations of main use for imported symbol `sym`: its redirected address or its placeholder. */
uint64_t import_address(int sym) {
    return g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT;
}

static int audit_strict_on(void);
/* exact call counts cost two interlocked increments on shared cache lines per import call (290 million calls in a
 * 240 s run, from every thread). They are kept only when SMO_IMPORT_COUNTS=1 or SMO_AUDIT_COUNTS is set;
 * otherwise an import is counted (and traced) for its first three calls, as before, and then called directly. */
static int g_import_count_all = 1;
/* suspension of the application by the applet manager (nnmisc.c oe_watch, with graphics.ini unfocused = pause; suyu am/applet.cpp
 * UpdateSuspensionStateLocked -> process->Suspend). While g_guest_frozen is set every guest thread stops at its next call of, or return from, an SDK function and
 * waits here (the console's kernel stops the threads wherever they are; a thread inside a kernel call finishes it there too). */
volatile LONG g_guest_frozen;
void guest_freeze_wait(void) { while (g_guest_frozen) Sleep(5); }
/* SMO_IMPORT_WATCH=<part of an import's name>: the calls of that one import are timed, and for every
 * frame in which they took more than 2 ms a line says how many there were, what they cost, the slowest one (with the text
 * its third argument points at when it is a printable string: the format of a formatting function) and how many different
 * return addresses called it. Off: one compare per import call. */
static void host_import_real(Ctx *c, int sym);
static int g_watch_sym = -2;
void host_import(Ctx *c, int sym) {
    CM_SAFE_POINT();                                 /* macOS: the console core model may take the core here (os/core_model.c) */
    if (g_watch_sym == -2) {
        const char *e = getenv("SMO_IMPORT_WATCH");
        int w = -1;
        if (e && e[0]) for (int i = 0; i < (int)dyn_sym_count; i++) if (strstr(dyn_syms[i].name, e)) { w = i; break; }
        if (w >= 0) printf("import_watch: watching \"%s\"\n", dyn_syms[w].name);
        g_watch_sym = w;
    }
    if (sym != g_watch_sym) { host_import_real(c, sym); return; }
    extern long long nvn_front_frame_next(void);
    static long long cur_frame, n, tks, max_tk, n_all; static char max_txt[72]; static uint64_t max_ra, ras[64]; static int nras; static uint64_t max_x1;
    static volatile LONG lock;
    LARGE_INTEGER q0, q1, fq;
    uint64_t ra = c->x[30], x1 = c->x[1], x2 = c->x[2];
    char txt[72]; txt[0] = 0;
    {   MEMORY_BASIC_INFORMATION mi;               /* the third argument as text, when it is one (read BEFORE the call: the registers change) */
        if (x2 > 0x10000 && VirtualQuery((const void *)(uintptr_t)x2, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize > x2 + 80) {
            const unsigned char *s = (const unsigned char *)(uintptr_t)x2; int k = 0;
            for (; k < 70 && s[k] >= 32 && s[k] < 127; k++) txt[k] = (char)s[k];
            txt[k] = 0;
        }
    }
    QueryPerformanceCounter(&q0);
    host_import_real(c, sym);
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
    long long d = q1.QuadPart - q0.QuadPart, fr = nvn_front_frame_next();
    while (InterlockedExchange(&lock, 1)) YieldProcessor();
    if (fr != cur_frame) {
        if (tks * 500 > fq.QuadPart) printf("import_watch: frame %lld: %lld calls, %.2f ms; slowest %.3f ms (argument 1 = %llu, argument 2 as text \"%s\", called from %llx); %d%s different return addresses, the first: %llx %llx %llx %llx; %lld calls since the start\n", cur_frame, n, (double)tks * 1000.0 / (double)fq.QuadPart,
                                             (double)max_tk * 1000.0 / (double)fq.QuadPart, (unsigned long long)max_x1, max_txt, (unsigned long long)max_ra, nras, nras == 64 ? "+" : "", (unsigned long long)ras[0], (unsigned long long)ras[1], (unsigned long long)ras[2], (unsigned long long)ras[3], n_all);
        cur_frame = fr; n = tks = max_tk = 0; nras = 0; memset(ras, 0, sizeof ras);
    }
    n++; n_all++; tks += d;
    if (d > max_tk) { max_tk = d; max_ra = ra; max_x1 = x1; memcpy(max_txt, txt, sizeof max_txt); }
    { int k = 0; for (; k < nras; k++) if (ras[k] == ra) break; if (k == nras && nras < 64) ras[nras++] = ra; }
    InterlockedExchange(&lock, 0);
}
static void host_import_real(Ctx *c, int sym) {
    static volatile LONG order_count;
    if (g_guest_frozen) guest_freeze_wait();
    if (!g_import_count_all && g_import_calls[sym] >= 3 && g_handlers[sym]) {
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = dyn_syms[sym].name;
        c->import_from = c->x[30] - 4;
        g_handlers[sym](c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        if (g_guest_frozen) guest_freeze_wait();
        return;
    }
    unsigned long long n = (unsigned long long)InterlockedIncrement64((volatile LONG64 *)&g_import_calls[sym]);
    if (n == 1) { g_import_order[InterlockedIncrement(&order_count) - 1] = sym; g_import_order_count = order_count; }
    ImportFn fn = g_handlers[sym];
    InterlockedIncrement64((volatile LONG64 *)&g_trace_seq);
    if (n <= 3) {
        trace_printf("%8llu %s %s  x0=%llx x1=%llx x2=%llx x3=%llx  from=%llx\n", g_trace_seq,
                     fn ? "call " : "STUB ", dyn_syms[sym].name,
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[3], (unsigned long long)c->x[30] - 4);
    }
    const char *saved_name = c->in_import;
    uint64_t saved_from = c->import_from;
    c->in_import = dyn_syms[sym].name;
    c->import_from = c->x[30] - 4;
    if (fn) fn(c);
    else {
        /* No handler: the function was never implemented. The console would fault on the unresolved address; a silent 0
         * is a fake answer. */
        if (n == 1) {
            printf("import \"%s\" was called without a handler (caller 0x%llx)\n", dyn_syms[sym].name, (unsigned long long)c->x[30] - 4);
            trace_printf("import \"%s\" called without a handler\n", dyn_syms[sym].name);
        }
        if (audit_strict_on()) guest_trap("call to an import that has no handler (SMO_STRICT)", c->x[30] - 4);
        c->x[0] = 0;
    }
    c->in_import = saved_name;
    c->import_from = saved_from;
    if (g_guest_frozen) guest_freeze_wait();
}

int import_counts(int *stubs) {                       /* smo_nerve.c (SMO_STAGE_LIST): imports reached so far, and how many of them are stubs */
    int n = g_import_order_count, s = 0;
    for (int i = 0; i < n; i++) s += !g_handlers[g_import_order[i]];
    *stubs = s;
    return n;
}

void print_import_summary(FILE *out) {
    int stubs = 0, impl = 0;
    for (int i = 0; i < g_import_order_count; i++) { if (g_handlers[g_import_order[i]]) impl++; else stubs++; }
    fprintf(out, "\nimports reached: %d  (implemented: %d, still stubs: %d), %llu calls in total\n",
            g_import_order_count, impl, stubs, g_trace_seq);
    if (!g_import_count_all) fprintf(out, "(call counts are off: each import is counted up to its third call only; SMO_IMPORT_COUNTS=1 counts every call)\n");
    fprintf(out, "most called imports:\n");
    for (int k = 0; k < 14; k++) {                       /* small selection sort over the reached imports */
        int best = -1;
        for (int i = k; i < g_import_order_count; i++)
            if (best < 0 || g_import_calls[g_import_order[i]] > g_import_calls[g_import_order[best]]) best = i;
        if (best < 0) break;
        int t = g_import_order[k]; g_import_order[k] = g_import_order[best]; g_import_order[best] = t;
        fprintf(out, "  %10llu  %s%s\n", g_import_calls[g_import_order[k]], dyn_syms[g_import_order[k]].name,
                g_handlers[g_import_order[k]] ? "" : "   (stub)");
    }
    if (getenv("SMO_IMPORT_COUNTS") && atoi(getenv("SMO_IMPORT_COUNTS")) >= 2) {      /* D2 debug aid: every reached import with its count */
        fprintf(out, "all import counts:\n");
        for (int i = 0; i < g_import_order_count; i++) fprintf(out, "  count %10llu  %s\n", g_import_calls[g_import_order[i]], dyn_syms[g_import_order[i]].name);
    }
    fprintf(out, "unimplemented imports reached:\n");
    for (int i = 0; i < g_import_order_count; i++) {
        int s = g_import_order[i];
        if (!g_handlers[s]) fprintf(out, "  %8llu  %s\n", g_import_calls[s], dyn_syms[s].name);
    }
}

/* ---------------------------------------------------------------- traps */
void print_devinfo_summary(FILE *out);       /* nvn_stub.c */
void display_report(FILE *out);              /* display.c */

void hid_report(FILE *out);                  /* nnhid.c: synthetic XInput pad (SMO_XINPUT_SYNTH) */
void os_thread_report(FILE *out);            /* os_threads.c: every thread the game created, with its nn::os arguments */


void report_and_exit(int code) {
    audit_exit_counts();
    print_import_summary(stdout);
    print_hostfn_summary(stdout);
    print_devinfo_summary(stdout);
    { void nvn_virtual_pool_report(FILE *out); nvn_virtual_pool_report(stdout); }   /* nvn_stub.c */
    display_report(stdout);
    hid_report(stdout);
    os_thread_report(stdout);
    { void smo_heap_report(FILE *out); smo_heap_report(stdout); }   /* smo_nn.c */
    { void smo_audio_report(FILE *out); smo_audio_report(stdout); }   /* smo_audio.c */
    { void smo_fast_report(FILE *out); smo_fast_report(stdout); }     /* smo_fast.c */
    { void nerve_report(FILE *out); nerve_report(stdout); }   /* smo_nerve.c */
    { void fontll_report(FILE *out); fontll_report(stdout); }   /* nnfontll.c */
    if (g_trace) { print_import_summary(g_trace); print_hostfn_summary(g_trace); fclose(g_trace); }
    fflush(stdout);
    ExitProcess((UINT)code);
}

void guest_trap(const char *what, uint64_t addr) {
    const char *guest_thread_name_of(DWORD tid);      /* os_threads.c */
    const char *tn = guest_thread_name_of(GetCurrentThreadId());
    printf("\nGUEST TRAP: %s (0x%llx), on thread %s\n", what, (unsigned long long)addr, tn ? tn : "main (or a host thread)");
    trace_printf("GUEST TRAP: %s (0x%llx)\n", what, (unsigned long long)addr);
#ifdef _WIN32
    RaiseException(0xE0000001, 0, 0, NULL);
#else
    __builtin_debugtrap();
#endif
    report_and_exit(2);
}
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) {
    printf("\nUNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
    trace_printf("UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
#ifdef _WIN32
    RaiseException(0xE0000002, 0, 0, NULL);
#else
    __builtin_debugtrap();
#endif
    report_and_exit(3);
}
void guest_svc(Ctx *c, uint32_t number) { (void)c; guest_trap("svc (direct kernel call)", number); }

/* debug aid: what the guest registers point to (SMO_CRASH_DUMP=<bytes>, 1 = 0x60) */
static void crash_dump_guest_memory(void) {
    if (getenv("SMO_CRASH_DUMP") && t_cur_ctx) {
        int dlen = (int)strtol(getenv("SMO_CRASH_DUMP"), NULL, 0);
        if (dlen < 0x60) dlen = 0x60;
        if (dlen > 0x1000) dlen = 0x1000;
        for (int r = 0; r < 31; r++) {
            uint64_t v = t_cur_ctx->x[r];
            MEMORY_BASIC_INFORMATION mi;
            if (v < 0x10000 || !VirtualQuery((void *)(uintptr_t)v, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                || (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize < v + (uint64_t)dlen) continue;
            printf("  [x%d = %llx]", r, (unsigned long long)v);
            for (int k = 0; k < dlen; k += 8) printf("%s%016llx", k % 0x20 ? " " : "\n     ", (unsigned long long)rd64(v + (uint64_t)k));
            uint64_t s = rd64(v + 0x20);
            if (s > 0x10000 && VirtualQuery((void *)(uintptr_t)s, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const unsigned char *p = (const unsigned char *)(uintptr_t)s; int n = 0;
                while (n < 60 && p[n] >= 32 && p[n] < 127) n++;
                if (n >= 3 && p[n] == 0) printf("\n     [+0x20] -> \"%.*s\"", n, (const char *)p);
            }
            printf("\n");
        }
        {   /* debug aid: registers that point at a C string, and the guest frame-pointer chain (x29 -> {fp, lr}):
             * the game is built with frame pointers, so this is the guest call stack even when the host stack has no unwind info */
            MEMORY_BASIC_INFORMATION mi;
#define READABLE(a, n) ((a) > 0x10000 && VirtualQuery((void *)(uintptr_t)(a), &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize >= (a) + (n))
            for (int r = 0; r < 31; r++) {
                uint64_t v = t_cur_ctx->x[r];
                if (!READABLE(v, 64)) continue;
                const unsigned char *p = (const unsigned char *)(uintptr_t)v; int n = 0;
                while (n < 63 && p[n] >= 32 && p[n] < 127) n++;
                if (n >= 3 && p[n] == 0) printf("  x%d -> \"%.*s\"\n", r, n, (const char *)p);
            }
            uint64_t fp = t_cur_ctx->x[29];
            printf("  guest frame chain (x29): lr=%llx", (unsigned long long)t_cur_ctx->x[30]);
            for (int d = 0; d < 24 && READABLE(fp, 16); d++) {
                uint64_t lr = rd64(fp + 8), nfp = rd64(fp);
                if (!lr) break;
                printf("%s%llx", d % 8 == 7 ? "\n    " : " <- ", (unsigned long long)lr);
                if (nfp <= fp) break;
                fp = nfp;
            }
            printf("\n");
#undef READABLE
        }
    }
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    printf("\nHOST EXCEPTION 0x%08lx at host address %p\n", er->ExceptionCode, er->ExceptionAddress);
    {   /* name the module and its offset, so the address can be looked up in the linker map (build_exe.py writes
         * <exe>.map next to the exe) even with address-space randomisation */
        HMODULE m = NULL;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)er->ExceptionAddress, &m) && m) GetModuleFileNameA(m, mod, sizeof mod);
        printf("  in %s at offset 0x%llx\n", mod, (unsigned long long)((uintptr_t)er->ExceptionAddress - (uintptr_t)m));
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        printf("  %s address 0x%llx\n", er->ExceptionInformation[0] == 1 ? "writing" : "reading",
               (unsigned long long)er->ExceptionInformation[1]);
    dump_guest_registers();
    crash_dump_guest_memory();
    {   /* the host frames too (module + offset; tools/lifter/mapaddr.py names them from the exe's map) */
        CONTEXT hc = *ep->ContextRecord;
        printf("host call stack (innermost first):\n");
        for (int depth = 0; depth < 16 && hc.Rip; depth++) {
            HMODULE m = NULL;
            char mod[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)hc.Rip, &m) && m) GetModuleFileNameA(m, mod, sizeof mod);
            const char *bn = strrchr(mod, '\\');
            printf("  %s+0x%llx\n", bn ? bn + 1 : mod, (unsigned long long)(hc.Rip - (uintptr_t)m));
            DWORD64 image_base;
            RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(hc.Rip, &image_base, NULL);
            if (!rf) break;
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, hc.Rip, rf, &hc, &hd, &est, NULL);
        }
    }
    printf("guest call stack (innermost first):\n");
    CONTEXT ctx = *ep->ContextRecord;
    uint64_t last = 0;
    for (int depth = 0, shown = 0; depth < 400 && shown < 40; depth++) {
        uint64_t g = guest_function_of(ctx.Rip);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
        DWORD64 image_base;
        RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
        if (!rf) break;
        void *handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, rf, &ctx, &handler_data, &establisher, NULL);
        if (!ctx.Rip) break;
    }
    report_and_exit(4);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
/* macOS and Linux: the same report from a signal handler. Host frames come from the frame-pointer chain (every arm64
 * macOS binary keeps one; the Linux build keeps them with -fno-omit-frame-pointer); a guest trap arrives here as SIGTRAP
 * from __builtin_debugtrap(), as RaiseException does on Windows. */
static void host_module_line(const char *indent, uint64_t pc) {
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(uintptr_t)pc, &m) && m) GetModuleFileNameA(m, mod, sizeof mod);
    const char *bn = strrchr(mod, '\\');
    printf("%s%s+0x%llx\n", indent, bn ? bn + 1 : mod, (unsigned long long)(pc - (uintptr_t)m));
}
static void crash_signal(int sig, siginfo_t *si, void *ucv) {
    static volatile int in_report;
    if (sig == SIGSEGV || sig == SIGBUS) recomp_try_catch();        /* a fault inside a __try probe goes to its __except */
    if (__atomic_exchange_n(&in_report, 1, __ATOMIC_SEQ_CST)) _exit(4);     /* a fault inside the report */
    ucontext_t *uc = ucv;
#ifdef __APPLE__
    uint64_t pc = arm_thread_state64_get_pc(uc->uc_mcontext->__ss), lr = arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
    uint64_t fp = arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
    uint32_t esr = uc->uc_mcontext->__es.__esr, ec = esr >> 26;
    int writing = (ec == 0x24 || ec == 0x25) && (esr & 0x40);
#else                                                /* x86-64: no link register; bit 1 of the page-fault error code is "write" */
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], lr = 0, fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    int writing = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#endif
    printf("\nHOST EXCEPTION signal %d (%s) at host address %p\n", sig, strsignal(sig), (void *)(uintptr_t)pc);
    host_module_line("  in ", pc);
    if (sig == SIGSEGV || sig == SIGBUS)
        printf("  %s address 0x%llx\n", writing ? "writing" : "reading", (unsigned long long)(uintptr_t)si->si_addr);
    dump_guest_registers();
    crash_dump_guest_memory();
    uint64_t pcs[400];
    int n = host_frames_from(pc, lr, fp, pcs, 400);
    printf("host call stack (innermost first):\n");
    for (int i = 0; i < n && i < 16; i++) host_module_line("  ", pcs[i]);
    printf("guest call stack (innermost first):\n");
    uint64_t last = 0;
    for (int i = 0, shown = 0; i < n && shown < 40; i++) {
        uint64_t g = guest_function_of(pcs[i]);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
    }
    report_and_exit(4);
}
static void crash_handlers_install(void) {
    static uint8_t alt[1 << 16];                  /* the thread that boots the runtime reports a host stack overflow too */
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (int i = 0; i < (int)(sizeof sigs / sizeof sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}
/* The guest window, from the game image to the end of the SDK module's 4 GB (20 GB of address space, no memory), is
 * taken when the program is loaded, before anything else of it allocates. VirtualAlloc maps the image, the import
 * placeholders and the SDK module into it later. */
__attribute__((constructor)) static void guest_window_reserve(void) {
    if (!recomp_reserve_window(GUEST_BASE, SDK_BASE + 0x100000000ULL - GUEST_BASE)) {
        fprintf(stderr, "cannot reserve the guest address range 0x%llx..0x%llx: something else of the process is there\n",
                GUEST_BASE, SDK_BASE + 0x100000000ULL);
        exit(1);
    }
}
#endif

/* ---------------------------------------------------------------- loader */
void runtime_init(const char *elf, const char *trace_path) {
    g_handlers = calloc((size_t)dyn_sym_count, sizeof *g_handlers);
    g_import_calls = calloc((size_t)dyn_sym_count, sizeof *g_import_calls);
    g_import_order = calloc((size_t)dyn_sym_count, sizeof *g_import_order);
    g_by_host = malloc((size_t)fn_table_count * sizeof *g_by_host);
    memcpy(g_by_host, fn_table, (size_t)fn_table_count * sizeof *g_by_host);
    qsort(g_by_host, (size_t)fn_table_count, sizeof *g_by_host, cmp_host);
    fn_hash_build();
    {   const char *e = getenv("SMO_IMPORT_COUNTS"), *a = getenv("SMO_AUDIT_COUNTS");
        g_import_count_all = (e && e[0] && e[0] != '0') || (a && a[0]); }
    {   /* sanity: host code addresses must be strictly increasing after the sort */
        int bad = 0;
        for (int i = 1; i < fn_table_count; i++) if ((uintptr_t)g_by_host[i].fn <= (uintptr_t)g_by_host[i - 1].fn) bad++;
        printf("host address table: %d entries, %d out of order, span %p..%p\n", fn_table_count, bad,
               (void *)g_by_host[0].fn, (void *)g_by_host[fn_table_count - 1].fn);
    }
    g_trace = fopen(trace_path, "w");
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    crash_handlers_install();
#endif

    void *mem = VirtualAlloc((void *)GUEST_BASE, IMAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void *)GUEST_BASE) { fprintf(stderr, "cannot map guest image at 0x%llx\n", GUEST_BASE); exit(1); }
    /* data symbols provided by other modules: zero-filled placeholders, IMPORT_SLOT bytes each */
    void *imp = VirtualAlloc((void *)IMPORT_BASE, (SIZE_T)dyn_sym_count * IMPORT_SLOT, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (imp != (void *)IMPORT_BASE) { fprintf(stderr, "cannot map import area\n"); exit(1); }
    FILE *f = fopen(elf, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", elf); exit(1); }
    fseek(f, (long)FILE_OFF, SEEK_SET);
    if (fread(mem, 1, FILE_IMAGE, f) != FILE_IMAGE) { fprintf(stderr, "short read on image\n"); exit(1); }
    fclose(f);

    unsigned long long rel = 0, sym_int = 0, sym_ext = 0, sym_weak = 0, other = 0;
    const uint64_t tables[2][2] = {{RELA, RELASZ}, {JMPREL, PLTRELSZ}};
    for (int t = 0; t < 2; t++) {
        for (uint64_t i = 0; i < tables[t][1] / 24; i++) {
            uint64_t e = GUEST_BASE + tables[t][0] + i * 24;
            uint64_t off = rd64(e), info = rd64(e + 8);
            int64_t addend = (int64_t)rd64(e + 16);
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (type == 0x403) { wr64(GUEST_BASE + off, GUEST_BASE + (uint64_t)addend); rel++; }
            else if (type == 0x101 || type == 0x401 || type == 0x402) {
                uint64_t s;
                if (dyn_syms[sym].defined) { s = GUEST_BASE + dyn_syms[sym].value; sym_int++; }
                /* An undefined WEAK symbol is null unless a module defines it, so the game's `if (hook)` tests are false, as on
                 * the console (the line below counts the relocations that point at such symbols). */
                else if ((rd8(GUEST_BASE + GUEST_DYNSYM + (uint64_t)sym * 24 + 4) >> 4) == 2 && !(g_import_addr && g_import_addr[sym])) { s = 0; sym_weak++; }
                else { s = g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; sym_ext++; }
                wr64(GUEST_BASE + off, s + (type == 0x101 ? (uint64_t)addend : 0));
            } else other++;
        }
    }
    printf("image mapped at 0x%llx: %llu relative relocations, %llu to own symbols, %llu to imported symbols, %llu to undefined weak symbols (null), %llu unhandled\n",
           GUEST_BASE, rel, sym_int, sym_ext, sym_weak, other);
    printf("lifted functions: %d   dynamic symbols: %d\n", fn_table_count, dyn_sym_count);
    /* page protection as the loader sets it on the console: the text and read-only data segments (program headers of
     * main.elf: R-X 0x0..0x39c7e90, R-- 0x39c8000..0x4f15f60; the data segment starts at 0x4f16000) become read-only once the
     * relocations are applied (all of them land in the data segment: the image would not load on the console otherwise), so a
     * wild write into code or constants faults instead of corrupting silently. SMO_NO_PROTECT=1 leaves the old mapping. */
    if (!getenv("SMO_NO_PROTECT")) {
        DWORD oldp;
#ifdef _WIN32
        const uint64_t ro_end = GUEST_RO_END;
#else
        const uint64_t ro_end = GUEST_RO_END & ~(uint64_t)(recomp_page_size() - 1);   /* 16 KB pages on macOS: the page holding the start of .data stays writable */
#endif
        if (!VirtualProtect((void *)GUEST_BASE, ro_end, PAGE_READONLY, &oldp)) { fprintf(stderr, "cannot protect the image's text and read-only data\n"); exit(1); }
        printf("image protection: 0x%llx..0x%llx read-only (text, read-only data), data and bss read-write\n", GUEST_BASE, GUEST_BASE + ro_end);
    }
}

/* ---------------------------------------------------------------- do-nothing handlers (SMO_STRICT)
 * A handler is "trivial" when its machine code does nothing but store constants into the guest context and
 * return: the generic ret0 / nothing / ret1 handlers, and every handler with a name of its own whose body is
 * the same thing. The test looks at the compiled code, so it does not depend on what a handler is called or
 * on which source file it lives in.
 *   SMO_STRICT=1        any import or run-time host function (vtable slot, NVN entry) bound to a trivial handler, and
 *                        any function import without a handler, stops the program with a list (exit code 5).
 *   SMO_STRICT=report   same list, without stopping.
 *   SMO_AUDIT_DUMP=<f>  write every import and its binding (handler RVA, trivial kind) to <f> after the
 *                        registrations and exit; "unimported" rows: handlers registered under names the game
 *                        does not import (reachable only through handler_by_name).
 *   SMO_AUDIT_COUNTS=<f> at exit, write the call count of every import and run-time host function. */

/* 0 = real code; 1 = empty body; 2 = only stores constants (x0 := *x0 when *sets_x0) */
#if defined(__aarch64__) || defined(_M_ARM64)
int audit_trivial(ImportFn fn, long long *x0, int *sets_x0) { return audit_trivial_a64((const void *)fn, x0, sets_x0); }
#else
/* The Ctx * argument is in rcx on Windows (register 1 in ModRM) and in rdi in the System V ABI of Linux (register 7). */
#ifdef _WIN32
#define AUDIT_ARG 1
#else
#define AUDIT_ARG 7
#endif
int audit_trivial(ImportFn fn, long long *x0, int *sets_x0) {
    const unsigned char *p = (const unsigned char *)fn;
    int stores = 0;
    long long rax = 0;
    *x0 = 0; *sets_x0 = 0;
    if (!p) return 0;
    for (int hop = 0; hop < 4 && p[0] == 0xE9; hop++) p = p + 5 + *(const int *)(p + 1);      /* linker thunk / tail jump */
    for (int step = 0; step < 12; step++) {
        const unsigned char *q = p;
        int w = 0;
        if (q[0] == 0xC3) return stores ? 2 : 1;
        if (q[0] == 0x48) { w = 1; q++; }
        if (q[0] == 0xC7 && (q[1] == AUDIT_ARG || q[1] == (0x40 | AUDIT_ARG) || q[1] == (0x80 | AUDIT_ARG))) {   /* mov [rcx+d], imm32 */
            long long disp = 0;
            int modrm = q[1];
            q += 2;
            if (modrm == (0x40 | AUDIT_ARG)) { disp = (signed char)q[0]; q += 1; } else if (modrm == (0x80 | AUDIT_ARG)) { disp = *(const int *)q; q += 4; }
            if (disp == 0) { *x0 = w ? (long long)*(const int *)q : (long long)*(const unsigned *)q; *sets_x0 = 1; }
            q += 4; stores++; p = q; continue;
        }
        if (w && q[0] == 0x83 && q[1] == (0x20 | AUDIT_ARG) && q[2] == 0x00) { *x0 = 0; *sets_x0 = 1; stores++; p = q + 3; continue; }   /* and qword [rcx], 0 */
        if (w && q[0] == 0xB8) { rax = *(const long long *)(q + 1); p = q + 9; continue; }      /* movabs rax, imm64 */
        if (!w && q[0] == 0xB8) { rax = *(const unsigned *)(q + 1); p = q + 5; continue; }      /* mov eax, imm32 */
        if (!w && q[0] == 0x31 && q[1] == 0xC0) { rax = 0; p = q + 2; continue; }               /* xor eax, eax */
        if (w && q[0] == 0x89 && q[1] == AUDIT_ARG) { *x0 = rax; *sets_x0 = 1; stores++; p = q + 2; continue; }   /* mov [rcx], rax */
        if (!w && q[0] == 0x0F && q[1] == 0x57 && q[2] == 0xC0) { p = q + 3; continue; }        /* xorps xmm0, xmm0 */
        if (!w && q[0] == 0x0F && (q[1] == 0x11 || q[1] == 0x29) && (q[2] == (0x40 | AUDIT_ARG) || q[2] == (0x80 | AUDIT_ARG))) {   /* movups/movaps [rcx+d], xmm0 */
            p = q + (q[2] == (0x40 | AUDIT_ARG) ? 4 : 7); stores++; continue;
        }
        return 0;
    }
    return 0;
}
#endif

static int g_strict = -1;                  /* 0 off, 1 stop, 2 report */
static int g_audit_started, g_audit_hostfn_bad;   /* before audit_startup, host-function findings are only counted, so the whole list is printed */
static int audit_mode(void) {
    if (g_strict < 0) {
        const char *e = getenv("SMO_STRICT");
        g_strict = !e || !e[0] || !strcmp(e, "0") ? 0 : !strcmp(e, "report") ? 2 : 1;
    }
    return g_strict;
}

static int audit_strict_on(void) { return audit_mode() == 1; }
static void audit_describe(char *buf, size_t size, int kind, long long x0, int sets) {
    if (kind == 1) snprintf(buf, size, "empty body");
    else if (sets) snprintf(buf, size, "only returns the constant 0x%llx", (unsigned long long)x0);
    else snprintf(buf, size, "only stores constants");
}

/* A run-time host function (vtable slot, NVN entry point) was handed out: check it. */
static void audit_hostfn(const char *name, ImportFn fn) {
    long long x0; int sets, kind;
    char what[96];
    if (!audit_mode() || !fn) return;
    kind = audit_trivial(fn, &x0, &sets);
    if (!kind) return;
    audit_describe(what, sizeof what, kind, x0, sets);
    printf("STRICT: run-time host function \"%s\" is bound to a do-nothing handler (%s)\n", name, what);
    trace_printf("STRICT: run-time host function \"%s\": %s\n", name, what);
    g_audit_hostfn_bad++;
    if (g_strict == 1 && g_audit_started) { printf("STRICT: stopping (SMO_STRICT=report lists everything without stopping)\n"); fflush(stdout); ExitProcess(5); }
}

static void audit_write(const char *path, int with_counts) {
    FILE *f = fopen(path, "w");
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!f) { printf("audit: cannot write %s\n", path); return; }
    fprintf(f, "# list\tname\tcalls\thandler_rva\tkind\tx0\tdata_redirect\n");
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0 = 0; int sets = 0, kind;
        if (dyn_syms[i].defined || !dyn_syms[i].name[0]) continue;
        kind = g_handlers[i] ? audit_trivial(g_handlers[i], &x0, &sets) : -1;
        fprintf(f, "import\t%s\t%llu\t%llx\t%d\t%s%llx\t%d\n", dyn_syms[i].name, with_counts ? g_import_calls[i] : 0ull,
                g_handlers[i] ? (unsigned long long)((uintptr_t)g_handlers[i] - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0,
                (int)(g_import_addr && g_import_addr[i]));
    }
    for (int i = 0; i < g_hostfn_count; i++) {
        long long x0 = 0; int sets = 0;
        int kind = g_hostfn[i].fn ? audit_trivial(g_hostfn[i].fn, &x0, &sets) : -1;
        fprintf(f, "hostfn\t%s\t%lld\t%llx\t%d\t%s%llx\t0\n", g_hostfn[i].name, with_counts ? (long long)g_hostfn[i].calls : 0ll,
                g_hostfn[i].fn ? (unsigned long long)((uintptr_t)g_hostfn[i].fn - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0);
    }
    for (int k = 0; k < g_known_count; k++) {      /* a handler registered under a name the game does not import */
        int imported = 0;
        for (int i = 0; i < dyn_sym_count && !imported; i++) imported = !dyn_syms[i].defined && !strcmp(dyn_syms[i].name, g_known[k].name);
        if (!imported) fprintf(f, "unimported\t%s\t0\t%llx\t-1\t-\t0\n", g_known[k].name, (unsigned long long)((uintptr_t)g_known[k].fn - base));
    }
    fclose(f);
}

static void audit_exit_counts(void) {
    const char *p = getenv("SMO_AUDIT_COUNTS");
    if (p && p[0]) audit_write(p, 1);
}

/* Called by boot.c after the last registration. */
void audit_startup(void) {
    const char *dump = getenv("SMO_AUDIT_DUMP");
    int bad = 0, trivial = 0, unbound = 0;
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0; int sets, kind;
        char what[96];
        const char *name = dyn_syms[i].name;
        if (dyn_syms[i].defined || !name[0]) continue;
        if (!g_handlers[i]) {
            if (g_import_addr && g_import_addr[i]) continue;         /* data symbol with its real value (sdkdata.c, C++ runtime tables) */
            unbound++;
            bad++;
            if (audit_mode()) printf("STRICT: import \"%s\" has no handler and no data (a call would return 0)\n", name);
            continue;
        }
        kind = audit_trivial(g_handlers[i], &x0, &sets);
        if (!kind) continue;
        trivial++;
        bad++;
        if (audit_mode()) { audit_describe(what, sizeof what, kind, x0, sets); printf("STRICT: import \"%s\" is bound to a do-nothing handler (%s)\n", name, what); }
    }
    printf("strict check: %d imports bound to do-nothing handlers, %d without handler or data%s\n",
           trivial, unbound, audit_mode() ? "" : " (SMO_STRICT=1 lists them and stops)");
    if (dump && dump[0]) { audit_write(dump, 0); fflush(stdout); ExitProcess(0); }
    g_audit_started = 1;
    bad += g_audit_hostfn_bad;
    if (bad && audit_mode() == 1) {
        printf("STRICT: %d binding(s) do nothing or have no handler: stopping (SMO_STRICT=report lists them without stopping)\n", bad);
        fflush(stdout);
        ExitProcess(5);
    }
}
