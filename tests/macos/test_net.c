/* nnnet.c's sockets on macOS (BSD sockets through compat/winsock_posix.h), without the rest of the runtime: the socket
 * part of its self-test (SMO_SDK_SELFTEST's): nifm without a network, descriptors, Bind / GetSockName, Fcntl, UDP and TCP
 * on loopback, Poll, error numbers as the console's, GetAddrInfo. The lifted SDK functions it also tests (InetPton, ...)
 * need sdk.elf from a dump and are not run here. The file is included whole to reach its static test. */
#include <stdlib.h>
#include "nnnet.c"

static ImportDef g_reg[4096];
static int g_nreg;
void register_imports(const ImportDef *defs, int count) {
    for (int i = 0; i < count; i++) {
        int k = 0;
        while (k < g_nreg && strcmp(g_reg[k].name, defs[i].name)) k++;
        if (k == g_nreg && g_nreg < 4096) g_nreg++;
        g_reg[k] = defs[i];
    }
}
ImportFn import_lookup(const char *name) { for (int k = 0; k < g_nreg; k++) if (!strcmp(g_reg[k].name, name)) return g_reg[k].fn; return NULL; }
void *import_data(const char *name) { static uint8_t slot[256]; (void)name; return slot; }     /* every name counts as imported */
void guest_trap(const char *what, uint64_t addr) { printf("GUEST TRAP: %s (%llx)\n", what, (unsigned long long)addr); exit(2); }
void report_and_exit(int code) { exit(code); }
int sdk_image_ready(void) { return 0; }
int sdk_install(const char *name) { (void)name; return 0; }
uint64_t sdk_symbol(const char *name) { (void)name; return 0; }
volatile LONG g_bcat_mounted;
int bcat_dirname_valid(const char *name) { (void)name; return 1; }

/* nnsdk.c's __errno_location: the guest's errno, one per thread */
static void errno_location(Ctx *c) { static _Thread_local int e; c->x[0] = (uint64_t)(uintptr_t)&e; }

int main(void) {
    static Ctx c;
    setvbuf(stdout, NULL, _IONBF, 0);
    static const ImportDef errno_def = {"__errno_location", errno_location};
    register_imports(&errno_def, 1);
    register_nnnet_imports();
    nnnet_register_final();                           /* as nnservices.c does: the nifm and socket handlers */
    int bad = sock_selftest(&c);
    printf("socket self-test: %d failure(s)\n", bad);
    return bad != 0;
}
