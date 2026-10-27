/* The cases of tests/check_yaz0.py: the runtime's Yaz0 decoders (runtime/game/smo_yaz0.c, copied into
 * build/tmp/yaz0_extract.h) against the game's, lifted (build/tmp/yaz0_lifted.c), on every file of a list.
 *
 *     yaz0_check <list of .szs files> [seed]
 *
 * Per file: the whole-buffer decoder of each, the same output and answer; then streamDecomp of each over the same random
 * chunks (1 byte to 128 KB, a third of them under 64 bytes; the last one with up to 64 bytes past the file), now and then with a forceDestCount below the size: the
 * same answer and context after every chunk, and the same output. The outputs start after 4 KB of the same bytes (a
 * bad back-reference reads them alike). Then the time of each over all the files: whole, and in 64 KB chunks. Exit
 * status 0 when nothing differs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "runtime.h"
#include "yaz0_extract.h"
#include "yaz0_pages.h"                /* YAZ0_PAGES: the game's read-only pages the lifted code reads (a jump table) */

void f_yaz0_stream(Ctx *c);
void f_yaz0_whole(Ctx *c);

void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s at %#llx\n", what, (unsigned long long)addr); exit(2); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { fprintf(stderr, "guest_unimpl %#llx %08x %s\n", (unsigned long long)pc, insn, what); exit(2); }
void call_indirect(Ctx *c, uint64_t a) { (void)c; guest_trap("call_indirect", a); }
void host_import(Ctx *c, int sym) { (void)c; guest_trap("host_import", (uint64_t)sym); }
void guest_svc(Ctx *c, uint32_t n) { (void)c; guest_trap("svc", n); }
void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int b) { (void)c; (void)e; (void)s; (void)v; (void)b; guest_trap("stx", a); }

static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static double now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec + t.tv_nsec * 1e-9; }

static void map_pages(void) {
    for (size_t i = 0; i < sizeof yaz0_pages / sizeof yaz0_pages[0]; i++) {
        void *want = (void *)(uintptr_t)yaz0_pages[i].addr, *m;
#ifdef _WIN32
        m = VirtualAlloc(want, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        m = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m == MAP_FAILED) m = NULL;
#endif
        if (m != want) { fprintf(stderr, "cannot map the game's page at %#llx\n", (unsigned long long)yaz0_pages[i].addr); exit(2); }
        memcpy(m, yaz0_pages[i].data, 4096);
    }
}

static uint8_t *read_file(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len + 64);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f);
    memset(d + len, 0, 64);
    *n = (uint32_t)len;
    return d;
}

#define PRE 4096
static uint8_t *out_buf(uint32_t size) {           /* PRE bytes of the same pattern, then the output (and slack) */
    uint8_t *b = malloc(PRE + (size_t)size + 0x200);
    for (int k = 0; k < PRE; k++) b[k] = (uint8_t)(k * 7);
    memset(b + PRE, 0xcd, (size_t)size + 0x200);
    return b;
}

static int32_t lifted_stream(Yaz0Ctx *x, const uint8_t *src, uint32_t len) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uintptr_t)x; c.x[1] = (uintptr_t)src; c.x[2] = len;
    f_yaz0_stream(&c);
    return (int32_t)c.x[0];
}

static uint32_t lifted_whole(uint8_t *dst, const uint8_t *src) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = (uintptr_t)dst; c.x[1] = (uintptr_t)src;
    f_yaz0_whole(&c);
    return (uint32_t)c.x[0];
}

static void ctx_init(Yaz0Ctx *x, uint8_t *dst, int32_t force) {
    memset(x, 0, sizeof *x);
    x->dst = dst; x->force = force; x->header = 16;
}

static long long g_files, g_bad, g_chunks;

static void check_file(const char *path, const uint8_t *src, uint32_t n) {
    uint32_t size = (uint32_t)src[4] << 24 | (uint32_t)src[5] << 16 | (uint32_t)src[6] << 8 | src[7];
    uint8_t *a = out_buf(size), *b = out_buf(size);
    uint32_t ra = yaz0_whole(a + PRE, src), rb = lifted_whole(b + PRE, src);
    if (ra != rb || memcmp(a, b, PRE + (size_t)size + 0x200)) { printf("DIFFERENT whole: %s\n", path); g_bad++; }
    for (int pass = 0; pass < 2; pass++) {
        memset(a + PRE, 0xcd, (size_t)size + 0x200); memset(b + PRE, 0xcd, (size_t)size + 0x200);
        int32_t force = pass && size > 2 ? (int32_t)(rnd() % size) : 0;
        Yaz0Ctx xa, xb;
        ctx_init(&xa, a + PRE, force); ctx_init(&xb, b + PRE, force);
        uint32_t at = 0;
        while (at < n) {
            uint32_t len = rnd() % 3 == 0 ? 1 + (uint32_t)(rnd() % 64) : 1 + (uint32_t)(rnd() % 0x20000);
            if (len > n - at) len = n - at;
            uint32_t extra = at + len == n ? (uint32_t)(rnd() % 65) : 0;   /* the last chunk: up to 64 bytes past the file */
            len += extra;
            int32_t qa = yaz0_stream(&xa, src + at, len), qb = lifted_stream(&xb, src + at, len);
            g_chunks++;
            if (qa != qb || xa.dst - a != xb.dst - b || memcmp((uint8_t *)&xa + 8, (uint8_t *)&xb + 8, 0x1b - 8)) {
                printf("DIFFERENT stream: %s at %u (+%u, force %d): answers %d / %d\n", path, at, len, force, qa, qb);
                g_bad++;
                break;
            }
            at += len - extra;
            if (qa <= 0) break;
        }
        if (memcmp(a, b, PRE + (size_t)size + 0x200)) { printf("DIFFERENT stream output: %s (force %d)\n", path, force); g_bad++; }
    }
    free(a); free(b);
    g_files++;
}

/* time of each decoder over the files: whole, and in 64 KB chunks */
static void timing(char **paths, int count) {
    double t[4] = {0}, in = 0, out = 0;
    for (int i = 0; i < count; i++) {
        uint32_t n;
        uint8_t *src = read_file(paths[i], &n);
        if (!src) continue;
        uint32_t size = (uint32_t)src[4] << 24 | (uint32_t)src[5] << 16 | (uint32_t)src[6] << 8 | src[7];
        uint8_t *d = out_buf(size);
        for (int k = 0; k < 4; k++) {
            double t0 = now();
            if (k == 0) yaz0_whole(d + PRE, src);
            else if (k == 1) lifted_whole(d + PRE, src);
            else {
                Yaz0Ctx x;
                ctx_init(&x, d + PRE, 0);
                for (uint32_t at = 0; at < n; at += 0x10000) {
                    uint32_t len = n - at < 0x10000 ? n - at : 0x10000;
                    if ((k == 2 ? yaz0_stream(&x, src + at, len) : lifted_stream(&x, src + at, len)) <= 0) break;
                }
            }
            t[k] += now() - t0;
        }
        in += n; out += size;
        free(d); free(src);
    }
    printf("timing over %d files (%.1f MB compressed, %.1f MB out), MB out per second:\n", count, in / 1e6, out / 1e6);
    printf("  whole:  host %.0f (%.2f s)   lifted %.0f (%.2f s)   %.1fx\n", out / 1e6 / t[0], t[0], out / 1e6 / t[1], t[1], t[1] / t[0]);
    printf("  stream: host %.0f (%.2f s)   lifted %.0f (%.2f s)   %.1fx\n", out / 1e6 / t[2], t[2], out / 1e6 / t[3], t[3], t[3] / t[2]);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: yaz0_check <list of .szs files> [seed]\n"); return 2; }
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 0) * 0x9e3779b97f4a7c15ull;
    map_pages();
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    static char line[4096];
    char **paths = NULL;
    int count = 0;
    while (fgets(line, sizeof line, lf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        paths = realloc(paths, sizeof *paths * (count + 1));
        paths[count++] = strdup(line);
    }
    fclose(lf);
    for (int i = 0; i < count; i++) {
        uint32_t n;
        uint8_t *src = read_file(paths[i], &n);
        if (!src || n < 16 || memcmp(src, "Yaz0", 4)) { free(src); continue; }
        check_file(paths[i], src, n);
        free(src);
    }
    printf("%lld files, %lld chunks: %lld different\n", g_files, g_chunks, g_bad);
    timing(paths, count);
    return g_bad != 0;
}
