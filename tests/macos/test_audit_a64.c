/* hostrt.c's do-nothing handler test on AArch64 (audit_a64.h), against handlers compiled by the host compiler. */
#include <stdio.h>
#include "runtime.h"
#include "audit_a64.h"

#define H(n) void n(Ctx *c);
H(h_nothing) H(h_ret0) H(h_ret1) H(h_ret_neg1) H(h_ret_big) H(h_ret_mask) H(h_ret_u32) H(h_two) H(h_pair_zero) H(h_vec_zero)
H(h_copy) H(h_add) H(h_call) H(h_load) H(h_global) H(h_global_vec)

static int g_fail, g_pass;
static void expect(const char *name, void (*fn)(Ctx *), int kind, int sets, long long x0) {
    long long got_x0;
    int got_sets, got = audit_trivial_a64((const void *)fn, &got_x0, &got_sets);
    int ok = got == kind && (kind != 2 || (got_sets == sets && (!sets || got_x0 == x0)));
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL %s: kind %d sets %d x0 0x%llx (want %d %d 0x%llx)\n", name, got, got_sets, (unsigned long long)got_x0, kind, sets, (unsigned long long)x0); }
}

int main(void) {
    expect("nothing", h_nothing, 1, 0, 0);
    expect("ret0", h_ret0, 2, 1, 0);
    expect("ret1", h_ret1, 2, 1, 1);
    expect("ret -1", h_ret_neg1, 2, 1, -1);
    expect("ret 0x123456789abc", h_ret_big, 2, 1, 0x123456789abcll);
    expect("ret bitmask", h_ret_mask, 2, 1, (long long)0xff00ff00ff00ff00ull);
    expect("ret 0x80000000", h_ret_u32, 2, 1, 0x80000000ll);
    expect("two stores", h_two, 2, 1, 0);
    expect("pair of zeros", h_pair_zero, 2, 1, 0);
    expect("zeroed vector", h_vec_zero, 2, 1, 0);
    expect("copy", h_copy, 0, 0, 0);
    expect("add", h_add, 0, 0, 0);
    expect("call", h_call, 0, 0, 0);
    expect("load", h_load, 0, 0, 0);
    expect("global", h_global, 0, 0, 0);
    expect("global vector", h_global_vec, 0, 0, 0);
    printf("audit_a64: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail != 0;
}
