/* NVN handle values: see nvn_handles.c. */
#ifndef NVN_HANDLES_H
#define NVN_HANDLES_H
#include "runtime.h"
#include "hostrt.h"
ImportFn nvn_gl_pick(const char *name);         /* nvn_gl.c */
/* Part of the SDK self-test: the five handle functions against the values the SDK's code gives (emulator on sdk.elf). */
int nvn_handles_selftest(Ctx *c);
#endif
