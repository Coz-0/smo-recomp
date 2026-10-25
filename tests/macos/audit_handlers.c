/* Handlers in the shapes the runtime's tables use, compiled on their own (test_audit_a64.c reads their machine code). */
#include <stdio.h>
#include "runtime.h"

void h_nothing(Ctx *c) { (void)c; }
void h_ret0(Ctx *c) { c->x[0] = 0; }
void h_ret1(Ctx *c) { c->x[0] = 1; }
void h_ret_neg1(Ctx *c) { c->x[0] = (uint64_t)-1; }
void h_ret_big(Ctx *c) { c->x[0] = 0x123456789abcull; }
void h_ret_mask(Ctx *c) { c->x[0] = 0xff00ff00ff00ff00ull; }
void h_ret_u32(Ctx *c) { c->x[0] = 0x80000000u; }
void h_two(Ctx *c) { c->x[0] = 0; c->x[1] = 5; }
void h_pair_zero(Ctx *c) { c->x[0] = 0; c->x[1] = 0; }
void h_vec_zero(Ctx *c) { memset(&c->v[0], 0, sizeof c->v[0]); c->x[0] = 0; }
/* real code: these must not be called trivial */
void h_copy(Ctx *c) { c->x[0] = c->x[1]; }
void h_add(Ctx *c) { c->x[0] = c->x[1] + 3; }
void h_call(Ctx *c) { c->x[0] = (uint64_t)puts((const char *)(uintptr_t)c->x[0]); }
void h_load(Ctx *c) { c->x[0] = rd64(c->x[0]); }
void h_global(Ctx *c);
uint64_t g_seen;
void h_global(Ctx *c) { g_seen = c->x[0]; }
void h_global_vec(Ctx *c);
V128 g_vec;
void h_global_vec(Ctx *c) { c->v[0] = g_vec; }
