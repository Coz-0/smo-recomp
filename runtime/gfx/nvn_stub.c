/* Graphics entry points: hands out a host function for every NVN name the game asks for.
 * Device information, memory pools and buffers are here; everything else is in nvn_gl.c (OpenGL).
 * A name that neither implements is MISSING: it is announced when the game fetches it, and calling it stops the game with
 * the function's name (SMO_NVN_MISSING=warn: report the call, answer 0 and go on, for exploration). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <errno.h>
#endif
#endif
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

static void log_call(Ctx *c) {
    if (hostfn_current_calls() <= 3)
        trace_printf("     nvn %s  x0=%llx x1=%llx x2=%llx x3=%llx  from=%llx\n", hostfn_current_name(),
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[3], (unsigned long long)c->x[30] - 4);
}
/* An entry point with no implementation. */
static int missing_warn_only(void) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("SMO_NVN_MISSING"); mode = e && !strcmp(e, "warn"); }
    return mode;
}
static void nvn_generic(Ctx *c) {
    log_call(c);
    printf("nvn: MISSING function %s was called (x0=%llx x1=%llx x2=%llx x3=%llx)%c", hostfn_current_name(), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
           (unsigned long long)c->x[2], (unsigned long long)c->x[3], 10);
    fflush(stdout);
    if (!missing_warn_only()) {
        printf("nvn: stopping: %s has no implementation (SMO_NVN_MISSING=warn to continue with a zero answer)%c", hostfn_current_name(), 10);
        fflush(stdout);
        report_and_exit(6);
    }
    RET(0);
}
/* nvnDeviceGetInteger(device, pname, int *out): device capabilities.
 * Only the values the boot has been seen to depend on are filled in; everything else
 * answers 0 and is counted, so the list of parameters the game really queries is known.
 * The numbers are provisional and must be checked against a reference implementation. */
/* NVNdeviceInfo values of a real Switch, indexed by pname (enum order from the SDK's nvn.h
 * as published in the BotW decomp; values from an NVN device-info dump of a development unit). */
static const int g_devinfo[0x62] = {
    53, 312, 14, 65536, 256, 8, 16, 4, 16, 32, 16, 4, 4, 4, 16, 32,             /* 0x00..0x0f */
    32, 256, 256, 4, 8, 512, 256, 2, 256, 4096, 131072, 16, 1536, 1024, 64, 1536, /* 0x10..0x1f */
    65535, 65535, 65535, 8, 1048576, 4096, 16, 4096, 1, 1, 1, 262144, 15, 2048, 1, 1, /* 0x20..0x2f */
    15, 5, 1, 8, 8, 4, 32, 128, 32, 128, 65536, 1, 4, 16384, 134217728, 2048,  /* 0x30..0x3f */
    16384, 16384, 1, 1, 1, 1, 5, 1, 1, 1, 1, 1, 1, 16, 1024, 32,                /* 0x40..0x4f */
    4096, 65536, 65536, 256, 16384, 262144, 4096, 1, 4, 2, 1, 4096, 16384, 4096, 128, 32, /* 0x50..0x5f */
    65535, 1,                                                                    /* 0x60..0x61 */
};
static volatile LONG g_devinfo_queries[256];
static void nvn_get_integer(Ctx *c) {
    uint32_t pname = (uint32_t)A1;
    int value = pname < 0x62 ? g_devinfo[pname] : 0;
    if (pname < 256 && InterlockedIncrement(&g_devinfo_queries[pname]) == 1)
        trace_printf("     nvnDeviceGetInteger pname=0x%x -> %d  from=%llx\n", pname, value, (unsigned long long)c->x[30] - 4);
    if (A2) wr32(A2, (uint32_t)value);
}
void print_devinfo_summary(FILE *out) {
    fprintf(out, "\nnvnDeviceGetInteger parameters queried (pname:count):");
    for (int i = 0; i < 256; i++) if (g_devinfo_queries[i]) fprintf(out, " %x:%ld", i, (long)g_devinfo_queries[i]);
    fprintf(out, "\n");
}

/* ---- memory pools and buffers: the game supplies the memory, so mapping must work for real.
 * Guest objects are opaque; what each one refers to is kept in a side table keyed by address. */
typedef struct NvnObj { uint64_t key, memory, size, pool, offset, flags; struct NvnObj *next; } NvnObj;
/* Looked up on every nvnBufferGetAddress and the like (about 8% of the main thread's busy time in New Donk City with one
 * exclusive lock and 4096 buckets): a shared lock for the lookup, the exclusive one only to add, 65536 buckets. */
#define NOBJ_N 65536
static NvnObj *g_objs[NOBJ_N];
static SRWLOCK g_objs_lock = SRWLOCK_INIT;
static NvnObj *nobj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (NOBJ_N - 1);
    AcquireSRWLockShared(&g_objs_lock);
    NvnObj *o = g_objs[h];
    while (o && o->key != key) o = o->next;
    ReleaseSRWLockShared(&g_objs_lock);
    if (o) return o;
    AcquireSRWLockExclusive(&g_objs_lock);
    o = g_objs[h];
    while (o && o->key != key) o = o->next;
    if (!o) { o = calloc(1, sizeof *o); o->key = key; o->next = g_objs[h]; g_objs[h] = o; }
    ReleaseSRWLockExclusive(&g_objs_lock);
    return o;
}
static void nobj_drop(uint64_t key) {                                   /* the object is finalized: forget what it referred to */
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (NOBJ_N - 1);
    AcquireSRWLockExclusive(&g_objs_lock);
    for (NvnObj **pp = &g_objs[h]; *pp; pp = &(*pp)->next)
        if ((*pp)->key == key) { NvnObj *o = *pp; *pp = o->next; free(o); break; }
    ReleaseSRWLockExclusive(&g_objs_lock);
}
void nvn_label_set(uint64_t obj, const char *text);                    /* nvn_gl.c */
void nvn_label_drop(uint64_t obj);
void nvn_gl_memory_written(uint64_t addr, uint64_t size);
static uint64_t g_device;
static void builder_defaults(Ctx *c) { NvnObj *b = nobj(A0); b->memory = b->size = b->pool = b->offset = b->flags = 0; }      /* nvnMemoryPoolBuilderSetDefaults / nvnBufferBuilderSetDefaults */
static void builder_set_device(Ctx *c) { if (A1) g_device = A1; }      /* (builder, device): there is one device */
static void builder_get_device(Ctx *c) { RET(g_device); }
static void pool_builder_get_memory(Ctx *c) { RET(nobj(A0)->memory); }
static void pool_builder_get_size(Ctx *c) { RET(nobj(A0)->size); }
static void pool_builder_get_flags(Ctx *c) { RET(nobj(A0)->flags); }
static void pool_finalize(Ctx *c) { nvn_label_drop(A0); nobj_drop(A0); }
static void obj_set_label(Ctx *c) { nvn_label_set(A0, A1 ? (const char *)(uintptr_t)A1 : ""); }
/* FlushMappedRange(object, offset, size): the CPU wrote this range and the GPU must see it. Pool memory is ordinary host memory here,
 * so what has to be told is the backend, which keeps copies of textures. */
static void obj_flush_range(Ctx *c) { NvnObj *o = nobj(A0); if (o->memory) nvn_gl_memory_written(o->memory + A1, A2); }
static void buffer_builder_get_pool(Ctx *c) { RET(nobj(A0)->pool); }
static void buffer_builder_get_offset(Ctx *c) { RET(nobj(A0)->offset); }
static void buffer_builder_get_size(Ctx *c) { RET(nobj(A0)->size); }
static void buffer_get_pool(Ctx *c) { RET(nobj(A0)->pool); }
static void buffer_get_offset(Ctx *c) { RET(nobj(A0)->offset); }
static void buffer_finalize(Ctx *c) { nvn_label_drop(A0); nobj_drop(A0); }
static void obj_debug_id(Ctx *c) { RET(A0); }
static void pool_builder_storage(Ctx *c) { log_call(c); NvnObj *b = nobj(A0); b->memory = A1; b->size = A2; }   /* (builder, memory, size) */
static void pool_builder_flags(Ctx *c) { nobj(A0)->flags = A1; }                                               /* (builder, flags) */
static void pool_virtual_init(Ctx *c, NvnObj *p);            /* below */
static void pool_initialize(Ctx *c) {
    log_call(c);
    NvnObj *p = nobj(A0), *b = nobj(A1);
    p->memory = b->memory; p->size = b->size; p->flags = b->flags;
    if (p->flags & 0x200) { pool_virtual_init(c, p); RET(1); return; }   /* NVN_MEMORY_POOL_FLAGS_VIRTUAL */
    RET(1);
}
static void pool_flags(Ctx *c) { RET(nobj(A0)->flags); }
/* nvnMemoryPoolMapVirtual(pool, numRequests, const NVNmappingRequest *requests) [sdk 0x41ec70]: takes the NVN lock and calls the
 * pool's method +0x60. Which class a pool is, is decided at nvnMemoryPoolInitialize by flag bit 9 (0x200, "virtual")
 * [0x41eae0 .. 0x41eb00 -> 0x41eda0]:
 *   ordinary pool (class [0xc13f30]): method +0x60 = [0x42acd0] = `return 0`: that is the whole function there.
 *   virtual pool (class [0xc13fc0]): [0x42b490] turns each 0x28-byte request {+0 physical pool, +8 offset in it, +0x10 offset in
 *     this pool, +0x18 size, +0x20 storage class} into an entry {nvmap handle of the physical pool (its method +0x50), class &
 *     ~0x100, both offsets and the size in pages of the device's page size} and gives them, at most (pool +0x50) at a time, to
 *     NvRmGpuAddressSpaceAllocationRemap: the GPU address range of the virtual pool then shows the physical pool's pages
 *     (a request without a pool unmaps). 1 when every batch succeeded.
 * NOT IMPLEMENTED for a virtual pool: here a pool is the game's own host memory and every texture reads its texels through one
 * contiguous pointer (nvn_gl.c Tex.mem); a virtual pool has no memory of its own (its builder must pass none [0x42b208]) and its
 * pages alias pages of other pools, which needs either operating-system aliasing of memory the game allocated (not possible
 * for memory that was not created as a section) or a page table consulted by every texture read / write-back in nvn_gl.c.
 * So the name stays without a handler (MISSING: a call stops with the function's name), also for ordinary pools, where the
 * answer would be 0. */
/* ---- VIRTUAL memory pools (flag 0x200) and nvnMemoryPoolMapVirtual.
 * SMO maps pages of physical pools into a virtual pool (agl::detail::PrivateResource, at start-up), so the note above no
 * longer holds for this game. Implementation: the game's memory heap is a view of a pagefile-backed section (smo_nn.c),
 * and pools hold memory the game took from that heap. A virtual pool gets a reserved PLACEHOLDER range of its size as its
 * "memory"; each mapping request {+0 physical pool, +8 offset in it, +0x10 offset in the virtual pool, +0x18 size,
 * +0x20 storage class} replaces the placeholder part with a view of the same section pages (MapViewOfFile3), so both
 * addresses show the same bytes, as the GPU address space does on the console. A request without a physical pool turns
 * the range back into a placeholder. The storage class is the GPU memory kind; the host has none to apply.
 * Stops with the request printed (never maps approximately) when: the physical memory is not in the heap, an offset or
 * size is not a multiple of 64 KB (the host's mapping granularity), or a Windows call fails.
 * macOS: the same with Mach VM (vp_* below). A placeholder is reserved memory (PROT_NONE); a view is the heap's own pages
 * at a second address, mach_vm_remap without copying (the heap is plain reserved memory there, smo_nn.c); splitting and
 * joining placeholders needs nothing. 64 KB stays the granularity, so both systems accept and refuse the same requests. */
#define VP_GRAN 0x10000ull
#define VP_MAXSEG 4096
typedef struct { uint64_t start, size; int mapped; uint64_t sec_off; } VpSeg;     /* start: offset in the virtual pool */
/* used: the pool's memory address was handed out (pool map / buffer / texture) before any mapping;
 * direct: the pool IS a physical pool's range (see pool_map_virtual: whole-pool request that views cannot express) */
typedef struct VPool { uint64_t key, base, size; int nseg, used, direct; VpSeg seg[VP_MAXSEG]; struct VPool *next; } VPool;
static VPool *g_vpools;
static SRWLOCK g_vp_lock = SRWLOCK_INIT;
static unsigned long long g_vp_maps, g_vp_unmaps;
int smo_heap_section_offset(uint64_t addr, uint64_t size, HANDLE *section, uint64_t *offset);     /* smo_nn.c */
int smo_heap_section_handle(HANDLE *h);                       /* smo_nn.c */
static void vp_fail(Ctx *c, const char *what, uint64_t a, uint64_t b) {
#ifdef _WIN32
    const char *os = "Windows";
#else
#ifdef __APPLE__
    const char *os = "Mach";
#else
    const char *os = "POSIX";
#endif
#endif
    printf("\nnvn: virtual memory pool: %s (0x%llx, 0x%llx; %s error %lu)\n", what, (unsigned long long)a, (unsigned long long)b, os, (unsigned long)GetLastError());
    guest_trap("nvnMemoryPoolMapVirtual: request that cannot be mapped exactly", c ? c->x[30] - 4 : 0);
}
/* The operating system's part: reserve a placeholder range, show section pages in a placeholder, take a view out again
 * (back to a placeholder), split / join placeholders, release a whole placeholder range. Each answers 1 when it worked. */
#ifdef _WIN32
typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn p_VirtualAlloc2;
static MapViewOfFile3Fn p_MapViewOfFile3;
static UnmapViewOfFile2Fn p_UnmapViewOfFile2;
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
static void vp_load(Ctx *c) {
    if (p_MapViewOfFile3) return;
    HMODULE k = GetModuleHandleA("kernelbase.dll");
    p_VirtualAlloc2 = (VirtualAlloc2Fn)(void *)GetProcAddress(k, "VirtualAlloc2");
    p_MapViewOfFile3 = (MapViewOfFile3Fn)(void *)GetProcAddress(k, "MapViewOfFile3");
    p_UnmapViewOfFile2 = (UnmapViewOfFile2Fn)(void *)GetProcAddress(k, "UnmapViewOfFile2");
    if (!p_VirtualAlloc2 || !p_MapViewOfFile3 || !p_UnmapViewOfFile2) vp_fail(c, "placeholder API missing (Windows 10 1803 or newer is needed)", 0, 0);
}
static uint64_t vp_reserve(uint64_t size) {
    return (uint64_t)(uintptr_t)p_VirtualAlloc2(GetCurrentProcess(), NULL, (SIZE_T)size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
}
static int vp_view(HANDLE sec, uint64_t at, uint64_t sec_off, uint64_t size) {
    return p_MapViewOfFile3(sec, GetCurrentProcess(), (PVOID)(uintptr_t)at, sec_off, (SIZE_T)size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) == (PVOID)(uintptr_t)at;
}
static int vp_unview(uint64_t at, uint64_t size) { (void)size; return p_UnmapViewOfFile2(GetCurrentProcess(), (PVOID)(uintptr_t)at, MEM_PRESERVE_PLACEHOLDER) != 0; }
static int vp_split_placeholder(uint64_t at, uint64_t size) { return VirtualFree((PVOID)(uintptr_t)at, (SIZE_T)size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0; }
static int vp_join_placeholders(uint64_t at, uint64_t size) { return VirtualFree((PVOID)(uintptr_t)at, (SIZE_T)size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0; }
static int vp_release(uint64_t at, uint64_t size) { (void)size; return VirtualFree((PVOID)(uintptr_t)at, 0, MEM_RELEASE) != 0; }
#else
static void vp_load(Ctx *c) { (void)c; }
static uint64_t vp_reserve(uint64_t size) {
    void *p = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? 0 : (uint64_t)(uintptr_t)p;
}
#ifdef __linux__
int smo_heap_memfd(void);                                          /* smo_nn.c */
/* Linux: the heap is a shared mapping of a memfd (smo_nn.c): a view is the same file range mapped at the second address.
 * The caller has checked that the range lies in the committed heap (smo_heap_section_offset). */
static int vp_view(HANDLE sec, uint64_t at, uint64_t sec_off, uint64_t size) {
    (void)sec;
    int fd = smo_heap_memfd();
    if (fd < 0) { SetLastError(0); return 0; }
    void *p = mmap((void *)(uintptr_t)at, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)sec_off);
    if (p != (void *)(uintptr_t)at) { SetLastError((DWORD)errno); return 0; }
    return 1;
}
#else
static int vp_view(HANDLE sec, uint64_t at, uint64_t sec_off, uint64_t size) {   /* sec: the heap's base (smo_nn.c) */
    mach_vm_address_t to = at;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &to, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                     (mach_vm_address_t)(uintptr_t)sec + sec_off, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || to != at) { SetLastError((DWORD)kr); return 0; }
    if ((cur & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) { SetLastError(0); return 0; }   /* heap pages not committed */
    return 1;
}
#endif
static int vp_unview(uint64_t at, uint64_t size) {
    return mmap((void *)(uintptr_t)at, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) != MAP_FAILED;
}
static int vp_split_placeholder(uint64_t at, uint64_t size) { (void)at; (void)size; return 1; }
static int vp_join_placeholders(uint64_t at, uint64_t size) { (void)at; (void)size; return 1; }
static int vp_release(uint64_t at, uint64_t size) { return munmap((void *)(uintptr_t)at, (size_t)size) == 0; }
#endif
static VPool *vp_find(uint64_t key) { VPool *v = g_vpools; while (v && v->key != key) v = v->next; return v; }
/* Make segment boundaries at off (splits the segment containing it). Views are split by unmapping and mapping the two
 * halves again; placeholders by VirtualFree(MEM_PRESERVE_PLACEHOLDER). */
static int vp_map_view(Ctx *c, VPool *v, VpSeg *s);
static void vp_split(Ctx *c, VPool *v, uint64_t off) {
    for (int i = 0; i < v->nseg; i++) {
        VpSeg *s = &v->seg[i];
        if (off <= s->start || off >= s->start + s->size) continue;
        if (v->nseg >= VP_MAXSEG) vp_fail(c, "too many segments", v->size, off);
        memmove(&v->seg[i + 2], &v->seg[i + 1], sizeof(VpSeg) * (size_t)(v->nseg - i - 1));
        VpSeg left = *s, right = *s;
        left.size = off - s->start;
        right.start = off; right.size = s->start + s->size - off; right.sec_off = s->sec_off + left.size;
        if (s->mapped && !vp_unview(v->base + s->start, s->size)) vp_fail(c, "unmap for a split", v->base + s->start, s->size);
        if (!vp_split_placeholder(v->base + left.start, left.size)) vp_fail(c, "placeholder split", v->base + left.start, left.size);
        v->seg[i] = left; v->seg[i + 1] = right; v->nseg++;
        if (left.mapped) { vp_map_view(c, v, &v->seg[i]); vp_map_view(c, v, &v->seg[i + 1]); }
        return;
    }
}
static int vp_map_view(Ctx *c, VPool *v, VpSeg *s) {
    HANDLE sec;
    if (!smo_heap_section_handle(&sec)) vp_fail(c, "no heap section", 0, 0);
    if (!vp_view(sec, v->base + s->start, s->sec_off, s->size)) vp_fail(c, "view of heap pages", v->base + s->start, s->size);
    return 1;
}
static void pool_virtual_init(Ctx *c, NvnObj *p) {
    vp_load(c);
    if (p->memory) vp_fail(c, "a virtual pool was given memory of its own", p->memory, p->size);
    if (!p->size || p->size % VP_GRAN) vp_fail(c, "virtual pool size is not a multiple of 64 KB", p->size, 0);
    uint64_t base = vp_reserve(p->size);
    if (!base) vp_fail(c, "placeholder reservation", p->size, 0);
    VPool *v = calloc(1, sizeof *v);
    v->key = p->key; v->base = base; v->size = p->size;
    v->nseg = 1; v->seg[0].start = 0; v->seg[0].size = p->size;
    AcquireSRWLockExclusive(&g_vp_lock); v->next = g_vpools; g_vpools = v; ReleaseSRWLockExclusive(&g_vp_lock);
    p->memory = v->base;
    printf("nvn: virtual memory pool %llx: 0x%llx bytes at %llx (placeholder)\n", (unsigned long long)p->key, (unsigned long long)p->size, (unsigned long long)v->base);
}
static void pool_map_virtual(Ctx *c) {
    log_call(c);
    uint64_t pool = A0, n = A1, req = A2;
    vp_load(c);
    AcquireSRWLockExclusive(&g_vp_lock);
    VPool *v = vp_find(pool);
    if (!v) { ReleaseSRWLockExclusive(&g_vp_lock); vp_fail(c, "nvnMemoryPoolMapVirtual on a pool that is not virtual", pool, 0); }
    for (uint64_t k = 0; k < n; k++) {
        uint64_t r = req + k * 0x28, phys = rd64(r), poff = rd64(r + 8), voff = rd64(r + 0x10), size = rd64(r + 0x18);
        uint32_t cls = rd32(r + 0x20);
        if (hostfn_current_calls() <= 2 && k < 4)
            printf("nvn: MapVirtual pool %llx request %llu: physical pool %llx +0x%llx -> +0x%llx, 0x%llx bytes, storage class 0x%x\n",
                   (unsigned long long)pool, (unsigned long long)k, (unsigned long long)phys, (unsigned long long)poff, (unsigned long long)voff, (unsigned long long)size, cls);
        if (voff % VP_GRAN || size % VP_GRAN || !size || voff + size > v->size) vp_fail(c, "request offset / size (64 KB granularity, inside the pool)", voff, size);
        uint64_t sec_off = 0;
        HANDLE sec;
        if (v->direct) vp_fail(c, "a pool that is a direct alias of physical memory is mapped again", voff, size);
        if (phys) {
            NvnObj *pp = nobj(phys);
            if (vp_find(phys)) vp_fail(c, "the physical pool of a request is itself virtual", phys, 0);
            int in_heap = smo_heap_section_offset(pp->memory + poff, size, &sec, &sec_off);
            if (!in_heap || sec_off % VP_GRAN) {
                /* A view cannot express this (memory outside the heap, or a heap offset that is only 4 KB aligned: NVN pool
                 * memory needs 4 KB alignment, host views 64 KB). Exact only in one case: ONE request mapping the WHOLE,
                 * still unmapped and unused virtual pool from one contiguous physical range. Then the virtual pool's
                 * memory IS that range: its address becomes the physical address, which aliases exactly. */
                if (n != 1 || voff != 0 || size != v->size || v->nseg != 1 || v->seg[0].mapped || v->used)
                    vp_fail(c, in_heap ? "physical offset in the heap is not a multiple of 64 KB, and the request is not a whole-pool mapping of an unused pool"
                                       : "physical memory is not inside the memory heap, and the request is not a whole-pool mapping of an unused pool", pp->memory + poff, size);
                if (!vp_release(v->base, v->size)) vp_fail(c, "placeholder release", v->base, v->size);
                v->direct = 1;
                v->base = pp->memory + poff;
                nobj(pool)->memory = v->base;
                g_vp_maps++;
                printf("nvn: virtual memory pool %llx is a direct alias of physical pool %llx +0x%llx (0x%llx bytes at %llx): heap offset 0x%llx is not 64 KB aligned\n",
                       (unsigned long long)pool, (unsigned long long)phys, (unsigned long long)poff, (unsigned long long)size,
                       (unsigned long long)v->base, (unsigned long long)sec_off);
                continue;
            }
        }
        vp_split(c, v, voff);
        vp_split(c, v, voff + size);
        /* unmap every view inside [voff, voff+size), then make the range one placeholder */
        int first = -1, last = -1;
        for (int i = 0; i < v->nseg; i++) {
            VpSeg *s = &v->seg[i];
            if (s->start < voff || s->start >= voff + size) continue;
            if (first < 0) first = i;
            last = i;
            if (s->mapped) {
                if (!vp_unview(v->base + s->start, s->size)) vp_fail(c, "unmap", v->base + s->start, s->size);
                s->mapped = 0; g_vp_unmaps++;
            }
        }
        if (last > first) {
            if (!vp_join_placeholders(v->base + voff, size)) vp_fail(c, "placeholder coalesce", v->base + voff, size);
            memmove(&v->seg[first + 1], &v->seg[last + 1], sizeof(VpSeg) * (size_t)(v->nseg - last - 1));
            v->nseg -= last - first;
        }
        VpSeg *s = &v->seg[first];
        s->start = voff; s->size = size; s->mapped = 0;
        if (phys) { s->sec_off = sec_off; vp_map_view(c, v, s); s->mapped = 1; g_vp_maps++; }
    }
    ReleaseSRWLockExclusive(&g_vp_lock);
    RET(1);
}
void nvn_virtual_pool_report(FILE *out) {
    int n = 0; for (VPool *v = g_vpools; v; v = v->next) n++;
    if (n) fprintf(out, "nvn: virtual memory pools: %d, %llu ranges mapped, %llu unmapped\n", n, g_vp_maps, g_vp_unmaps);
}

/* For nvn_gl.c: where a memory pool's storage is. */
/* a virtual pool whose address was handed out cannot later become a direct alias (pool_map_virtual) */
static void vp_note_use(uint64_t pool) {
    AcquireSRWLockExclusive(&g_vp_lock);
    VPool *v = vp_find(pool);
    if (v && !v->direct) v->used = 1;
    ReleaseSRWLockExclusive(&g_vp_lock);
}
int nvn_pool_range(uint64_t pool, uint64_t *mem, uint64_t *size) {
    NvnObj *p = nobj(pool);
    vp_note_use(pool);
    *mem = p->memory; *size = p->size;
    return p->memory != 0;
}
int nvn_pool_flags(uint64_t pool) { return pool ? (int)nobj(pool)->flags : 0; }      /* for nvn_gl.c */
static void pool_map(Ctx *c) { log_call(c); vp_note_use(A0); RET(nobj(A0)->memory); }
static void pool_size(Ctx *c) { log_call(c); RET(nobj(A0)->size); }
static void buffer_builder_storage(Ctx *c) {                         /* (builder, pool, offset, size) */
    log_call(c);
    NvnObj *b = nobj(A0);
    b->pool = A1; b->offset = A2; b->size = c->x[3];
}
static void buffer_initialize(Ctx *c) {                              /* (buffer, builder) */
    log_call(c);
    NvnObj *buf = nobj(A0), *b = nobj(A1);
    vp_note_use(b->pool);
    buf->memory = nobj(b->pool)->memory + b->offset;
    buf->size = b->size; buf->pool = b->pool; buf->offset = b->offset;
    RET(1);
}
static void buffer_map(Ctx *c) { log_call(c); RET(nobj(A0)->memory); }
static void buffer_address(Ctx *c) { log_call(c); RET(nobj(A0)->memory); }   /* GPU address: the same value for now */
static void buffer_size(Ctx *c) { log_call(c); RET(nobj(A0)->size); }

ImportFn nvn_gl_pick(const char *name);     /* nvn_gl.c: the OpenGL backend */
static ImportFn pick(const char *name);
static void nvn_get_proc(Ctx *c) {                                   /* (device, const char *name) */
    const char *name = (const char *)(uintptr_t)A1;
    RET(hostfn_address(name, pick(name)));
}
static void nvn_bootstrap(Ctx *c) {                                  /* nvnBootstrapLoader(const char *name) */
    const char *name = (const char *)(uintptr_t)A0;
    RET(hostfn_address(name, pick(name)));
}

static ImportFn pick(const char *name) {
    size_t n = strlen(name);
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) return nvn_get_proc;
    if (strcmp(name, "nvnDeviceGetInteger") == 0) return nvn_get_integer;
    static const struct { const char *name; ImportFn fn; } special[] = {
        {"nvnMemoryPoolBuilderSetStorage", pool_builder_storage}, {"nvnMemoryPoolInitialize", pool_initialize},
        {"nvnMemoryPoolMapVirtual", pool_map_virtual},       /* virtual pools (see above) */
        {"nvnMemoryPoolMap", pool_map}, {"nvnMemoryPoolGetSize", pool_size},
        {"nvnBufferBuilderSetStorage", buffer_builder_storage}, {"nvnBufferInitialize", buffer_initialize},
        {"nvnBufferMap", buffer_map}, {"nvnBufferGetAddress", buffer_address}, {"nvnBufferGetSize", buffer_size},
        {"nvnMemoryPoolGetBufferAddress", pool_map},         /* GPU address of the pool: same as its memory for now */
        {"nvnMemoryPoolBuilderSetFlags", pool_builder_flags}, {"nvnMemoryPoolGetFlags", pool_flags},
        {"nvnMemoryPoolBuilderSetDefaults", builder_defaults}, {"nvnMemoryPoolBuilderSetDevice", builder_set_device}, {"nvnMemoryPoolBuilderGetDevice", builder_get_device},
        {"nvnMemoryPoolBuilderGetMemory", pool_builder_get_memory}, {"nvnMemoryPoolBuilderGetSize", pool_builder_get_size}, {"nvnMemoryPoolBuilderGetFlags", pool_builder_get_flags},
        {"nvnMemoryPoolFinalize", pool_finalize}, {"nvnMemoryPoolSetDebugLabel", obj_set_label}, {"nvnMemoryPoolFlushMappedRange", obj_flush_range},
        {"nvnBufferBuilderSetDefaults", builder_defaults}, {"nvnBufferBuilderSetDevice", builder_set_device}, {"nvnBufferBuilderGetDevice", builder_get_device},
        {"nvnBufferBuilderGetMemoryPool", buffer_builder_get_pool}, {"nvnBufferBuilderGetMemoryOffset", buffer_builder_get_offset}, {"nvnBufferBuilderGetSize", buffer_builder_get_size},
        {"nvnBufferGetMemoryPool", buffer_get_pool}, {"nvnBufferGetMemoryOffset", buffer_get_offset}, {"nvnBufferFinalize", buffer_finalize},
        {"nvnBufferSetDebugLabel", obj_set_label}, {"nvnBufferFlushMappedRange", obj_flush_range}, {"nvnBufferGetDebugID", obj_debug_id},
    };
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++)
        if (strcmp(name, special[i].name) == 0) return special[i].fn;
    ImportFn gl = nvn_gl_pick(name);
    if (gl) return gl;
    (void)n;
    printf("nvn: MISSING at fetch: the game obtained %s, which has no implementation (calling it stops the game)%c", name, 10);
    fflush(stdout);
    return nvn_generic;
}

static void keep(Ctx *c) { (void)c; }
static const ImportDef nvn_imports[] = {
    {"nvnBootstrapLoader", nvn_bootstrap},
    {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", keep},
    {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", keep},
    {"_ZN2nv18InitializeGraphicsEPvm", keep},
};

void register_nvn_imports(void) {
    register_imports(nvn_imports, (int)(sizeof nvn_imports / sizeof nvn_imports[0]));
}
