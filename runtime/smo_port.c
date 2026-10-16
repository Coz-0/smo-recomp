/* smo_port.c - SMO port: the interfaces of the ssbu-recomp modules that are Smash-only and not part of this port.
 *
 *   nnro.c         nn::ro run-time script modules (Smash's lua2cpp_* NROs). SMO's main imports no nn::ro function
 *                  (checked against build/gen/imports_all.txt), so no run-time module can exist: every query answers
 *                  "not a module address", which is the truth for this game.
 *   debug_mode.c   Smash's test fast mode (fighters, stocks, stage chosen from the environment). An SMO test mode is
 *                  planned separately; until then it is off, exactly as ssbu's was with no SSBU_* variable set.
 *
 * Nothing here answers a call of the GAME: these are hooks the rest of the runtime calls. */
#include <stdio.h>
#include "hostrt.h"
#include "eh_runtime.h"

typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;     /* as nnunwind.c declares it */

int nro_call_indirect(Ctx *c, uint64_t addr) { (void)c; (void)addr; return 0; }
uint64_t nro_guest_function_of(uint64_t rip) { (void)rip; return 0; }
int nro_unwind_module(uint64_t pc, uint64_t *base, uint64_t *size, const NroResume **tab, int *count, const char **name)
{ (void)pc; (void)base; (void)size; (void)tab; (void)count; (void)name; return 0; }
void module_coverage_start(Ctx *main_ctx) { (void)main_ctx; }
void module_coverage_report(FILE *out) { (void)out; }
void register_ro_imports(void) {}

/* fighter_sdk/fighter_host.c (Smash): how many scripted-input steps to advance per pad poll. Outside a deterministic
 * Smash fight it answered 1, which is the scripted-input rule for SMO too (one step per poll of player 1). */
int fight_seq_ticks(void) { return 1; }

void debug_mode_init(void) {}
void debug_mode_frame(long long frame) { (void)frame; }
