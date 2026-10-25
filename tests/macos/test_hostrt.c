/* hostrt.c on macOS without a dump: maps a made-up image (tests/macos/stub/image.h), dispatches indirect calls to two
 * stand-in lifted functions, and crashes on purpose so check_core.sh can read the crash report.
 *   test_hostrt <image file> dispatch   indirect call through the hash table; exit 0
 *   test_hostrt <image file> write-ro   a write into the read-only text: the report names the fault and both functions
 *   test_hostrt <image file> trap       guest_trap from lifted code: GUEST TRAP, then the report
 *   test_hostrt <image file> probe      a __try probe of unreadable memory with hostrt.c's crash handler installed: caught */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"
#include "win32_posix.h"

/* ---- stand-ins for what the rest of the runtime and the generated tables provide */
static int g_mode;                       /* 0 dispatch, 1 write-ro, 2 trap */
static volatile uint64_t g_reached;
__attribute__((noinline)) static void f_inner(Ctx *c) {
    c->fn = GUEST_BASE + 0x200;
    g_reached = 1;
    if (g_mode == 1) wr32(GUEST_BASE + 0x1000, 0xdeadbeef);
    if (g_mode == 2) guest_trap("test trap", c->x[30]);
    c->x[0] = 42;
    __asm__ volatile("" ::: "memory");   /* keep this a frame of its own */
}
__attribute__((noinline)) static void f_outer(Ctx *c) {
    c->fn = GUEST_BASE + 0x100;
    c->x[30] = GUEST_BASE + 0x104;
    call_indirect(c, GUEST_BASE + 0x200);
    c->x[1] = c->x[0] + 1;
    __asm__ volatile("" ::: "memory");
}
const FnEntry fn_table[] = {{GUEST_BASE + 0x100, f_outer}, {GUEST_BASE + 0x200, f_inner}};
const int fn_table_count = 2;
const DynSym dyn_syms[] = {{"", 0, 0}};
const int dyn_sym_count = 1;
const SdkData sdk_data[] = {{"", NULL, 0}};
const int sdk_data_count = 0;

int g_nerve_mode;
volatile uint64_t g_nerve_watch[2], g_stage_init, g_stage_play;
void nerve_call(Ctx *c, uint64_t addr) { (void)c; (void)addr; }
void nerve_watch_hit(void) {}
void stage_init_hit(Ctx *c) { (void)c; }
void stage_play_hit(Ctx *c) { (void)c; }
void sdk_dispatch(Ctx *c, uint64_t addr) { (void)c; guest_trap("sdk_dispatch stand-in", addr); }
long long nvn_front_frame_next(void) { return 0; }
const char *guest_thread_name_of(DWORD tid) { (void)tid; return "test"; }
#define REPORT(n) void n(FILE *out) { (void)out; }
REPORT(display_report) REPORT(fontll_report) REPORT(hid_report) REPORT(nerve_report) REPORT(nvn_virtual_pool_report)
REPORT(os_thread_report) REPORT(print_devinfo_summary) REPORT(smo_audio_report) REPORT(smo_fast_report) REPORT(smo_heap_report)

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_hostrt <image file> dispatch|write-ro|trap\n"); return 2; }
    g_mode = !strcmp(argv[2], "write-ro") ? 1 : !strcmp(argv[2], "trap") ? 2 : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    runtime_init(argv[1], "/dev/null");
    if (!strcmp(argv[2], "probe")) {
        volatile int got = 0;
        __try { got = *(volatile int *)(uintptr_t)(GUEST_BASE + 0x40000000ull); } __except (1) { got = -1; }   /* reserved, not mapped */
        printf("probe: %s\n", got == -1 ? "caught" : "NOT caught");
        return got != -1;
    }
    static Ctx c;
    call_guest(&c, GUEST_BASE + 0x100);
    printf("dispatch: x0=%llu x1=%llu\n", (unsigned long long)c.x[0], (unsigned long long)c.x[1]);
    return c.x[0] == 42 && c.x[1] == 43 ? 0 : 1;
}
