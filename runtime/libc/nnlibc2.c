/* C library additions: conversions between binary128 (long double on AArch64) and double, the standard streams with
 * fclose / feof / ferror, and gmtime.
 *
 * Only names that have no handler yet are installed (see register_libc2_imports). */
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
#define RET(v) (c->x[0] = (uint64_t)(v))

/* ================================================================ binary128 <-> double
 * long double is IEEE binary128 on AArch64: clib.c reads it for printf's %Lf and writes it for sscanf's %Lf.
 * Narrowing rounds to nearest even. */
typedef unsigned __int128 u128;
#define Q_BIAS 16383
#define Q_FRAC_MASK ((((u128)1) << 112) - 1)
#define Q_IMPLICIT (((u128)1) << 112)
#define Q_INF ((u128)0x7fff << 112)
#define Q_SIGN ((u128)1 << 127)

static int q_is_nan(u128 a) { return (a & ~Q_SIGN) > Q_INF; }
static int q_is_inf(u128 a) { return (a & ~Q_SIGN) == Q_INF; }
static int q_is_zero(u128 a) { return (a & ~Q_SIGN) == 0; }
static int clz128(u128 v) { uint64_t hi = (uint64_t)(v >> 64); return hi ? __builtin_clzll(hi) : 64 + (v ? __builtin_clzll((uint64_t)v) : 64); }

/* Splits a finite non-zero value into exponent and a significand with the leading one at bit 112. */
static void q_unpack(u128 a, int *exp, u128 *sig) {
    int e = (int)((a >> 112) & 0x7fff);
    u128 m = a & Q_FRAC_MASK;
    if (e == 0) { int sh = clz128(m) - 15; m <<= sh; e = 1 - sh; }
    else m |= Q_IMPLICIT;
    *exp = e; *sig = m;
}

u128 q_from_f64(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int sign = (int)(bits >> 63), e = (int)((bits >> 52) & 0x7ff);
    uint64_t f = bits & 0xfffffffffffffull;
    if (e == 0x7ff) return ((u128)sign << 127) | Q_INF | (f ? ((u128)1 << 111) | ((u128)f << 60) : 0);
    if (e == 0) {
        if (!f) return (u128)sign << 127;
        int sh = __builtin_clzll(f) - 11;
        f <<= sh; e = 1 - sh; f &= 0xfffffffffffffull;
    }
    return ((u128)sign << 127) | ((u128)(e - 1023 + Q_BIAS) << 112) | ((u128)f << 60);
}
/* Narrowing: rounds the 113-bit significand to `bits` (53 for a double) with the target's exponent range. */
static uint64_t q_narrow(u128 a, int bits, int ebits) {
    int sign = (int)(a >> 127), emax = (1 << ebits) - 1, bias = emax >> 1;
    uint64_t s = (uint64_t)sign << (bits - 1 + ebits);
    if (q_is_nan(a)) return s | ((uint64_t)emax << (bits - 1)) | (1ull << (bits - 2)) | (uint64_t)((a & Q_FRAC_MASK) >> (113 - bits));
    if (q_is_inf(a)) return s | ((uint64_t)emax << (bits - 1));
    if (q_is_zero(a)) return s;
    int e;
    u128 m;
    q_unpack(a, &e, &m);
    e = e - Q_BIAS + bias;
    /* keep bits+3 bits: leading one at bit (bits+2), then guard/round/sticky */
    int shift = 113 - (bits + 3);
    u128 lost = m & (((u128)1 << shift) - 1);
    uint64_t sig = (uint64_t)(m >> shift) | (lost != 0);
    if (e <= 0) {
        int sh = 1 - e;
        if (sh > bits + 3) sig = sig != 0;
        else { uint64_t l = sig & ((1ull << sh) - 1); sig = (sig >> sh) | (l != 0); }
        e = 0;
    }
    unsigned grs = (unsigned)sig & 7;
    sig >>= 3;
    if (grs > 4 || (grs == 4 && (sig & 1))) sig++;
    if (e == 0) { if (sig >> (bits - 1)) e = 1; }
    else if (sig >> bits) { sig >>= 1; e++; }
    if (e >= emax) return s | ((uint64_t)emax << (bits - 1));
    return s | ((uint64_t)e << (bits - 1)) | (sig & ((1ull << (bits - 1)) - 1));
}
double q_to_f64(u128 a) { uint64_t b = q_narrow(a, 53, 11); double d; memcpy(&d, &b, 8); return d; }
void quad_from_double(double d, uint64_t out[2]) { u128 q = q_from_f64(d); out[0] = (uint64_t)q; out[1] = (uint64_t)(q >> 64); }
double quad_to_double(const uint64_t in[2]) { return q_to_f64(((u128)in[1] << 64) | in[0]); }

/* ================================================================ the standard streams
 * The game imports the variables stdin, stdout and stderr and the functions fclose, feof and ferror, but nothing that
 * opens a file: a guest FILE * is one of the three host objects below (the game only passes it back). */
typedef struct {
    uint32_t magic;                  /* 'FILE' */
    int kind;                        /* 1 stdin, 2 stdout, 3 stderr */
    int eof, err, open;
} GFile;
#define GFILE_MAGIC 0x454c4946u
static GFile g_std[3] = {{GFILE_MAGIC, 1, 0, 0, 1}, {GFILE_MAGIC, 2, 0, 0, 1}, {GFILE_MAGIC, 3, 0, 0, 1}};
static uint64_t g_std_ptr[3];        /* the objects behind the imported `stdin`, `stdout`, `stderr` (FILE * variables) */

static GFile *gfile(uint64_t p) {
    GFile *f = (GFile *)(uintptr_t)p;
    return f && f->magic == GFILE_MAGIC ? f : NULL;
}
static void c_fclose(Ctx *c) {
    GFile *f = gfile(A0);
    if (!f) { RET(-1); return; }
    f->open = 0;
    RET(0);
}
static void c_feof(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->eof : 0); }
static void c_ferror(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->err : 0); }

/* ================================================================ gmtime
 * musl struct tm: nine ints (sec, min, hour, mday, mon, year, wday, yday, isdst), long gmtoff at +40, const char *zone at +48. */
int libc2_gmtime(int64_t t, int32_t tm[9]) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    if (days < -1000000000ll || days > 1000000000ll) return 0;        /* year would overflow an int */
    tm[2] = (int32_t)(rem / 3600); tm[1] = (int32_t)(rem % 3600 / 60); tm[0] = (int32_t)(rem % 60);
    int64_t wd = (days + 4) % 7;                                      /* 1970-01-01 was a Thursday */
    tm[6] = (int32_t)(wd < 0 ? wd + 7 : wd);
    /* civil date from day count (proleptic Gregorian), era-based */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    tm[3] = (int32_t)d; tm[4] = (int32_t)(m - 1); tm[5] = (int32_t)(y - 1900);
    tm[7] = cum[m - 1] + (int32_t)d - 1 + (leap && m > 2);
    tm[8] = 0;
    return 1;
}
static void tm_store(uint64_t dst, const int32_t tm[9]) {
    static const char utc[] = "UTC";
    memcpy((void *)(uintptr_t)dst, tm, 36);
    wr32(dst + 36, 0);
    wr64(dst + 40, 0);
    wr64(dst + 48, (uint64_t)(uintptr_t)utc);
}
static void c_gmtime(Ctx *c) {
    static __declspec(thread) uint8_t buf[56];
    int32_t tm[9];
    if (!libc2_gmtime((int64_t)rd64(A0), tm)) { RET(0); return; }
    tm_store((uint64_t)(uintptr_t)buf, tm);
    RET(buf);
}

static const ImportDef libc2_imports[] = {
    {"fclose", c_fclose},
    {"feof", c_feof}, {"ferror", c_ferror},
    {"gmtime", c_gmtime},
};

/* Called before runtime_init (from cxxrt_install_data): the imported FILE * variables. */
void libc2_install_data(void) {
    for (int i = 0; i < 3; i++) g_std_ptr[i] = (uint64_t)(uintptr_t)&g_std[i];
    import_set_address("stdin", &g_std_ptr[0]);
    import_set_address("stdout", &g_std_ptr[1]);
    import_set_address("stderr", &g_std_ptr[2]);
}

void register_libc2_imports(void) {
    int n = (int)(sizeof libc2_imports / sizeof libc2_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_lookup(libc2_imports[i].name)) register_imports(&libc2_imports[i], 1);
}
