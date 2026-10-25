/* nntime.c's time zone rule on macOS, without the rest of the runtime: the file is included whole so its self-test
 * (SMO_SDK_SELFTEST's) can run here, once per zone given (each zone in a process of its own: the rule is built once).
 *   test_nntime <IANA zone>   exit code = failures */
#include <stdlib.h>
#include "nntime.c"

void host_clock_caller(const char *what, uint64_t lr) { (void)what; (void)lr; }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }

int main(int argc, char **argv) {
    if (argc > 1) setenv("TZ", argv[1], 1);
    static Ctx c;
    return nntime_selftest(&c);
}
