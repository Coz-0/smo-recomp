/* Link check of the lifted main (tools/lifter/linkcheck.py): the runtime interface the generated code needs
 * (runtime.h), as do-nothing stubs. Linking every object of build/obj/main with ONLY this file proves that every
 * f_<addr> the lifted code calls is defined by the lifted code itself; nothing here is the real runtime. */
#include "runtime.h"

void call_indirect(Ctx *c, uint64_t a) { (void)c; (void)a; }
void host_import(Ctx *c, int sym) { (void)c; (void)sym; }
void guest_trap(const char *what, uint64_t addr) { (void)what; (void)addr; }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { (void)pc; (void)insn; (void)what; }
void guest_svc(Ctx *c, uint32_t n) { (void)c; (void)n; }
void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int bits)
{ (void)c; (void)a; (void)e; (void)s; (void)v; (void)bits; }

/* exported so the linker keeps the whole table (and with it every lifted function) */
__declspec(dllexport) const FnEntry *linkcheck_table(void) { return fn_table; }
__declspec(dllexport) int linkcheck_count(void) { return fn_table_count; }
__declspec(dllexport) const DynSym *linkcheck_syms(void) { return dyn_syms; }
