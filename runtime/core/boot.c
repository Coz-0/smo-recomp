/* Boot driver: runs the game's own start-up code (nninitStartup, static
 * constructors, nnMain) on the lifted binary and records which functions of
 * other modules it asks for, in order. Unimplemented imports return 0. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "runtime.h"
#include "hostrt.h"

/* entry points and .init_array from build/gen/image.h */
#define NNINIT_STARTUP GAME_SYM_nninitStartup
#define NN_MAIN GAME_SYM_nnMain
#define INIT_ARRAY GUEST_INIT_ARRAY
#define INIT_ARRAY_SIZE GUEST_INIT_ARRAY_SIZE

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

/* ---- C strings and memory */
static void i_memcpy(Ctx *c) { memcpy(P(0), P(1), (size_t)A2); }
static void i_memmove(Ctx *c) { memmove(P(0), P(1), (size_t)A2); }
static void i_memset(Ctx *c) { memset(P(0), (int)A1, (size_t)A2); }
static void i_memcmp(Ctx *c) { RET((int64_t)memcmp(P(0), P(1), (size_t)A2)); }
static void i_memchr(Ctx *c) { RET(memchr(P(0), (int)A1, (size_t)A2)); }
static void i_strlen(Ctx *c) { RET(strlen(P(0))); }
static void i_strnlen(Ctx *c) { RET(strnlen(P(0), (size_t)A1)); }
static void i_strcmp(Ctx *c) { RET((int64_t)strcmp(P(0), P(1))); }
static void i_strncmp(Ctx *c) { RET((int64_t)strncmp(P(0), P(1), (size_t)A2)); }
static void i_strcpy(Ctx *c) { strcpy(P(0), P(1)); }
static void i_strncpy(Ctx *c) { strncpy(P(0), P(1), (size_t)A2); }
static void i_strcat(Ctx *c) { strcat(P(0), P(1)); }
static void i_strchr(Ctx *c) { RET(strchr(P(0), (int)A1)); }
static void i_strrchr(Ctx *c) { RET(strrchr(P(0), (int)A1)); }
static void i_strstr(Ctx *c) { RET(strstr(P(0), P(1))); }

/* ---- C++ runtime */
static void i_guard_acquire(Ctx *c) { RET(rd8(A0) == 0); }
static void i_guard_release(Ctx *c) { wr8(A0, 1); }
static void i_abort(Ctx *c) { guest_trap("guest called abort()", c->x[30] - 4); }

/* ---- math: arguments and result in v0/v1 */
#define M1F(n, fn) static void n(Ctx *c) { float r = fn(c->v[0].f[0]); vzero(&c->v[0]); c->v[0].f[0] = r; }
#define M2F(n, fn) static void n(Ctx *c) { float r = fn(c->v[0].f[0], c->v[1].f[0]); vzero(&c->v[0]); c->v[0].f[0] = r; }
#define M2D(n, fn) static void n(Ctx *c) { double r = fn(c->v[0].df[0], c->v[1].df[0]); vzero(&c->v[0]); c->v[0].df[0] = r; }
M1F(m_sinf, sinf) M1F(m_cosf, cosf) M1F(m_tanf, tanf) M1F(m_asinf, asinf) M1F(m_acosf, acosf) M1F(m_atanf, atanf)
M1F(m_expf, expf) M1F(m_exp2f, exp2f) M1F(m_logf, logf) M1F(m_log10f, log10f) M1F(m_sqrtf, sqrtf)
M2F(m_atan2f, atan2f) M2F(m_powf, powf) M2F(m_fmodf, fmodf)
M2D(m_pow, pow)

static const ImportDef base_imports[] = {
    {"memcpy", i_memcpy}, {"memmove", i_memmove}, {"memset", i_memset}, {"memcmp", i_memcmp}, {"memchr", i_memchr},
    {"strlen", i_strlen}, {"strnlen", i_strnlen}, {"strcmp", i_strcmp}, {"strncmp", i_strncmp}, {"strcpy", i_strcpy},
    {"strncpy", i_strncpy}, {"strcat", i_strcat}, {"strchr", i_strchr}, {"strrchr", i_strrchr}, {"strstr", i_strstr},
    {"__cxa_guard_acquire", i_guard_acquire}, {"__cxa_guard_release", i_guard_release},
    {"abort", i_abort},
    {"sinf", m_sinf}, {"cosf", m_cosf}, {"tanf", m_tanf}, {"asinf", m_asinf}, {"acosf", m_acosf}, {"atanf", m_atanf},
    {"expf", m_expf}, {"exp2f", m_exp2f}, {"logf", m_logf}, {"log10f", m_log10f}, {"sqrtf", m_sqrtf},
    {"atan2f", m_atan2f}, {"powf", m_powf}, {"fmodf", m_fmodf},
    {"pow", m_pow},
};

void register_sdk_imports(void);         /* nnsdk.c */
void register_cxx_imports(void);         /* libcxx.c */
void register_thread_imports(void);      /* os_threads.c */
void register_nvn_imports(void);         /* nvn_stub.c */
void register_fs_imports(void);          /* nnfs.c */
void register_clib_imports(void);        /* clib.c */
void register_time_imports(void);        /* nntime.c */
void register_hid_imports(void);         /* nnhid.c */
void register_iostream_imports(void);    /* nniostream.c */
void register_display_imports(void);     /* display.c */
void register_pl_imports(void);          /* nnpl.c */
void register_fontll_imports(void);      /* nnfontll.c */
void register_movie_imports(void);       /* nnmovie.c */
void register_misc_imports(void);        /* nnmisc.c */
void register_audio_imports(void);       /* nnaudio.c */
void register_service_imports(void);        /* nnservices.c: sdklift_*.c, nnapplet.c, nnnet.c, nnfsdir.c */
void register_movie_mf_imports(void);    /* nnmovie_mf.c */
void cxxrt_install_data(void);           /* nncxxrt.c: vtables / typeinfo objects of the C++ runtime (before runtime_init) */
void register_unwind_imports(void);      /* nnunwind.c: C++ exception unwinding through lifted module code */
void register_cxxrt_imports(void);       /* nncxxrt.c + nncxxlib.c + nnlibc2.c: C and C++ runtime, fills what is still missing */
void display_init(void);
int guest_thread_count(void);
extern uint64_t g_main_stack_base, g_main_stack_size;

void dump_guest_threads(FILE *out);      /* os_threads.c */
void icall_dump(FILE *out, uint64_t main_ctx);   /* hostrt.c, SMO_ICALLS */
uint64_t icall_first_x0(uint64_t target);
static Ctx *g_main_ctx;

static const char *g_elf;

/* Debug aid: SMO_PROF=<start s>,<duration s> samples the main guest thread's host stack every ~2 ms in that
 * window, unwinds it to guest functions and prints every (caller -> callee) pair seen with its sample count. */
#define PROF_MAX 8192
static struct { uint64_t callee, caller; unsigned n; } g_prof[PROF_MAX];
static int g_prof_used;
static HANDLE g_prof_target;
static DWORD WINAPI profiler(LPVOID arg) {
    int start = 0, dur = 0;
    (void)arg;
    char mode = 's';
    if (sscanf(getenv("SMO_PROF"), "%d,%d,%c", &start, &dur, &mode) < 2 || dur <= 0) return 0;
    Sleep((DWORD)start * 1000);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)dur * 1000;
    unsigned samples = 0;
    if (mode == 'f') {        /* SMO_PROF=<start>,<dur>,f : spin reading c->fn / x30: every function the main thread enters */
        while (GetTickCount64() < end) {
            for (int r = 0; r < 4096; r++) {
                uint64_t callee = g_main_ctx->fn, caller = g_main_ctx->x[30];
                unsigned h = (unsigned)((callee >> 2) * 2654435761u) % PROF_MAX;
                while (g_prof[h].callee && g_prof[h].callee != callee) h = (h + 1) % PROF_MAX;
                if (!g_prof[h].callee) { if (g_prof_used >= PROF_MAX - 16) continue; g_prof[h].callee = callee; g_prof[h].caller = caller; g_prof_used++; }
                g_prof[h].n++;
                samples++;
            }
        }
        printf("\n[prof] %u reads of the main thread's current function (function, first lr seen: reads)\n", samples);
        for (int j = 0; j < PROF_MAX; j++)
            if (g_prof[j].callee)
                printf("[prof] f_%llx lr %llx: %u\n", (unsigned long long)g_prof[j].callee, (unsigned long long)g_prof[j].caller, g_prof[j].n);
        return 0;
    }
    while (GetTickCount64() < end) {
        uint64_t chain[64];
        int k = 0;
#ifndef _WIN32
        uint64_t pcs[400];
        int npc = host_thread_backtrace(g_prof_target, pcs, 400);
        for (int depth = 0; depth < npc && k < 64; depth++) {
            uint64_t g = guest_function_of(pcs[depth]);
            if (g && (k == 0 || chain[k - 1] != g)) chain[k++] = g;
        }
#else
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        SuspendThread(g_prof_target);
        GetThreadContext(g_prof_target, &ctx);
        for (int depth = 0; depth < 400 && k < 64; depth++) {
            uint64_t g = guest_function_of(ctx.Rip);
            if (g && (k == 0 || chain[k - 1] != g)) chain[k++] = g;
            DWORD64 base;
            RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!rf) break;
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
            if (!ctx.Rip) break;
        }
        ResumeThread(g_prof_target);
#endif
        samples++;
        for (int i = 0; i < k; i++) {
            uint64_t callee = chain[i], caller = i + 1 < k ? chain[i + 1] : 0;
            int j;
            for (j = 0; j < g_prof_used; j++) if (g_prof[j].callee == callee && g_prof[j].caller == caller) break;
            if (j == g_prof_used) { if (g_prof_used == PROF_MAX) continue; g_prof[g_prof_used].callee = callee; g_prof[g_prof_used].caller = caller; g_prof_used++; }
            g_prof[j].n++;
        }
        for (ULONGLONG t0 = GetTickCount64(); GetTickCount64() - t0 < 1;) YieldProcessor();
    }
    printf("\n[prof] %u samples of the main thread (callee <- caller: samples)\n", samples);
    for (int j = 0; j < g_prof_used; j++)
        printf("[prof] f_%llx <- f_%llx: %u\n", (unsigned long long)g_prof[j].callee, (unsigned long long)g_prof[j].caller, g_prof[j].n);
    return 0;
}

static size_t main_stack_size_from_npdm(void) {
    const char *ov = getenv("SMO_MAIN_STACK");
    if (ov && strtoull(ov, NULL, 0) >= 0x10000) return (size_t)(strtoull(ov, NULL, 0) + 0xfff) & ~(size_t)0xfff;
    char path[1024];
    snprintf(path, sizeof path, "%s", g_elf);
    char *sl = strrchr(path, '/'), *bs = strrchr(path, '\\');
    char *cut = sl > bs ? sl : bs;
    snprintf(cut ? cut + 1 : path, sizeof path - (size_t)((cut ? cut + 1 : path) - path), "..\\exefs\\main.npdm");   /* build/dump/update/{elf,exefs} */
    FILE *f = fopen(path, "rb");
    unsigned char h[0x20];
    if (!f || fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "META", 4)) {
        fprintf(stderr, "missing input: %s (the program's meta data: main thread stack size)\n", path); exit(7);
    }
    fclose(f);
    uint32_t size; memcpy(&size, h + 0x1c, 4);
    printf("main.npdm: main thread stack 0x%x bytes, priority %u, default core %u\n", size, h[0xe], h[0xf]);
    return ((size_t)size + 0xfff) & ~(size_t)0xfff;
}

/* how much of the main guest stack was ever written (pages are zero until used) */
void main_stack_report(void) {
    extern uint64_t g_main_stack_base, g_main_stack_size;
    if (!g_main_stack_base) return;
    const uint64_t *p = (const uint64_t *)(uintptr_t)g_main_stack_base, *e = p + g_main_stack_size / 8;
    while (p < e && !*p) p++;
    MEMORY_BASIC_INFORMATION lo, hi;
    VirtualQuery((void *)(uintptr_t)(g_main_stack_base - 0x1000), &lo, sizeof lo);
    VirtualQuery((void *)(uintptr_t)(g_main_stack_base + g_main_stack_size), &hi, sizeof hi);
    printf("main guest stack: 0x%llx bytes at %llx, deepest use 0x%llx bytes (%.1f%%); page below: %s, page above: %s\n",
           (unsigned long long)g_main_stack_size, (unsigned long long)g_main_stack_base,
           (unsigned long long)((const uint8_t *)e - (const uint8_t *)p), 100.0 * (double)((const uint8_t *)e - (const uint8_t *)p) / (double)g_main_stack_size,
           lo.State == MEM_RESERVE ? "no access" : "ACCESSIBLE", hi.State == MEM_RESERVE ? "no access" : "ACCESSIBLE");
}

static DWORD WINAPI guest_main(LPVOID arg) {
    (void)arg;
    static Ctx c;
    /* the main thread's stack has the size the program's own meta data asks for (main.npdm, META header +0x1c:
     * 0x100000 for this game; read from the file next to the ELF), with no-access pages below and above it, so that an
     * overflow faults as it does on the console instead of running on. Guest frames are the original's (the lifted code
     * moves the guest SP exactly as the ARM code does); the host's own frames live on the host stack and are not
     * guest-visible. SMO_MAIN_STACK=<bytes> overrides (diagnosis only). */
    size_t stack_size = main_stack_size_from_npdm();
    uint8_t *stack_res = VirtualAlloc(NULL, stack_size + 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *stack = stack_res ? VirtualAlloc(stack_res + 0x10000, stack_size, MEM_COMMIT, PAGE_READWRITE) : NULL;
    if (!stack) { fprintf(stderr, "cannot allocate the main thread's stack (0x%llx bytes)\n", (unsigned long long)stack_size); exit(1); }
    uint8_t *tls = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c.x[31] = (uint64_t)(stack + stack_size);          /* the initial SP is the top of the stack (16-byte aligned: the size is whole pages) */
    c.tpidr = (uint64_t)tls;
    { uint64_t os_main_thread_type(void); memcpy(tls + 0x1f8, &(uint64_t){os_main_thread_type()}, 8); }   /* [sdk 0x3349dc..e0] nn::os::detail::ThreadManager(): the main thread's ThreadType pointer */
    g_main_ctx = &c;
    { void os_apply_main_sched(void); os_apply_main_sched(); }                  /* os_threads.c: SMO_OS_CONSOLE_CORES=1 */
    { void smo_fast_install(void); smo_fast_install(); }                        /* smo_fast.c: host versions of hot game code (before the game runs) */
    { void hitch_set_main(Ctx *c); hitch_set_main(&c); }      /* hitch.c: SMO_HITCH */
    g_main_stack_base = (uint64_t)stack;
    g_main_stack_size = stack_size;

    { void sdk_run_ctors(Ctx *c); sdk_run_ctors(&c); }       /* selected SDK static constructors (sdklift_rt.c) */
    printf("\n[phase 1] nninitStartup\n");
    trace_printf("=== phase 1: nninitStartup\n");
    call_guest(&c, NNINIT_STARTUP);

    int count = (int)(INIT_ARRAY_SIZE / 8);
    printf("[phase 2] %d static constructors\n", count);
    trace_printf("=== phase 2: static constructors\n");
    for (int i = 0; i < count; i++) {
        uint64_t fn = rd64(INIT_ARRAY + (uint64_t)i * 8);
        if (!fn) continue;
        trace_printf("--- constructor %d at %llx\n", i, (unsigned long long)fn);
        call_guest(&c, fn);
    }

    printf("[phase 3] nnMain\n");
    trace_printf("=== phase 3: nnMain\n");
    call_guest(&c, NN_MAIN);

    printf("\nnnMain returned.\n");
    print_import_summary(stdout);
    return 0;
}

/* The main thread waits for the guest's main thread. On macOS it pumps the window's events meanwhile: Cocoa windows belong
 * to the main thread (display.c). */
static DWORD wait_game(HANDLE t, DWORD ms) {
#ifdef _WIN32
    return WaitForSingleObject(t, ms);
#else
    void display_pump(void);
    ULONGLONG end = ms == INFINITE ? ~0ull : GetTickCount64() + ms;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD slice = now >= end ? 0 : end - now > 8 ? 8 : (DWORD)(end - now);
        DWORD r = WaitForSingleObject(t, slice);
        display_pump();
        if (r != WAIT_TIMEOUT || GetTickCount64() >= end) return r;
    }
#endif
}

int main(int argc, char **argv) {
    g_elf = argc > 1 ? argv[1] : GAME_ELF_DEFAULT;   /* the target's main.elf from the exe's folder (image.h) */
    setvbuf(stdout, NULL, _IONBF, 0);
    {   /* pad tools and their self-test (nnpad_sdl.c): no game is started */
        int nnhid_pad_selftest(void), nnpad_tool(int, char **);
        int tool;
        if (getenv("SMO_PAD_SELFTEST")) return nnhid_pad_selftest() != 0;
        if ((tool = nnpad_tool(argc, argv)) >= 0) return tool;
    }
    /* data symbols the game imports from the SDK module get their real values (build/gen/sdkdata.c, written by gen_tables.py) */
    for (int i = 0; i < sdk_data_count; i++) {
        void *copy = _aligned_malloc(sdk_data[i].size, 16);
        memcpy(copy, sdk_data[i].bytes, sdk_data[i].size);
        import_set_address(sdk_data[i].name, copy);
    }
    cxxrt_install_data();
    runtime_init(g_elf, getenv("SMO_TRACE") ? getenv("SMO_TRACE") : "boot_trace.txt");
    register_imports(base_imports, (int)(sizeof base_imports / sizeof base_imports[0]));
    register_sdk_imports();
    register_cxx_imports();
    register_thread_imports();               /* later registrations replace earlier ones */
    register_nvn_imports();
    register_fs_imports();
    register_clib_imports();                 /* full printf engine replaces the integer-only versions */
    register_time_imports();
    register_hid_imports();
    register_display_imports();
    register_pl_imports();
    register_fontll_imports();
    register_movie_imports();
    register_misc_imports();                 /* only fills names nobody else implements */
    register_audio_imports();
    register_movie_mf_imports();             /* replaces nnmovie.c's failing placeholder unless SMO_MOVIE=0 */
    register_iostream_imports();             /* libc++ stream base classes */
    register_cxxrt_imports();                /* C / C++ runtime: everything still without a handler (keep last) */
    register_unwind_imports();               /* installs the unwinder the C++ runtime calls on a throw */
    register_service_imports();                 /* SDK families (lifted sdk.elf code + offline service answers): fills only; after the C runtime because its self-test needs every handler */
    { void register_smo_imports(void); register_smo_imports(); }   /* smo_nn.c: SDK functions no other table handles */
    if (getenv("SMO_IMPORT_SELFTEST")) { int smo_import_selftest(void); return smo_import_selftest() != 0; }   /* smo_selftest.c */
    { void audit_startup(void); audit_startup(); }   /* hostrt.c: the check of do-nothing handlers (SMO_STRICT) */
    display_init();
    int seconds = argc > 2 ? atoi(argv[2]) : 0;      /* no number = no time limit (a player's launch); tests pass theirs */
    /* the lifted code uses the host stack for every guest call: give it plenty */
    HANDLE t = CreateThread(NULL, (SIZE_T)1 << 30, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (getenv("SMO_PROF")) { g_prof_target = t; CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL)); }
    DWORD wr = wait_game(t, seconds > 0 ? (DWORD)seconds * 1000 : INFINITE);
    {   unsigned long long stage_list_started(void);     /* smo_nerve.c: SMO_STAGE_LIST (TEST ONLY): the limit counts from the start of the stage being played */
        for (unsigned long long s; wr == WAIT_TIMEOUT && (s = stage_list_started()) && s + (unsigned long long)seconds * 1000 > GetTickCount64(); )
            wr = wait_game(t, (DWORD)(s + (unsigned long long)seconds * 1000 - GetTickCount64()));
    }
    {   unsigned nvn_gl_held_ms(void);                   /* nvn_gl.c: time the presents were held for shader preparation is not run time */
        for (unsigned done = 0, h; wr == WAIT_TIMEOUT && (h = nvn_gl_held_ms()) > done + 50; done = h) wr = wait_game(t, h - done);
    }
    if (wr == WAIT_TIMEOUT) {
        printf("\nstill running after %d s with %d guest thread(s) started: stopping here.\n", seconds, guest_thread_count());
        trace_printf("=== stopped by the time limit after %d s\n", seconds);
        main_stack_report();
        if (g_main_ctx) {
            printf("main thread: last function f_%llx  lr=%llx  %s%s (from %llx)\n", (unsigned long long)g_main_ctx->fn,
                   (unsigned long long)g_main_ctx->x[30], g_main_ctx->in_import ? "in " : "",
                   g_main_ctx->in_import ? g_main_ctx->in_import : "guest code", (unsigned long long)g_main_ctx->import_from);
            for (int i = 19; i < 29; i++) printf("  x%d %016llx%s", i, (unsigned long long)g_main_ctx->x[i], i % 3 == 0 ? "\n" : "");
            printf("\n");
            /* where is the main host thread really? sample its instruction pointer and unwind */
            for (int s = 0; s < 3; s++) {
                Sleep(50);
#ifndef _WIN32
                uint64_t pcs[200];
                int npc = host_thread_backtrace(t, pcs, 200);
                printf("  host pc sample %d: pc=%p -> f_%llx |", s, npc ? (void *)(uintptr_t)pcs[0] : NULL, (unsigned long long)(npc ? guest_function_of(pcs[0]) : 0));
                uint64_t last = 0;
                for (int depth = 0, shown = 0; depth < npc && shown < 12; depth++) {
                    uint64_t g = guest_function_of(pcs[depth]);
                    if (g && g != last) { printf(" f_%llx", (unsigned long long)g); shown++; last = g; }
                    else if (!g && depth == 0) printf(" [host %p]", (void *)(uintptr_t)pcs[0]);
                }
                printf("\n");
#else
                CONTEXT ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_FULL;
                SuspendThread(t);
                GetThreadContext(t, &ctx);
                printf("  host rip sample %d: rip=%p -> f_%llx |", s, (void *)ctx.Rip, (unsigned long long)guest_function_of(ctx.Rip));
                uint64_t last = 0;
                for (int depth = 0, shown = 0; depth < 200 && shown < 12; depth++) {
                    uint64_t g = guest_function_of(ctx.Rip);
                    if (g && g != last) { printf(" f_%llx", (unsigned long long)g); shown++; last = g; }
                    else if (!g && depth == 0) printf(" [host %p]", (void *)ctx.Rip);
                    DWORD64 base;
                    RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (!rf) break;
                    void *hd; DWORD64 est;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
                    if (!ctx.Rip) break;
                }
                printf("\n");
                ResumeThread(t);
#endif
            }
            for (int s = 0; s < 8; s++) {            /* is it moving? sample a few times */
                Sleep(37);
                printf("  sample %d: f_%llx lr=%llx x0=%llx x1=%llx x8=%llx x9=%llx x11=%llx x12=%llx\n", s,
                       (unsigned long long)g_main_ctx->fn, (unsigned long long)g_main_ctx->x[30], (unsigned long long)g_main_ctx->x[0],
                       (unsigned long long)g_main_ctx->x[1], (unsigned long long)g_main_ctx->x[8], (unsigned long long)g_main_ctx->x[9],
                       (unsigned long long)g_main_ctx->x[11], (unsigned long long)g_main_ctx->x[12]);
            }
        }
        dump_guest_threads(stdout);
        icall_dump(stdout, (uint64_t)(uintptr_t)g_main_ctx);
        if (getenv("SMO_CHAIN")) {
            /* SMO_CHAIN=<fn>:<off>,<off>,...[;<fn>:...] (hex): start from x0 of the first recorded indirect call to fn
             * (needs SMO_ICALLS), follow pointers at the given offsets, print 0x40 bytes at every step */
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", getenv("SMO_CHAIN"));
            for (char *spec = strtok(buf, ";"); spec; spec = strtok(NULL, ";")) {
                char *colon = strchr(spec, ':');
                uint64_t p = icall_first_x0(strtoull(spec, NULL, 16));
                printf("[chain] %s: x0=%llx\n", spec, (unsigned long long)p);
                for (char *o = colon ? colon + 1 : NULL; p; o = o ? strchr(o, ',') : NULL) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_COMMIT) { printf("[chain]   %llx not mapped\n", (unsigned long long)p); break; }
                    printf("[chain]   %llx:", (unsigned long long)p);
                    for (int w = 0; w < 0x130; w += 8) printf("%s%016llx", w % 0x40 ? " " : "\n[chain]     ", (unsigned long long)rd64(p + w));
                    printf("\n");
                    if (!o) break;
                    if (*o == ',') o++;
                    uint64_t off = strtoull(o, NULL, 16);
                    p = rd64(p + off);
                    printf("[chain]   [+%llx] = %llx\n", (unsigned long long)off, (unsigned long long)p);
                }
            }
        }
        report_and_exit(0);
    }
    return 0;
}
