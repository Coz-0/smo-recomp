/* Host runtime interface for the fully lifted game. */
#ifndef RECOMP_HOSTRT_H
#define RECOMP_HOSTRT_H

#include <stdio.h>
#include <stdarg.h>
#include "runtime.h"

#define GUEST_BASE 0x7100000000ULL
#include "image.h"               /* generated per build: tools/lifter/gen_tables.py */
/* SMO port: code that used a Super Smash Bros. Ultimate 13.0.4 address for a Smash-only diagnostic or test and has no
 * SMO equivalent yet is disabled through this constant (never used for game behaviour). */
#define SMO_UNPORTED 0
#define IMPORT_BASE 0x7300000000ULL      /* placeholder addresses for symbols of other modules */
#define IMPORT_SLOT 256ULL

typedef void (*ImportFn)(Ctx *c);
typedef struct { const char *name; ImportFn fn; } ImportDef;

#define HOSTFN_BASE 0x7400000000ULL      /* placeholder addresses for host functions handed out at run time */

uint64_t hostfn_address(const char *name, ImportFn fn);
const char *hostfn_current_name(void);
unsigned long long hostfn_current_calls(void);
void print_hostfn_summary(FILE *out);
void report_and_exit(int code);
void runtime_init(const char *elf, const char *trace_path);
void *import_data(const char *name);
/* Make an imported data symbol resolve to host memory (call before runtime_init). */
void import_set_address(const char *name, void *addr);
typedef struct { const char *name; const unsigned char *bytes; unsigned size; } SdkData;
extern const SdkData sdk_data[];
extern const int sdk_data_count;
void register_imports(const ImportDef *defs, int count);
ImportFn import_lookup(const char *name);    /* registered handler of an imported function, or NULL */
void call_guest(Ctx *c, uint64_t addr);
uint64_t guest_function_of(uint64_t host_rip);
void print_import_summary(FILE *out);
void trace_printf(const char *fmt, ...);

#endif
