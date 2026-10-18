/* Host implementations of the libc++ (std::__1) functions the game imports
 * from the system module. Objects live in guest memory, so the layouts below
 * must match libc++ on AArch64 exactly.
 *
 * std::string (24 bytes, little endian, default ABI):
 *   short: byte 0 = size << 1, characters from byte 1 (up to 22 + NUL)
 *   long : u64 capacity | 1 at +0, u64 size at +8, char* data at +16
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

typedef uint8_t Str;                                     /* pointer to a guest std::string */

/* String buffers must come from the game's own allocator: most std::string code is inlined in
 * the game and allocates/frees through f_710392dce0(align, size) / f_710392e590(ptr) directly,
 * so a buffer created here can be freed there and vice versa. The stored capacity is the
 * allocation size (libc++ keeps "size | 1"), rounded up to 16 as the inlined code does. */
#define GAME_MEMALIGN GAME_SYM_aligned_alloc   /* the game's own allocator, by symbol */
#define GAME_FREE GAME_SYM_free
static __declspec(thread) Ctx *t_ctx;                     /* context of the string op in progress */
static char *sbuf_alloc(size_t alloc) {
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = 16; c->x[1] = alloc;
    call_guest(c, GAME_MEMALIGN);
    char *p = (char *)(uintptr_t)c->x[0];
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
    if (!p) guest_trap("game allocator returned null for a string buffer", alloc);
    return p;
}
static void sbuf_free(char *p, const char *where) {
    (void)where;
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = (uint64_t)(uintptr_t)p;
    call_guest(c, GAME_FREE);
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
}

static int s_long(const Str *s) { return s[0] & 1; }
static char *s_data(Str *s) { return s_long(s) ? (char *)(uintptr_t)rd64((uint64_t)s + 16) : (char *)s + 1; }
static size_t s_size(const Str *s) { return s_long(s) ? (size_t)rd64((uint64_t)s + 8) : (size_t)(s[0] >> 1); }
static size_t s_cap(const Str *s) { return s_long(s) ? (size_t)(rd64((uint64_t)s) & ~1ull) - 1 : 22; }
static void s_set_size(Str *s, size_t n) { if (s_long(s)) wr64((uint64_t)s + 8, n); else s[0] = (uint8_t)(n << 1); }

static void s_reserve(Str *s, size_t need) {
    size_t cap = s_cap(s);
    if (need <= cap) return;
    size_t want = need > 2 * cap ? need : 2 * cap;
    size_t alloc = (want + 16) & ~(size_t)15;
    size_t size = s_size(s);
    char *p = sbuf_alloc(alloc);
    memcpy(p, s_data(s), size + 1);
    if (s_long(s)) sbuf_free(s_data(s), "reserve");
    wr64((uint64_t)s, alloc | 1);
    wr64((uint64_t)s + 8, size);
    wr64((uint64_t)s + 16, (uint64_t)p);
}
static void s_assign(Str *s, const char *p, size_t n) {
    char *copy = NULL;
    char *d = s_data(s);
    if (p >= d && p <= d + s_size(s)) { copy = malloc(n + 1); memcpy(copy, p, n); p = copy; }   /* self-assignment */
    s_reserve(s, n);
    d = s_data(s);
    memmove(d, p, n);
    d[n] = 0;
    s_set_size(s, n);
    free(copy);
}
static void s_append(Str *s, const char *p, size_t n) {
    char *copy = NULL;
    char *d = s_data(s);
    size_t size = s_size(s);
    if (p >= d && p <= d + size) { copy = malloc(n + 1); memcpy(copy, p, n); p = copy; }
    s_reserve(s, size + n);
    d = s_data(s);
    memcpy(d + size, p, n);
    d[size + n] = 0;
    s_set_size(s, size + n);
    free(copy);
}
static void s_init(Str *s, const char *p, size_t n) { memset(s, 0, 24); s_assign(s, p, n); }

/* For the other C++ runtime files (nncxxrt.c, nncxxlib.c): build / read guest std::string objects and
 * free buffers the game's inlined container code allocated. */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n) { Ctx *prev = t_ctx; t_ctx = c; s_init((Str *)(uintptr_t)str, p, n); t_ctx = prev; }
void cxx_game_free(Ctx *c, uint64_t p) { Ctx *prev = t_ctx; t_ctx = c; sbuf_free((char *)(uintptr_t)p, "runtime"); t_ctx = prev; }

static void str_assign_cstr(Ctx *c) { t_ctx = c; s_assign(P(0), P(1), strlen(P(1))); }
static void str_assign_n(Ctx *c) { t_ctx = c; s_assign(P(0), P(1), (size_t)A2); }
static void str_push_back(Ctx *c) { t_ctx = c; char ch = (char)A1; s_append(P(0), &ch, 1); }
static void str_copy_ctor(Ctx *c) { t_ctx = c; Str *o = P(1); s_init(P(0), s_data(o), s_size(o)); }
static void str_dtor(Ctx *c) { t_ctx = c; Str *s = P(0); if (s_long(s)) sbuf_free(s_data(s), "dtor"); }
static void str_resize(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t n = (size_t)A1, size = s_size(s);
    if (n > size) { s_reserve(s, n); memset(s_data(s) + size, (int)A2, n - size); }
    s_data(s)[n] = 0;
    s_set_size(s, n);
}

/* ---- hashing support */
static void next_prime(Ctx *c) {
    uint64_t n = A0;
    if (n <= 2) { RET(2); return; }
    n |= 1;
    for (;; n += 2) {
        int prime = 1;
        for (uint64_t d = 3; d * d <= n; d += 2) if (n % d == 0) { prime = 0; break; }
        if (prime) { RET(n); return; }
    }
}

static void throw_trap(Ctx *c) { guest_trap("libc++ threw length_error / out_of_range", c->x[30] - 4); }

/* ---- std::locale: there is only the classic "C" locale */
static void locale_dtor(Ctx *c) { (void)c; }
/* locale::use_facet is in nncxxlib.c (ctype<char> and numpunct<char> facets of the classic locale) */

#define STR "NSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
static const ImportDef cxx_imports[] = {
    {"_Z" STR "6assignEPKc", str_assign_cstr}, {"_Z" STR "6assignEPKcm", str_assign_n},
    {"_Z" STR "9push_backEc", str_push_back},
    {"_Z" STR "C1ERKS5_", str_copy_ctor},
    {"_Z" STR "D1Ev", str_dtor} /* SMO imports the complete-object form: the same for a string */,
    {"_Z" STR "6resizeEmc", str_resize},
    {"_ZNSt3__112__next_primeEm", next_prime},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", throw_trap},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNSt3__16localeD1Ev", locale_dtor},
};

void register_cxx_imports(void) {
    register_imports(cxx_imports, (int)(sizeof cxx_imports / sizeof cxx_imports[0]));
}
