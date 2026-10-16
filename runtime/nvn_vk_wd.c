/* nvn_vk_wd.c (session 12; included at the end of nvn_vk_draw.c: not a build unit of its own).
 *
 * THE FREEZE WATCHDOG. On by default in the Vulkan renderer (SMO_VK_WATCHDOG=0 off, =<seconds> another limit; default 5).
 * A thread looks twice a second at the time of the last frame that reached the window. When no frame was presented for the
 * limit while the process is alive, a report is printed and appended to <exe name>_freeze.txt next to the exe:
 *   - what the renderer was doing (frames, present attempts, failures, queues of the workers, the preparation's state);
 *   - our locks (g_sh_lock, g_gpl_lock): the thread that owns each;
 *   - every wait of ours that was entered through WD_SLEEP (function, line, how long);
 *   - EVERY thread of the process: its name (guest thread name / render / main), whether it used CPU in the last 300 ms,
 *     and its call stack: host functions by name when <exe name>.map is next to the exe (tests\s12_link.py writes it; without
 *     it: module + offset), functions of the lifted game as "guest f_<address>", system and driver DLLs by nearest export.
 * The report is repeated after 4x the time (5 s, 20 s, 80 s, ...), at most 6 times a run; a line says when frames come back.
 * It only reads: nothing is unblocked or killed. Threads are suspended one at a time for the few microseconds of the unwind;
 * nothing is printed or allocated while one is suspended. */
#include <tlhelp32.h>
const char *guest_thread_name_of(DWORD tid);       /* os_threads.c */
/* fn_table / fn_table_count (runtime.h; -Dfn_table=(*game_fn_table_p)): the lifted game's functions, by guest address */
static volatile LONG g_wd_quit;
static HANDLE g_wd_thread;
static FILE *g_wd_fp;
static DWORD g_wd_main_tid;
typedef struct { uint32_t rva; char name[68]; } WdSym;
static WdSym *g_wd_sym;
static int g_wd_nsym, g_wd_map_tried;
static void wdp(const char *fmt, ...) {
    char b[1400];
    va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stdout);
    if (g_wd_fp) fputs(b, g_wd_fp);
}
static int wd_sym_cmp(const void *a, const void *b) { const WdSym *x = a, *y = b; return x->rva < y->rva ? -1 : x->rva > y->rva; }
static void wd_map_load(void) {                    /* the linker's map of this exe, if it is there: "<exe without .exe>.map" */
    char path[MAX_PATH + 8], ln[1024], t1[64], t2[512], t3[64];
    unsigned long long base = 0x140000000ull;
    g_wd_map_tried = 1;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n < 5) return;
    strcpy(path + n - 4, ".map");
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    int cap = 0;
    while (fgets(ln, sizeof ln, fp)) {
        const char *p = strstr(ln, "Preferred load address is ");
        if (p) { base = strtoull(p + 26, NULL, 16); continue; }
        if (sscanf(ln, " %63s %511s %63s", t1, t2, t3) != 3 || strlen(t1) != 13 || t1[4] != ':' || strlen(t3) != 16) continue;
        unsigned long long va = strtoull(t3, NULL, 16);
        if (va <= base || va - base > 0x7fffffffull) continue;
        if (g_wd_nsym == cap) { cap = cap ? cap * 2 : 8192; g_wd_sym = realloc(g_wd_sym, (size_t)cap * sizeof *g_wd_sym); if (!g_wd_sym) { g_wd_nsym = 0; break; } }
        g_wd_sym[g_wd_nsym].rva = (uint32_t)(va - base);
        snprintf(g_wd_sym[g_wd_nsym].name, sizeof g_wd_sym[0].name, "%s", t2);
        g_wd_nsym++;
    }
    fclose(fp);
    if (g_wd_nsym) qsort(g_wd_sym, (size_t)g_wd_nsym, sizeof *g_wd_sym, wd_sym_cmp);
}
static void wd_export(HMODULE m, uint32_t rva, char *out, size_t n) {      /* the nearest exported name at or below rva */
    const uint8_t *b = (const uint8_t *)m;
    const IMAGE_DOS_HEADER *dh = (const IMAGE_DOS_HEADER *)b;
    out[0] = 0;
    if (dh->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + dh->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes < 1) return;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[0];
    if (!dd.VirtualAddress || !dd.Size) return;
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(b + dd.VirtualAddress);
    const uint32_t *fn = (const uint32_t *)(b + ed->AddressOfFunctions), *nm = (const uint32_t *)(b + ed->AddressOfNames);
    const uint16_t *od = (const uint16_t *)(b + ed->AddressOfNameOrdinals);
    uint32_t best = 0;
    const char *bn = NULL;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        uint32_t f = od[i] < ed->NumberOfFunctions ? fn[od[i]] : 0;
        if (!f || f > rva || f < best || (f >= dd.VirtualAddress && f < dd.VirtualAddress + dd.Size)) continue;
        best = f; bn = (const char *)(b + nm[i]);
    }
    if (bn) snprintf(out, n, "%.60s+0x%x", bn, (unsigned)(rva - best));
}
static void wd_addr(uint64_t pc, char *out, size_t n) {
    HMODULE m = NULL, exe = GetModuleHandleA(NULL), game = NULL;
    char mod[MAX_PATH] = "", sym[96] = "";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &m) || !m) { snprintf(out, n, "%llx (in no module)", (unsigned long long)pc); return; }
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)&fn_table[0], &game);
    GetModuleFileNameA(m, mod, sizeof mod);
    const char *bn = strrchr(mod, '\\');
    bn = bn ? bn + 1 : mod;
    uint32_t rva = (uint32_t)(pc - (uint64_t)(uintptr_t)m);
    if (m == exe) {
        if (!g_wd_map_tried) wd_map_load();
        int lo = 0, hi = g_wd_nsym - 1, at = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (g_wd_sym[mid].rva <= rva) { at = mid; lo = mid + 1; } else hi = mid - 1; }
        if (at >= 0) snprintf(sym, sizeof sym, "%s+0x%x", g_wd_sym[at].name, (unsigned)(rva - g_wd_sym[at].rva));
    } else if (m == game) {
        uint64_t best = 0, guest = 0;
        for (int i = 0; i < fn_table_count; i++) { uint64_t f = (uint64_t)(uintptr_t)fn_table[i].fn; if (f <= pc && f > best) { best = f; guest = fn_table[i].addr; } }
        if (best && pc - best < 0x400000) snprintf(sym, sizeof sym, "guest f_%llx (host +0x%x)", (unsigned long long)guest, (unsigned)(pc - best));
    } else wd_export(m, rva, sym, sizeof sym);
    snprintf(out, n, "%s+0x%x%s%s%s", bn, (unsigned)rva, sym[0] ? " [" : "", sym, sym[0] ? "]" : "");
}
/* The return addresses of another thread (suspended by the caller). Unwind data that cannot be read ends the walk. */
static int wd_walk(CONTEXT *ctx, uint64_t *pcs, int max) {
    int n = 0;
    __try {
        while (n < max && ctx->Rip) {
            DWORD64 base = 0, ef = 0;
            void *hd = NULL;
            pcs[n++] = ctx->Rip;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
            if (!rf) { ctx->Rip = *(const DWORD64 *)(uintptr_t)ctx->Rsp; ctx->Rsp += 8; }      /* a leaf function: the return address is at the stack pointer */
            else RtlVirtualUnwind(0 /* UNW_FLAG_NHANDLER */, base, ctx->Rip, rf, ctx, &hd, &ef, NULL);
        }
    } __except (1) { }
    return n;
}
static const char *wd_tname(DWORD tid, char *buf, size_t n) {
    const char *g = tid == g_render_tid ? "render (host)" : tid == g_wd_main_tid ? "the process's first thread (boot.c main: waits for the game)" : guest_thread_name_of(tid);
    if (g) return g;
    for (int i = 0; i < g_pw_nthread; i++) if (g_pw_thread[i] && GetThreadId(g_pw_thread[i]) == tid) { snprintf(buf, n, "shader worker %d (host)", i); return buf; }
    if (g_prep_thread && GetThreadId(g_prep_thread) == tid) return "preparation (host)";
    return "host thread";
}
static void wd_owner(const char *name, CRITICAL_SECTION *cs) {
    char tb[48];
    DWORD o = (DWORD)(uintptr_t)cs->OwningThread;
    if (o) wdp("  lock %s: OWNED by thread %lu (%s), recursion %ld, lock count %ld\n", name, (unsigned long)o, wd_tname(o, tb, sizeof tb), (long)cs->RecursionCount, (long)cs->LockCount);
    else wdp("  lock %s: free\n", name);
}
static void wd_dump(double stall_s, LONG64 tries_since) {
    static struct { DWORD tid; ULONGLONG cpu0, cpu1, born; int prio, nfr; uint64_t pc[28]; } th[400];
    char path[MAX_PATH + 16], tb[48], ab[400];
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId(), n = GetModuleFileNameA(NULL, path, MAX_PATH);
    int nt = 0;
    if (n > 4) { strcpy(path + n - 4, "_freeze.txt"); g_wd_fp = fopen(path, "ab"); }
    wdp("\nnvn_vk_watchdog: ======== NO FRAME PRESENTED FOR %.1f s ======== %04d-%02d-%02d %02d:%02d:%02d, process %lu, game frame %lld\n", stall_s, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long)pid, (long long)nvn_front_frame_next());
    wdp("  presents: %lld frames reached the window so far; present attempts since the last one that did: %lld (%s); frames not presented %lld, failed Vulkan calls %lld, swapchain made again %lld times; window %s\n",
        (long long)g_vk_frames, (long long)tries_since, tries_since ? "the game still presents but nothing reaches the window: see the failures" : "none: the game's present never came, or the render thread is held",
        (long long)g_vk_skipped, (long long)g_vk_errors, (long long)g_vk_recreated, IsIconic((HWND)nvn_front_hwnd()) ? "MINIMISED" : "not minimised");
    wdp("  render thread %lu, place in its present: %s; %s; fight near: %s\n", (unsigned long)g_render_tid, g_wd_where == 1 ? "the loading barrier (waits for the workers, at most 20 s)" : g_wd_where == 2 ? "vkAcquireNextImageKHR" : g_wd_where == 3 ? "blit and submit" : g_wd_where == 4 ? "vkQueuePresentKHR" : "not in a present",
        g_in_fight ? "the last frame was a FIGHT frame" : "the last frame was not a fight frame", fight_recent() ? "yes" : "no");
    wdp("  workers: %d threads; foreground jobs %lld queued / %lld done, background %lld / %lld, preparation %lld / %lld (%lld running); preparation state %d (0 not started, 1 running, 2 finished, 3 no containers, 5 store being built)%s, %d of %d containers read; loading-barrier holds %lld (time-outs %lld)\n",
        g_pw_nthread, (long long)g_jq_queued, (long long)g_jq_done, (long long)g_bq_queued, (long long)g_bq_done, (long long)g_pq_queued, (long long)g_pq_done, (long long)g_pq_running, g_prep_state, g_prep_marked ? ", marked start" : "", g_prep_read, g_prep_nfiles,
        (long long)g_bar_n, (long long)g_bar_timeout);
    wd_owner("g_sh_lock (stage, pipeline and job tables)", &g_sh_lock);
    wd_owner("g_gpl_lock (vertex-input / fragment-output libraries, universal layout)", &g_gpl_lock);
    {   int any = 0;
        for (int i = 0; i < 64; i++) {
            WdWait w = g_wd_wait[i];
            if (!w.tid) continue;
            wdp("  wait of ours: thread %lu (%s) in %s, line %d of nvn_vk_m4.c / nvn_vk_draw.c, for %.1f s\n", (unsigned long)w.tid, wd_tname((DWORD)w.tid, tb, sizeof tb), w.fn ? w.fn : "?", w.line, (double)(tk() - w.since) / (double)fq.QuadPart);
            any = 1;
        }
        if (!any) wdp("  waits of ours (stage / library / pipeline being made by another thread, queues): none entered\n");
    }
    for (int pass = 0; pass < 2; pass++) {         /* CPU time of every thread, twice, 300 ms apart: who is running and who is blocked */
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (snap == INVALID_HANDLE_VALUE) break;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            int i;
            for (i = 0; i < nt && th[i].tid != te.th32ThreadID; i++) {}
            if (i == nt) { if (pass || nt == 400) continue; th[nt].tid = te.th32ThreadID; th[nt].cpu0 = th[nt].cpu1 = 0; th[nt].nfr = 0; th[nt].prio = 0; th[nt].born = ~0ull; nt++; }
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            FILETIME c, e, k, u;
            if (!h) continue;
            if (GetThreadTimes(h, &c, &e, &k, &u)) { ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime); if (pass) th[i].cpu1 = cpu; else th[i].cpu0 = th[i].cpu1 = cpu; th[i].born = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime; }
            th[i].prio = GetThreadPriority(h);
            CloseHandle(h);
        }
        CloseHandle(snap);
        if (!pass) Sleep(300);
    }
    for (int i = 0; i < nt; i++) {                 /* the stacks: one thread suspended at a time, nothing else done meanwhile */
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, th[i].tid);
        static CONTEXT ctx;
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &ctx)) th[i].nfr = wd_walk(&ctx, th[i].pc, 28);
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    { ULONGLONG first = ~0ull; for (int i = 0; i < nt; i++) if (th[i].born < first) { first = th[i].born; g_wd_main_tid = th[i].tid; } }      /* the process's first thread runs the guest's main thread (boot.c) */
    wdp("  threads: %d (the watchdog's own not listed); \"cpu\" = CPU time used in 300 ms: 0.0 means blocked or idle\n", nt);
    for (int i = 0; i < nt; i++) {
        wdp("  thread %lu  %s  priority %d  cpu %.1f ms of 300\n", (unsigned long)th[i].tid, wd_tname(th[i].tid, tb, sizeof tb), th[i].prio, (double)(th[i].cpu1 - th[i].cpu0) / 10000.0);
        for (int f = 0; f < th[i].nfr; f++) { wd_addr(th[i].pc[f], ab, sizeof ab); wdp("      %2d  %s\n", f, ab); }
    }
    wdp("nvn_vk_watchdog: ======== end of the report%s%s ========\n\n", g_wd_fp ? "; also appended to " : "", g_wd_fp ? path : "");
    fflush(stdout);
    if (g_wd_fp) { fclose(g_wd_fp); g_wd_fp = NULL; }
}
static DWORD WINAPI wd_main(void *arg) {
    double limit = *(double *)arg, next = limit, reported = 0;
    LONG64 seen = 0, try0 = 0;
    int dumps = 0;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    while (!g_wd_quit) {
        Sleep(500);
        LONG64 b = g_wd_beat;
        if (!b || !V.alive || g_wd_quit) continue;  /* armed by the first frame that reaches the window */
        if (b != seen) {
            if (reported > 0) { printf("nvn_vk_watchdog: frames reach the window again (the stall lasted between %.1f and %.1f s)\n", reported, reported + (double)(b - seen) / (double)fq.QuadPart); fflush(stdout); }
            seen = b; try0 = g_wd_try; next = limit; reported = 0;
            continue;
        }
        double s = (double)(tk() - b) / (double)fq.QuadPart;
        if (s < next || dumps >= 6) continue;
        if (IsIconic((HWND)nvn_front_hwnd()) && g_wd_try != try0) { next = s + limit; continue; }      /* a minimised window: presents are skipped, the game runs */
        dumps++;
        wd_dump(s, g_wd_try - try0);
        reported = s; next = s * 4;
    }
    return 0;
}
static void wd_start(void) {
    static double limit = 5.0;
    const char *e = getenv("SMO_VK_WATCHDOG");
    if (e) limit = atof(e);
    if (limit <= 0 || g_offline) { printf("nvn_vk_watchdog: off (SMO_VK_WATCHDOG=0)\n"); return; }
    if (limit < 1) limit = 1;
    g_wd_thread = CreateThread(NULL, 0, wd_main, &limit, 0, NULL);
    if (g_wd_thread) SetThreadPriority(g_wd_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    printf("nvn_vk_watchdog: on: a report of every thread (locks, waits, call stacks) is written to the log and to <exe>_freeze.txt when no frame reaches the window for %.0f s (SMO_VK_WATCHDOG=<seconds>, 0 = off)\n", limit);
}
static void wd_stop(void) {
    g_wd_quit = 1;
    if (g_wd_thread) { WaitForSingleObject(g_wd_thread, 3000); CloseHandle(g_wd_thread); g_wd_thread = NULL; }
}
