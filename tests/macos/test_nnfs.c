/* nnfs.c / nnfsdir.c on macOS, without the rest of the runtime: their own self-tests (SMO_SDK_SELFTEST's) of the mount
 * table, the save data journal (commit, interrupted commits, other processes' leftovers) and directory listing, in
 * scratch folders under $TMPDIR. Exit code = failures. */
#include <stdio.h>
#include <stdarg.h>
#include "runtime.h"
#include "hostrt.h"

int g_hitch_ms;
void hitch_note(const char *s) { (void)s; }
void *import_data(const char *name) { (void)name; return NULL; }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
int nnfs_selftest(Ctx *c);
int nnfsdir_selftest(Ctx *c);

int main(void) {
    static Ctx c;
    setvbuf(stdout, NULL, _IONBF, 0);
    int a = nnfs_selftest(&c), b = nnfsdir_selftest(&c);
    printf("nnfs: %d failures, nnfsdir: %d failures\n", a, b);
    return a + b;
}
