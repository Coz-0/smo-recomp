/* gfx/nvn_stub.c's virtual memory pools on macOS, without the game: the real nvnMemoryPool* entry points (fetched through
 * nvnBootstrapLoader), over a heap made as smo_nn.c makes it on macOS (reserved, then committed). Checks that a virtual
 * pool shows the same bytes as the physical pool range mapped into it, in both directions, through splits, remaps, unmaps,
 * heap growth and pages never touched before; that requests a view cannot express are refused, not approximated.
 * Exit code = failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/mman.h>
#endif
#include "win32_posix.h"
#include "runtime.h"
#include "hostrt.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- stand-ins for the rest of the runtime */
void guest_trap(const char *what, uint64_t addr) { (void)what; (void)addr; fflush(stdout); _exit(42); }   /* the game stops there */
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)name; return (uint64_t)(uintptr_t)fn; }
const char *hostfn_current_name(void) { return "?"; }
unsigned long long hostfn_current_calls(void) { return 1000; }      /* quiet: no request printing */
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }
void nvn_gl_memory_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
ImportFn nvn_gl_pick(const char *name) { (void)name; return NULL; }
void nvn_label_set(uint64_t obj, const char *text) { (void)obj; (void)text; }
void nvn_label_drop(uint64_t obj) { (void)obj; }
static ImportFn g_bootstrap;
void register_imports(const ImportDef *defs, int count) {
    for (int i = 0; i < count; i++) if (!strcmp(defs[i].name, "nvnBootstrapLoader")) g_bootstrap = defs[i].fn;
}
void register_nvn_imports(void);

/* the heap, as smo_nn.c on macOS: reserved memory, committed in steps; its "section" is its base */
#define HEAP_RESERVE (1ull << 30)
static uint8_t *g_heap;
static uint64_t g_heap_size;
int smo_heap_section_handle(HANDLE *h) { *h = g_heap; return g_heap != NULL; }
#ifdef __linux__
static int g_heap_fd = -1;                    /* Linux, as smo_nn.c: the heap is a shared mapping of a memfd */
int smo_heap_memfd(void) { return g_heap_fd; }
#endif
int smo_heap_section_offset(uint64_t addr, uint64_t size, HANDLE *section, uint64_t *offset) {
    uint64_t base = (uint64_t)(uintptr_t)g_heap;
    if (!g_heap || addr < base || addr + size > base + g_heap_size || addr + size < addr) return 0;
    *section = g_heap;
    *offset = addr - base;
    return 1;
}
static void heap_grow(uint64_t size) { if (VirtualAlloc(g_heap + g_heap_size, size - g_heap_size, MEM_COMMIT, PAGE_READWRITE)) g_heap_size = size; }

/* ---- calling the entry points */
static ImportFn fn(const char *name) {
    Ctx c; memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)name;
    g_bootstrap(&c);
    return (ImportFn)(uintptr_t)c.x[0];
}
static uint64_t call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2) {
    Ctx c; memset(&c, 0, sizeof c);
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[30] = 0x1004;
    fn(name)(&c);
    return c.x[0];
}
static uint64_t g_key = 0x100000;                                   /* guest objects: only their addresses matter */
static uint64_t pool(uint64_t memory, uint64_t size, uint64_t flags) {
    uint64_t b = g_key += 0x100, p = g_key += 0x100;
    call("nvnMemoryPoolBuilderSetDefaults", b, 0, 0);
    call("nvnMemoryPoolBuilderSetStorage", b, memory, size);
    call("nvnMemoryPoolBuilderSetFlags", b, flags, 0);
    call("nvnMemoryPoolInitialize", p, b, 0);
    return p;
}
static uint8_t *mem(uint64_t p) { return (uint8_t *)(uintptr_t)call("nvnMemoryPoolMap", p, 0, 0); }
typedef struct { uint64_t phys, poff, voff, size; uint32_t cls, pad; } Req;   /* 0x28 bytes, as the game passes them */
static int map(uint64_t v, uint64_t phys, uint64_t poff, uint64_t voff, uint64_t size) {   /* 1: done */
    Req r = {phys, poff, voff, size, 0, 0};
    return call("nvnMemoryPoolMapVirtual", v, 1, (uint64_t)(uintptr_t)&r) == 1;
}
static int refused(uint64_t v, uint64_t phys, uint64_t poff, uint64_t voff, uint64_t size) {   /* the request stops the game */
    pid_t pid = fork();
    if (pid == 0) { int fd = open("/dev/null", 1); dup2(fd, 1); map(v, phys, poff, voff, size); _exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 42;
}
#ifdef __linux__
static int no_access(const void *a) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(a, &mi, sizeof mi) && mi.State == MEM_RESERVE;
}
#else
static int no_access(const void *a) {
    mach_vm_address_t at = (uintptr_t)a;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj)) return 0;
    return at <= (uintptr_t)a && info.protection == VM_PROT_NONE;
}
#endif
static int same(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }
static void fill(uint8_t *p, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7 + (i >> 12)); }

#define K64 0x10000ull
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    register_nvn_imports();
#ifdef __linux__
    g_heap_fd = memfd_create("test-heap", MFD_CLOEXEC);
    if (g_heap_fd < 0 || ftruncate(g_heap_fd, (off_t)HEAP_RESERVE)) return 1;
    g_heap = mmap(NULL, HEAP_RESERVE, PROT_NONE, MAP_SHARED | MAP_NORESERVE, g_heap_fd, 0);
#else
    g_heap = VirtualAlloc(NULL, HEAP_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
#endif
    heap_grow(64 << 20);
    CHECK(g_heap && g_bootstrap, "heap and nvnBootstrapLoader");

    uint64_t P = pool((uint64_t)(uintptr_t)g_heap + 0x100000, 0x400000, 0x8), V = pool(0, 0x200000, 0x8 | 0x200);
    uint8_t *pm = mem(P), *vm = mem(V);
    CHECK(vm && vm != pm && no_access(vm), "a virtual pool is a reserved range of its own, unmapped (at %p)", (void *)vm);

    /* 1. physical +0x10000 -> virtual +0, 0x40000 bytes: the same bytes both ways */
    fill(pm + 0x10000, 0x40000, 1);
    CHECK(map(V, P, 0x10000, 0, 0x40000), "map 0x40000 bytes");
    CHECK(same(vm, pm + 0x10000, 0x40000), "the virtual pool shows what was written through the physical pool");
    fill(vm + 0x8000, 0x100, 9);
    CHECK(same(vm + 0x8000, pm + 0x18000, 0x100), "a write through the virtual pool is seen through the physical pool");

    /* 2. a part of the mapped range is mapped again elsewhere: splits; the rest keeps its pages */
    fill(pm + 0x200000, K64, 77);
    CHECK(map(V, P, 0x200000, 0x10000, K64), "remap the second 64 KB of a mapped range");
    CHECK(same(vm + 0x10000, pm + 0x200000, K64) && same(vm, pm + 0x10000, K64) && same(vm + 0x20000, pm + 0x30000, 0x20000),
          "after a split: the remapped part shows the new pages, both sides keep the old ones");

    /* 3. a request without a pool unmaps; mapping again shows the heap's bytes, which were never lost */
    CHECK(map(V, 0, 0, 0, 0x40000), "unmap the range (a request without a physical pool)");
    CHECK(no_access(vm) && no_access(vm + 0x30000), "the unmapped range has no access, as on the console");
    CHECK(map(V, P, 0x10000, 0x100000, 0x40000), "map at another virtual offset");
    CHECK(same(vm + 0x100000, pm + 0x10000, 0x40000) && vm[0x100000 + 0x8000] == pm[0x18000], "the bytes are still the heap's");

    /* 4. pages nobody touched yet, mapped first and written through the virtual pool */
    CHECK(map(V, P, 0x300000, 0x180000, K64), "map pages never touched");
    fill(vm + 0x180000, K64, 33);
    CHECK(same(vm + 0x180000, pm + 0x300000, K64), "untouched pages are shared too (no private copy on the first write)");

    /* 5. the heap grows after a mapping: the views stay what they are */
    heap_grow(128 << 20);
    vm[0x100000] ^= 0x5a;
    CHECK(vm[0x100000] == pm[0x10000], "after the heap grew, a view still shares its pages");
    uint64_t P2 = pool((uint64_t)(uintptr_t)g_heap + (100 << 20), 0x100000, 0x8);
    fill(mem(P2), K64, 5);
    CHECK(map(V, P2, 0, 0x1f0000, K64) && same(vm + 0x1f0000, mem(P2), K64), "a pool in the grown part maps too");

    /* 6. refused, not approximated */
    CHECK(refused(V, P, 0x10000, 0x1000, K64), "a virtual offset that is not a multiple of 64 KB is refused");
    CHECK(refused(V, P, 0x11000, 0x40000, K64), "a 4 KB-aligned physical offset for part of a pool is refused");
    uint8_t *outside = VirtualAlloc(NULL, 0x40000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint64_t Q = pool((uint64_t)(uintptr_t)outside, 0x40000, 0x8);
    CHECK(refused(V, Q, 0, 0, K64), "memory outside the heap for part of a pool is refused");

    /* 7. the one exact case a view cannot express: a whole, unused virtual pool on a 4 KB-aligned physical range */
    uint64_t W = pool(0, 0x20000, 0x8 | 0x200);
    CHECK(map(W, P, 0x1000, 0, 0x20000), "whole-pool request at a 4 KB-aligned physical offset");
    CHECK(mem(W) == pm + 0x1000, "the pool becomes a direct alias of the physical range");

    printf("vpool: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    void nvn_virtual_pool_report(FILE *out);
    nvn_virtual_pool_report(stdout);
    return g_fail != 0;
}
