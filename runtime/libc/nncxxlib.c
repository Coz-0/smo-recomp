/* libc++ (std::__1) pieces beyond libcxx.c / nniostream.c: locale facets, stream vtables.
 * Layouts marked [sdk] were read from the real SDK module (Super Smash Bros. Ultimate 13.0.4's, by ssbu-recomp).
 *
 * Locale facets [sdk]: ctype<char> = { vptr, refs, const unsigned short *table, bool del }, classic table copied from
 * the SDK (mask bits: 0x1 blank, 0x2 cntrl, 0x4 punct, 0x100 upper, 0x200 lower, 0x400 alpha, 0x800 digit,
 * 0x1000 xdigit, 0x2000 space, 0x4000 print); numpunct<char> = { vptr, refs, char '.', char ',', string grouping "" }.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include "win32_posix.h"
#endif
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P64(p) ((uint64_t)(uintptr_t)(p))

/* nncxxrt.c */
uint64_t cxxrt_call(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2);
void cxxrt_guest_delete(Ctx *c, uint64_t p);
uint64_t cxxrt_allocate_exception(size_t size);
void cxxrt_throw_report(Ctx *c, const char *how, uint64_t obj, uint64_t type);
uint64_t cxxrt_refstring_new(const char *s, size_t n);
uint64_t cxxrt_class_ti_vptr(int kind);
/* libcxx.c */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n);

/* a short std::string (at most 22 characters) written in place: no allocation */
static void short_string(uint64_t s, const char *text) {
    size_t n = strlen(text);
    memset((void *)(uintptr_t)s, 0, 24);
    wr8(s, (uint8_t)(n << 1));
    memcpy((void *)(uintptr_t)(s + 1), text, n);
}

/* ================================================================ locale facets of the classic locale */
static const uint16_t classic_table[256] = {            /* [sdk] ctype<char>::classic_table(), entries 0..127; the rest are 0 */
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x2003, 0x2002, 0x2002, 0x2002, 0x2002, 0x002, 0x002,
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002,
    0x6001, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x4004, 0x5500, 0x5500, 0x5500, 0x5500, 0x5500, 0x5500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500,
    0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x4004, 0x5600, 0x5600, 0x5600, 0x5600, 0x5600, 0x5600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600,
    0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4004, 0x4004, 0x4004, 0x4004, 0x002,
};
static uint64_t g_id_ctype[2], g_id_numpunct[2];        /* the imported locale::id objects: only their addresses matter */
static uint64_t g_vt_ctype[13], g_ti_ctype[2], g_ctype[4];
static uint64_t g_vt_numpunct[10], g_ti_numpunct[2], g_numpunct[6];
static uint8_t g_classic_impl[64];

static int lower(int ch) { return ch >= 'a' && ch <= 'z'; }
static int upper(int ch) { return ch >= 'A' && ch <= 'Z'; }
static void facet_dtor(Ctx *c) { RET(A0); }             /* the facets are static: nothing to destroy or free */
static void facet_nop(Ctx *c) { (void)c; }
static void ct_toupper(Ctx *c) { int ch = (uint8_t)A1; RET(lower(ch) ? ch - 0x20 : ch); }
static void ct_toupper_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (lower(*p)) *p -= 0x20; RET(A2); }
static void ct_tolower(Ctx *c) { int ch = (uint8_t)A1; RET(upper(ch) ? ch + 0x20 : ch); }
static void ct_tolower_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (upper(*p)) *p += 0x20; RET(A2); }
static void ct_widen(Ctx *c) { RET((uint8_t)A1); }
static void ct_widen_range(Ctx *c) { memmove((void *)(uintptr_t)A3, (void *)(uintptr_t)A1, (size_t)(A2 - A1)); RET(A2); }     /* (low, high, dest) */
static void ct_narrow(Ctx *c) { RET((int8_t)A1 >= 0 ? (uint8_t)A1 : (uint8_t)A2); }                                          /* [sdk] non-ASCII -> default */
static void ct_narrow_range(Ctx *c) {                   /* (low, high, dfault, dest) */
    uint8_t *d = (uint8_t *)(uintptr_t)A4;
    for (const int8_t *p = (const int8_t *)(uintptr_t)A1; P64(p) < A2; p++) *d++ = *p >= 0 ? (uint8_t)*p : (uint8_t)A3;
    RET(A2);
}
static void np_decimal_point(Ctx *c) { RET(rd8(A0 + 0x10)); }
static void np_thousands_sep(Ctx *c) { RET(rd8(A0 + 0x11)); }
static void np_grouping(Ctx *c) { short_string(c->x[8], ""); }
static void np_truename(Ctx *c) { short_string(c->x[8], "true"); }
static void np_falsename(Ctx *c) { short_string(c->x[8], "false"); }
static void locale_use_facet(Ctx *c) {                  /* const facet *locale::use_facet(id &) const */
    uint64_t id = A1;
    if (id == P64(g_id_ctype)) { RET(g_ctype); return; }
    if (id == P64(g_id_numpunct)) { RET(g_numpunct); return; }
    printf("\nC++ RUNTIME: std::locale::use_facet for an id at 0x%llx that is neither ctype<char> nor numpunct<char> (std::bad_cast); guest caller 0x%llx\n",
           (unsigned long long)id, (unsigned long long)c->x[30] - 4);
    cxxrt_throw_report(c, "throw by std::locale::use_facet", 0, 0);
}

/* ================================================================ basic_ostream / basic_istream vtables
 * [sdk] _ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE: { vbase offset 8, 0, typeinfo, D1, D0,
 *   vcall offset -8, offset-to-top -8, typeinfo, virtual thunk D1, virtual thunk D0 } (istream: 0x10 / -0x10).
 *
 * Destructors, re-implemented from the SDK's code:
 *   [sdk 0x464d38] ios_base::~ios_base: vptr = ios_base vtable; for i = event_size-1 .. 0: fn[i](erase_event = 0, *this, index[i])
 *       (fn array +0x38, index array +0x40, event_size +0x48); ~locale() on +0x30; free(+0x38), free(+0x40), free(+0x58), free(+0x70)
 *       with the C `free` the SDK module imports from the game.
 *   [sdk 0x464dc0] basic_ios::~basic_ios (D2) = tail call to it.
 *   [sdk 0x46bd10] ostream D1: `add x0, x0, #8; b ios_base D1`; [sdk 0x46bd30] D0: the same, then operator delete(this).
 *       istream ([sdk] _ZNSt3__113basic_istream...D1Ev): its basic_ios part is at +0x10.
 *   [sdk 0x4656c0] basic_streambuf D2: vptr = basic_streambuf vtable; ~locale() on +8.
 *   [sdk 0x46be38] ostream::sentry::~sentry: if rdbuf && state == good && (flags & unitbuf) && !uncaught_exception():
 *       rdbuf->pubsync() == -1 -> setstate(badbit).
 * The vptr written is the SDK image's own vtable (mapped by sdklift_rt.c), as on the console. */
uint64_t sdk_symbol(const char *name);                    /* sdklift_rt.c */
static uint64_t g_vt_ostream[10], g_vt_istream[10], g_ti_ostream[2], g_ti_istream[2];
static void call_handler(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("nncxxlib: no handler for %s\n", name); guest_trap("missing C++ library handler", c->x[30] - 4); }
    f(c);
}
static void ios_base_destroy(Ctx *c, uint64_t ios) {
    uint64_t vt = sdk_symbol("_ZTVNSt3__18ios_baseE");
    if (vt) wr64(ios, vt + 0x10);
    for (uint64_t n = rd64(ios + 0x48); n-- > 0;) {
        uint64_t fn = rd64(rd64(ios + 0x38) + n * 8);
        c->x[2] = rd32(rd64(ios + 0x40) + n * 4);
        c->x[0] = 0; c->x[1] = ios;
        call_guest(c, fn);
    }
    c->x[0] = ios + 0x30; call_handler(c, "_ZNSt3__16localeD1Ev");
    static const int arrays[4] = {0x38, 0x40, 0x58, 0x70};
    for (int i = 0; i < 4; i++) { c->x[0] = rd64(ios + (uint64_t)arrays[i]); if (c->x[0]) call_guest(c, GAME_SYM_free); }   /* free(NULL) does nothing */
}
static void ios_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self); RET(self); }
static void sbuf_dtor(Ctx *c) {
    uint64_t self = A0, vt = sdk_symbol("_ZTVNSt3__115basic_streambufIcNS_11char_traitsIcEEEE");
    if (vt) wr64(self, vt + 0x10);
    c->x[0] = self + 8; call_handler(c, "_ZNSt3__16localeD1Ev");
    RET(self);
}
static void sentry_dtor(Ctx *c) {
    uint64_t s = A0, os = rd64(s + 8), ios = os + rd64(rd64(os) - 0x18);
    if (rd64(ios + 0x28) && rd32(ios + 0x20) == 0 && (rd8(ios + 9) & 0x20)) {
        int cxxrt_uncaught(Ctx *c);                                                 /* nncxxrt.c: std::uncaught_exception() */
        if (!cxxrt_uncaught(c)) {
            ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
            uint64_t sb = rd64(ios + 0x28);
            c->x[0] = sb;
            call_guest(c, rd64(rd64(sb) + 0x30));                                   /* pubsync -> virtual sync() */
            if ((uint32_t)c->x[0] == 0xffffffffu) {
                ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
                c->x[0] = ios; c->x[1] = rd32(ios + 0x20) | 1;
                call_handler(c, "_ZNSt3__18ios_base5clearEj");
            }
        }
    }
    RET(s);
}
/* complete-object destructors: `this` is the stream; its basic_ios part is at +8 (ostream) or +0x10 (istream) = vtable[-3] */
static void stream_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); RET(self); }
static void ostream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); RET(self); }
static void istream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); RET(self); }
static void ostream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); cxxrt_guest_delete(c, self); }
static void istream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); cxxrt_guest_delete(c, self); }
static void stream_dtor_delete(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); cxxrt_guest_delete(c, self); }
/* virtual thunks: `this` is the basic_ios part; this += vcall offset (vtable[-3] of the ios vptr) gives the stream */
static void stream_thunk_dtor(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); RET(self); }
static void stream_thunk_dtor_delete(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); cxxrt_guest_delete(c, self); }

static void make_stream_vtable(uint64_t *vt, uint64_t *ti, const char *cls, int64_t ios_offset) {
    char name[72];
    vt[0] = (uint64_t)ios_offset; vt[1] = 0; vt[2] = P64(ti);
    snprintf(name, sizeof name, "std::%s::~%s [D1]", cls, cls); vt[3] = hostfn_address(name, stream_dtor);
    snprintf(name, sizeof name, "std::%s::~%s [D0]", cls, cls); vt[4] = hostfn_address(name, stream_dtor_delete);
    vt[5] = (uint64_t)-ios_offset; vt[6] = (uint64_t)-ios_offset; vt[7] = P64(ti);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D1]", cls, cls); vt[8] = hostfn_address(name, stream_thunk_dtor);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D0]", cls, cls); vt[9] = hostfn_address(name, stream_thunk_dtor_delete);
}

void cxxlib_install_data(void) {
    uint64_t vt_class = cxxrt_class_ti_vptr(1);
    /* facets: [sdk] vtable orders of ctype<char> and numpunct<char> */
    static const struct { const char *name; ImportFn fn; } ct[] = {
        {"std::ctype<char>::~ [D1]", facet_dtor}, {"std::ctype<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::ctype<char>::do_toupper(char)", ct_toupper}, {"std::ctype<char>::do_toupper(range)", ct_toupper_range},
        {"std::ctype<char>::do_tolower(char)", ct_tolower}, {"std::ctype<char>::do_tolower(range)", ct_tolower_range},
        {"std::ctype<char>::do_widen(char)", ct_widen}, {"std::ctype<char>::do_widen(range)", ct_widen_range},
        {"std::ctype<char>::do_narrow(char)", ct_narrow}, {"std::ctype<char>::do_narrow(range)", ct_narrow_range},
    }, np[] = {
        {"std::numpunct<char>::~ [D1]", facet_dtor}, {"std::numpunct<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::numpunct<char>::do_decimal_point", np_decimal_point}, {"std::numpunct<char>::do_thousands_sep", np_thousands_sep},
        {"std::numpunct<char>::do_grouping", np_grouping}, {"std::numpunct<char>::do_truename", np_truename},
        {"std::numpunct<char>::do_falsename", np_falsename},
    };
    g_ti_ctype[0] = vt_class; g_ti_ctype[1] = P64("NSt3__15ctypeIcEE");
    g_vt_ctype[0] = 0; g_vt_ctype[1] = P64(g_ti_ctype);
    for (int i = 0; i < 11; i++) g_vt_ctype[2 + i] = hostfn_address(ct[i].name, ct[i].fn);
    g_ctype[0] = P64(&g_vt_ctype[2]); g_ctype[1] = 0x7fffffff; g_ctype[2] = P64(classic_table); g_ctype[3] = 0;
    g_ti_numpunct[0] = vt_class; g_ti_numpunct[1] = P64("NSt3__18numpunctIcEE");
    g_vt_numpunct[0] = 0; g_vt_numpunct[1] = P64(g_ti_numpunct);
    for (int i = 0; i < 8; i++) g_vt_numpunct[2 + i] = hostfn_address(np[i].name, np[i].fn);
    g_numpunct[0] = P64(&g_vt_numpunct[2]); g_numpunct[1] = 0x7fffffff; g_numpunct[2] = 0x2c2e;        /* [sdk] '.' at +0x10, ',' at +0x11 */
    import_set_address("_ZNSt3__15ctypeIcE2idE", g_id_ctype);
    import_set_address("_ZNSt3__18numpunctIcE2idE", g_id_numpunct);
    import_set_address("_ZTVNSt3__15ctypeIcEE", g_vt_ctype);
    import_set_address("_ZTVNSt3__18numpunctIcEE", g_vt_numpunct);

    /* streams */
    g_ti_ostream[0] = vt_class; g_ti_ostream[1] = P64("NSt3__113basic_ostreamIcNS_11char_traitsIcEEEE");
    g_ti_istream[0] = vt_class; g_ti_istream[1] = P64("NSt3__113basic_istreamIcNS_11char_traitsIcEEEE");
    make_stream_vtable(g_vt_ostream, g_ti_ostream, "basic_ostream", 8);
    make_stream_vtable(g_vt_istream, g_ti_istream, "basic_istream", 0x10);
    import_set_address("_ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", g_vt_ostream);
    import_set_address("_ZTVNSt3__113basic_istreamIcNS_11char_traitsIcEEEE", g_vt_istream);
    (void)g_classic_impl;
}

#define OSTR "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE"
#define ISTR "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE"
/* Replace earlier registrations: use_facet used to trap; the deleting destructor did not free the object. */
static const ImportDef cxxlib_replace[] = {
    {"_ZNKSt3__16locale9use_facetERNS0_2idE", locale_use_facet},
    {OSTR "D0Ev", ostream_d0}, {ISTR "D0Ev", istream_d0}, {OSTR "D1Ev", ostream_d1}, {ISTR "D1Ev", istream_d1},
    {"_ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev", ios_dtor},
    {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev", sbuf_dtor},
    {OSTR "6sentryD1Ev", sentry_dtor},
};
static const ImportDef cxxlib_fill[] = {
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
};

void register_cxxlib_imports(void) {
    register_imports(cxxlib_replace, (int)(sizeof cxxlib_replace / sizeof cxxlib_replace[0]));
    for (int i = 0; i < (int)(sizeof cxxlib_fill / sizeof cxxlib_fill[0]); i++)
        if (!import_lookup(cxxlib_fill[i].name)) register_imports(&cxxlib_fill[i], 1);
}
