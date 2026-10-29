/* Host runtime interface for the fully lifted game. */
#ifndef RECOMP_HOSTRT_H
#define RECOMP_HOSTRT_H

#include <stdio.h>
#include <stdarg.h>
#include "runtime.h"

/* Where the guest's modules are mapped: the console's own addresses on Windows. macOS keeps 0x70_0000_0000 ..
 * 0x80_0000_0000 for malloc's heap range, so there everything is 0x10_0000_0000 higher (docs/macos-port.md). The lifted
 * code is made for these bases: tools/target.py has the same numbers and image.h says which ones it was made for. */
#ifdef __APPLE__
#define GUEST_LAYOUT_SHIFT 0x1000000000ULL
#else
#define GUEST_LAYOUT_SHIFT 0ULL
#endif
#define GUEST_BASE (0x7100000000ULL + GUEST_LAYOUT_SHIFT)
#define SDK_BASE (0x7500000000ULL + GUEST_LAYOUT_SHIFT)     /* the SDK module (sdklift_rt.c) */
#include "image.h"               /* generated per build: tools/lifter/gen_tables.py */
#if defined(GEN_GUEST_BASE) && (GEN_GUEST_BASE != GUEST_BASE || GEN_SDK_BASE != SDK_BASE)
#error "build/gen/image.h was generated for other guest bases: run scripts/setup.py again on this system"
#endif
#define IMPORT_BASE (0x7300000000ULL + GUEST_LAYOUT_SHIFT)      /* placeholder addresses for symbols of other modules */
#define IMPORT_SLOT 256ULL

typedef void (*ImportFn)(Ctx *c);
typedef struct { const char *name; ImportFn fn; } ImportDef;

#define HOSTFN_BASE (0x7400000000ULL + GUEST_LAYOUT_SHIFT)      /* placeholder addresses for host functions handed out at run time */

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
ImportFn handler_by_name(const char *name);  /* the handler registered under a name, imported or not, or NULL */
void call_guest(Ctx *c, uint64_t addr);
uint64_t guest_function_of(uint64_t host_rip);
void print_import_summary(FILE *out);
void trace_printf(const char *fmt, ...);

#endif
