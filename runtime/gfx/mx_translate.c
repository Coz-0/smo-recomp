/* Maxwell shader machine code -> GLSL. See mx_translate.h for the conventions shared with nvn_gl.c.
 *
 * How it works
 *   1. Control flow analysis from the entry: every reachable instruction slot is marked; the SSY/PBK/PCNT
 *      token stack is followed statically (as the hardware's per-warp stack, and as suyu's control_flow.cpp),
 *      so SYNC / BRK / CONT become plain jumps to known targets. BRX jump tables are found as suyu does
 *      (LDC from a constant buffer, bounded by SHL + IMNMX) and read from constant buffer 1.
 *   2. Only jump targets start a new "case" of a dispatch switch inside a loop; straight-line code between
 *      them is emitted as ordinary statements (falling through from case to case). A shader without jumps
 *      becomes straight code with no loop at all.
 *   3. Registers are local uints r0..r254 (RZ reads 0u); predicates are bools p0..p6 (PT = true); the
 *      condition code is four global bools (zero, sign, carry, overflow). Float operations bit-cast.
 * Instruction encodings come from suyu's opcode table (mx_ops.h) and the
 * semantics from its translate/impl files. Anything not handled makes the whole shader fail with the instruction's name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mx_translate.h"
#include "mx_ops.h"

#define MXB(x, lo, n) ((unsigned)(((x) >> (lo)) & ((1ull << (n)) - 1)))
#define MAXSTK 16

typedef struct { uint8_t tok[MAXSTK]; uint32_t tgt[MAXSTK]; int n; } Stk;     /* tokens: 1 SSY, 2 PBK, 3 PCNT, 4 PEXIT */
typedef struct {
    const MxIn *in;
    const uint8_t *code;
    unsigned words;
    char *s; size_t len, cap;                 /* body being emitted */
    int fail; char why[200], failop[40];
    uint32_t cbufs, in_attr, out_attr, tex_units, clip_out, ssbos;
    uint8_t regs[256], preds[8], in_comp[32], in_mode[32];
    int uses_gmem, uses_gstore, uses_cc, uses_layer_out, uses_vp_out, uses_psize, uses_lmem, insns, blocks, any_jump, uses_alpha;
    uint8_t *reach, *leader;
    int structured;                           /* jumps emitted as guarded blocks + real loops (plan_structured) instead of the pc switch */
    unsigned *loop_end;                       /* structured: loop_end[T] = last word of the loop whose head is T (0: no loop starts at T) */
    const uint8_t *base;                      /* instruction word 0 (a scheduling word) */
    uint32_t gubos;                           /* compute: uniform buffers reached through global memory */
    unsigned smem_words;
    int uses_smem;
    int uses_fcs, uses_tf, uses_shfl;                    /* MxIn.screen_scale: gl_FragCoord.xy was scaled / a texelFetch was mapped */
    uint32_t images, images_read, images_written;
    uint16_t image_fmt[8]; uint8_t image_type[8];
    uint32_t *jt;                             /* static jump target of SYNC / BRK / CONT */
    Stk *stks; int nstk, capstk;
    uint16_t *stk_of;
    char ring[24][1200]; int ri;
    char exitcode[4096];
    unsigned lmem_words, ssbo_word0;
    struct { uint8_t unit, shadow; uint16_t binding; char type[24]; } smp[64]; int nsmp;      /* target 1: named samplers used (SMP) */
    int dyn_unit_ok;                          /* tex_unit() may return a unit of a handle kept in a uniform buffer (op_tex_b) */
} Mx;

const char *mx_opcode_name(uint64_t insn) {
    for (size_t k = 0; k < sizeof mx_op_table / sizeof mx_op_table[0]; k++)
        if ((insn & mx_op_table[k].mask) == mx_op_table[k].value) return mx_op_name[mx_op_table[k].op];
    return "INVALID";
}
static int decode(uint64_t insn) {
    for (size_t k = 0; k < sizeof mx_op_table / sizeof mx_op_table[0]; k++)
        if ((insn & mx_op_table[k].mask) == mx_op_table[k].value) return mx_op_table[k].op;
    return MXOP_INVALID;
}

/* ---------------------------------------------------------------- text */
static void emit(Mx *e, const char *fmt, ...) {
    va_list ap;
    if (e->len + 4096 > e->cap) { e->cap = e->cap * 2 + 65536; e->s = realloc(e->s, e->cap); }
    va_start(ap, fmt);
    e->len += (size_t)vsnprintf(e->s + e->len, e->cap - e->len, fmt, ap);
    va_end(ap);
}
static const char *S(Mx *e, const char *fmt, ...) {      /* formatted temporary (ring of buffers) */
    char *b = e->ring[e->ri++ % 24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof e->ring[0], fmt, ap);
    va_end(ap);
    return b;
}
static void fail(Mx *e, const char *op, const char *fmt, ...) {
    va_list ap;
    if (e->fail) return;
    e->fail = 1;
    snprintf(e->failop, sizeof e->failop, "%s", op);
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof e->why, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- operands */
static const char *X(Mx *e, unsigned r) { if (r == 255) return "0u"; e->regs[r] = 1; return S(e, "r%u", r); }
static const char *D(Mx *e, unsigned r) { if (r == 255) return "rz"; e->regs[r] = 1; return S(e, "r%u", r); }
static const char *CB(Mx *e, uint64_t i) {                /* c[bits 34..38][bits 20..33 words] */
    unsigned b = MXB(i, 34, 5), w = MXB(i, 20, 14);
    if (b == 0 && w >= e->ssbo_word0 && w < e->ssbo_word0 + 64) {     /* NVN storage buffer descriptor {address, size}: a tagged fake address */
        unsigned k = (w - e->ssbo_word0) / 4, f = (w - e->ssbo_word0) & 3;
        if (f == 0) { e->ssbos |= 1u << k; return S(e, "0x%08xu", (k + 1) << 27); }
        return f == 2 ? "0x07fffffcu" : "0u";
    }
    if (b == 0 && e->in->stage == MX_COMPUTE && w >= 0x210 / 4 && w < 0x210 / 4 + 60) {
        /* compute: the descriptor {address, size} of uniform buffer n, at c0 byte 0x210 + 16n. The compute engine has 8 constant
         * buffer slots only, so the game's compiler reaches the uniform buffers that do not fit (seen: 6, 7, 8) through global
         * memory (LDG). Tag 17 + n; mx_ldg reads the same block the constant buffer path uses (c3 + n). */
        unsigned n = (w - 0x210 / 4) / 4, f = (w - 0x210 / 4) & 3;
        if (f == 0) { e->gubos |= 1u << n; e->cbufs |= 1u << (3 + n); return S(e, "0x%08xu", (17 + n) << 27); }
        return f == 2 ? "0x07fffffcu" : "0u";
    }
    e->cbufs |= 1u << b;
    return S(e, "c%u[%uu]", b, w);
}
static const char *I20(Mx *e, uint64_t i) { uint32_t v = MXB(i, 20, 19); if (MXB(i, 56, 1)) v |= 0xfff80000u; return S(e, "0x%08xu", v); }
static const char *F20(Mx *e, uint64_t i) { return S(e, "0x%08xu", (MXB(i, 20, 19) << 12) | (MXB(i, 56, 1) << 31)); }
static const char *I32(Mx *e, uint64_t i) { return S(e, "0x%08xu", MXB(i, 20, 32)); }
#define A8 X(e, MXB(i, 8, 8))
#define R20 X(e, MXB(i, 20, 8))
#define R39 X(e, MXB(i, 39, 8))
#define DST D(e, MXB(i, 0, 8))
static const char *FN(Mx *e, const char *bits, int absv, int neg) { return S(e, "%s%s(F(%s))", neg ? "-" : "", absv ? "abs" : "", bits); }
static const char *PR(Mx *e, unsigned p, int neg) { if (p == 7) return neg ? "false" : "true"; e->preds[p] = 1; return S(e, "%sp%u", neg ? "!" : "", p); }
static const char *PD(Mx *e, unsigned p) { if (p == 7) return NULL; e->preds[p] = 1; return S(e, "p%u", p); }
static const char *BOP(Mx *e, const char *a, const char *b, unsigned op) {
    if (op == 0) return S(e, "(%s && %s)", a, b);
    if (op == 1) return S(e, "(%s || %s)", a, b);
    if (op == 2) return S(e, "(%s != %s)", a, b);
    fail(e, "BOP", "invalid boolean operation 3");
    return "false";
}
static const char *ICMP(Mx *e, unsigned op, int sgn, const char *a, const char *b) {
    static const char *const o[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0 || op == 7) return o[op];
    if (sgn && op != 2 && op != 5) return S(e, "(int(%s) %s int(%s))", a, o[op], b);
    return S(e, "(%s %s %s)", a, o[op], b);
}
/* operand of a video instruction (suyu ExtractVideoOperandValue): width 0 or 1 = byte `sel` of the register, 2 = half-word
 * `sel`, 3 = the whole word; zero- or sign-extended */
static const char *VOP(Mx *e, const char *r, unsigned sel, unsigned width, int sgn) {
    unsigned bits = width == 2 ? 16 : 8;
    if (width == 3) return r;
    if (sgn) return S(e, "uint(bitfieldExtract(int(%s), %u, %u))", r, sel * bits, bits);
    return S(e, "bitfieldExtract(%s, %u, %u)", r, sel * bits, bits);
}
/* extended (.X) integer compare: uses the carry and zero flags of the previous (lower) compare, as suyu */
static const char *ICMPX(Mx *e, unsigned op, int sgn, const char *a, const char *b) {
    e->uses_cc = 1;
    const char *im = S(e, "(%s + ~%s + uint(ccf))", a, b), *flip = sgn ? "false" : S(e, "((int(%s) < 0) != (int(%s) < 0))", a, b);
    const char *lt = S(e, "(%s ? int(%s) >= 0 : int(%s) < 0)", flip, im, im), *gt = S(e, "(%s ? int(%s) <= 0 : int(%s) > 0)", flip, im, im);
    const char *ge = S(e, "(%s ? int(%s) < 0 : int(%s) >= 0)", flip, im, im);
    switch (op) {
    case 0: return "false";
    case 1: return lt;
    case 2: return S(e, "(%s == 0u && cz)", im);
    case 3: return S(e, "(%s || (%s == 0u && cz))", lt, im);
    case 4: return S(e, "(%s || (%s == 0u && !cz))", gt, im);
    case 5: return S(e, "(%s != 0u || !cz)", im);
    case 6: return S(e, "(%s || (%s == 0u && cz))", ge, im);
    }
    return "true";
}
static const char *FCMP(Mx *e, unsigned op, const char *a, const char *b) {
    static const char *const o[8] = {"", "<", "==", "<=", ">", "!=", ">=", ""};
    unsigned base = op & 7;
    if (op == 0) return "false";
    if (op == 15) return "true";
    if (op == 7) return S(e, "(!isnan(%s) && !isnan(%s))", a, b);
    if (op == 8) return S(e, "(isnan(%s) || isnan(%s))", a, b);
    if (op < 8) {
        if (op == 5) return S(e, "(%s < %s || %s > %s)", a, b, a, b);       /* ordered not-equal */
        return S(e, "(%s %s %s)", a, o[base], b);
    }
    if (op == 13) return S(e, "(%s != %s)", a, b);                           /* GLSL != is already unordered */
    return S(e, "((%s %s %s) || isnan(%s) || isnan(%s))", a, o[base], b, a, b);
}
static const char *FLOW(Mx *e, unsigned t) {
    static const char *const f[32] = {
        "false", "((cs && !cz) != cov)", "(!cs && cz)", "(cs != (cz || cov))", "((!cs != cov) && !cz)", "(!cz)", "(cs == cov)", "(!cs || !cz)",
        "(cs && cz)", "(cs != cov)", "cz", "((cs != cov) || cz)", "(!cs != (cz || cov))", "(cs || !cz)", "((!cs || cz) != cov)", "true",
        "(!cov)", "(!ccf)", "(!cs)", "(cz || !ccf)", "(ccf && !cz)", "cs", "ccf", "cov",
        NULL, NULL, NULL, NULL, "false", NULL, "(cs || cz)", "(!cs && !cz)"};
    if (t >= 32 || !f[t]) { fail(e, "flow test", "condition code test %u", t); return "false"; }
    if (t != 0 && t != 15 && t != 28) e->uses_cc = 1;
    return f[t];
}
static void setcc_int(Mx *e, const char *res) { e->uses_cc = 1; emit(e, " cz = %s == 0u; cs = int(%s) < 0; ccf = false; cov = false;", res, res); }

/* ---------------------------------------------------------------- results */
/* float result with optional saturation and condition code */
static void fres(Mx *e, uint64_t i, const char *expr, int sat, int cc) {
    emit(e, "float f = %s;", expr);
    if (sat) emit(e, " f = clamp(f, 0.0, 1.0);");
    emit(e, " %s = U(f);", DST);
    if (cc) { e->uses_cc = 1; emit(e, " cz = f == 0.0 || isnan(f); cs = f < 0.0 || isnan(f); ccf = false; cov = false;"); }
}
static const char *fmz_mul(Mx *e, const char *a, const char *b, int fmz) {
    if (!fmz) return S(e, "%s * %s", a, b);
    return S(e, "((%s == 0.0 || %s == 0.0) ? 0.0 : %s * %s)", a, b, a, b);
}

/* ---------------------------------------------------------------- attributes */
static const char *attr_in(Mx *e, unsigned a, const char *vtx) {      /* vtx: geometry stage, the input vertex (an expression) */
    unsigned c = (a >> 2) & 3, st = (unsigned)e->in->stage;
    if (st == MX_GEOMETRY) {                                    /* the inputs of one vertex of the input primitive */
        /* the code reads past the primitive's vertices when it has fewer than it handles (it then sets the index to 0x80000000):
         * clamped to the last vertex, as an index out of range is undefined in GLSL */
        static const unsigned nv[5] = {1, 2, 3, 4, 6};
        vtx = S(e, "min(%s, %uu)", vtx, nv[(unsigned)e->in->gs_in_prim % 5] - 1);
        if (a >= 0x80 && a < 0x280) { unsigned n = (a - 0x80) >> 4; e->in_attr |= 1u << n; e->in_comp[n] |= (uint8_t)(1u << c); return S(e, "U(in_attr%u[%s][%u])", n, vtx, c); }
        if (a >= 0x70 && a < 0x80) return S(e, "U(gl_in[%s].gl_Position[%u])", vtx, c);
        if (a == 0x6c) return S(e, "U(gl_in[%s].gl_PointSize)", vtx);
        if (a == 0x60) return "uint(gl_PrimitiveIDIn)";
        fail(e, "attribute read", "read of attribute 0x%x in a geometry stage", a);
        return "0u";
    }
    if (a >= 0x80 && a < 0x280) {
        unsigned n = (a - 0x80) >> 4;
        e->in_attr |= 1u << n;
        e->in_comp[n] |= (uint8_t)(1u << c);
        if (st == MX_VERTEX && (e->in->int_attrs >> n & 1)) return S(e, "uint(in_attr%u[%u])", n, c);
        return S(e, "U(in_attr%u[%u])", n, c);
    }
    if (st == MX_FRAGMENT) {
        if (a >= 0x70 && a < 0x80 && c < 2 && e->in->screen_scale && e->in->target == 1) { e->uses_fcs = 1; return S(e, "U(gl_FragCoord[%u] * mx_fcs)", c); }      /* the game's pixel position */
        if (a >= 0x70 && a < 0x80) return S(e, "U(gl_FragCoord[%u])", c);
        if (a == 0x3fc) return "(gl_FrontFacing ? 0xffffffffu : 0u)";
        if ((a == 0x2e0 || a == 0x2e4) && e->in->target == 1 && c == 1) return "U(1.0 - gl_PointCoord[1])";      /* Vulkan: t runs the other way (flipped viewport) */
        if (a == 0x2e0 || a == 0x2e4) return S(e, "U(gl_PointCoord[%u])", c);
        if (a == 0x60) return "uint(gl_PrimitiveID)";
        if (a == 0x64) return "uint(gl_Layer)";
        if (a == 0x68) return "uint(gl_ViewportIndex)";
        if (a >= 0x2c0 && a < 0x2e0) return S(e, "U(gl_ClipDistance[%u])", (a - 0x2c0) >> 2);
    }
    if (st == MX_VERTEX) {
        if (a == 0x2f8) return e->in->target == 1 ? "uint(gl_InstanceIndex - gl_BaseInstance)" : "uint(gl_InstanceID)";
        if (a == 0x2fc) return e->in->target == 1 ? "uint(gl_VertexIndex)" : "uint(gl_VertexID)";
        if (a == 0x400) return "uint(gl_BaseInstance)";
        if (a == 0x404) return "uint(gl_BaseVertex)";
        if (a == 0x408) return "uint(gl_DrawID)";
    }
    fail(e, "attribute read", "read of attribute 0x%x in stage %u", a, st);
    return "0u";
}
static void attr_out(Mx *e, unsigned a, const char *v) {
    unsigned c = (a >> 2) & 3;
    if (e->in->stage != MX_VERTEX && e->in->stage != MX_GEOMETRY) { fail(e, "attribute store", "attribute store in stage %d", e->in->stage); return; }
    if (a >= 0x70 && a < 0x80) emit(e, " gl_Position[%u] = F(%s);", c, v);
    else if (a >= 0x80 && a < 0x280) { e->out_attr |= 1u << ((a - 0x80) >> 4); emit(e, " out_attr%u[%u] = F(%s);", (a - 0x80) >> 4, c, v); }
    else if (a == 0x6c && e->in->stage == MX_GEOMETRY) emit(e, " gl_PointSize = F(%s);", v);      /* a geometry stage writes its own (no push constant) */
    else if (a == 0x6c) { e->uses_psize = 1; emit(e, e->in->target == 1 ? " mx_psize = F(%s);" : " gl_PointSize = F(%s);", v); }
    else if (a >= 0x2c0 && a < 0x2e0) { e->clip_out |= 1u << ((a - 0x2c0) >> 2); emit(e, " gl_ClipDistance[%u] = F(%s);", (a - 0x2c0) >> 2, v); }
    else if (a == 0x64) { e->uses_layer_out = 1; emit(e, " gl_Layer = int(%s);", v); }
    else if (a == 0x68) { e->uses_vp_out = 1; emit(e, " gl_ViewportIndex = int(%s);", v); }
    else if ((a >= 0x280 && a < 0x2c0) || (a >= 0x300 && a < 0x3a0) || a == 0x2e8) emit(e, " /* legacy output 0x%x ignored */", a);
    else fail(e, "attribute store", "store to attribute 0x%x", a);
}

/* ---------------------------------------------------------------- textures */
/* Units 0..31 are the entries of the texture handle table c[2] (bound textures). Units 32..255 stand for a handle the game
 * keeps in one of its own uniform buffers (bindless_unit): 32 + 8 * buffer + the handle's 64-bit word in that buffer; the
 * back end reads the handle there at every draw. Per-unit masks of MxIn (int_tex, sint_tex) describe units 0..31 only. */
static int unit_bit(uint32_t mask, int unit) { return unit >= 0 && unit < 32 && (mask >> unit & 1); }
static int tex_unit(Mx *e, uint64_t i) {
    unsigned off = MXB(i, 36, 13);
    if (off < 8 || (off & 1) || ((off - 8) / 2 >= 32 && !(e->dyn_unit_ok && (off - 8) / 2 < 256))) { fail(e, "texture handle", "texture handle at constant buffer word %u", off); return -1; }
    if ((off - 8) / 2 >= 32) return (int)(off - 8) / 2;
    e->cbufs |= 4;
    e->tex_units |= 1u << ((off - 8) / 2);
    return (int)(off - 8) / 2;
}
/* sampler of the given GLSL base type ("2D", "2DArray", "Cube", ...) for a bound unit */
static const char *SMP(Mx *e, int unit, const char *base, int shadow) {
    const char *pre = "";
    if (!shadow && (unit_bit(e->in->int_tex, unit))) pre = (unit_bit(e->in->sint_tex, unit)) ? "i" : "u";
    if (e->in->target == 1) {                /* Vulkan: a named sampler per (unit, type), declared in the header from e->smp */
        char type[24];
        int k, same_unit = 0;
        snprintf(type, sizeof type, "%ssampler%s%s", pre, base, shadow ? "Shadow" : "");
        for (k = 0; k < e->nsmp; k++) { if (e->smp[k].unit != unit) continue; if (!strcmp(e->smp[k].type, type)) break; same_unit++; }
        if (k == e->nsmp) {
            if (same_unit > 1 || e->nsmp == 64) { fail(e, "texture unit with three sampler types", "texture unit %d is used with more than two sampler types (%s)", unit, type); return "mx_none"; }
            e->smp[k].unit = (uint8_t)unit; e->smp[k].shadow = (uint8_t)shadow; e->smp[k].binding = (uint16_t)(e->in->tex_base + (same_unit ? 240u : 0u) + (unsigned)unit);
            snprintf(e->smp[k].type, sizeof e->smp[k].type, "%s", type);
            e->nsmp++;
        }
        return S(e, "mx_t%d_%s", unit, type);
    }
    return S(e, "%ssampler%s%s(uvec2(c2[%uu], c2[%uu]))", pre, base, shadow ? "Shadow" : "", 8 + 2 * unit, 9 + 2 * unit);
}
static int tex_is_int(Mx *e, int unit, int shadow) { return !shadow && unit >= 0 && (unit_bit(e->in->int_tex, unit)); }
static const char *ARR(Mx *e, unsigned r) { return S(e, "float(%s & 0xffffu)", X(e, r)); }
static const char *FR(Mx *e, unsigned r) { return S(e, "F(%s)", X(e, r)); }
static const char *IR(Mx *e, unsigned r) { return S(e, "int(%s)", X(e, r)); }
static const char *const k_tex_base[8] = {"1D", "1DArray", "2D", "2DArray", "3D", NULL, "Cube", "CubeArray"};
/* coordinates of TEX / TLD4 / TMML-style instructions for texture type t (0..7), starting at register c */
static const char *tex_coords(Mx *e, unsigned t, unsigned c, int *dim) {
    switch (t) {
    case 0: *dim = 1; return FR(e, c);
    case 1: *dim = 2; return S(e, "vec2(%s, %s)", FR(e, c + 1), ARR(e, c));
    case 2: *dim = 2; return S(e, "vec2(%s, %s)", FR(e, c), FR(e, c + 1));
    case 3: *dim = 3; return S(e, "vec3(%s, %s, %s)", FR(e, c + 1), FR(e, c + 2), ARR(e, c));
    case 4: case 6: *dim = 3; return S(e, "vec3(%s, %s, %s)", FR(e, c), FR(e, c + 1), FR(e, c + 2));
    case 7: *dim = 4; return S(e, "vec4(%s, %s, %s, %s)", FR(e, c + 1), FR(e, c + 2), FR(e, c + 3), ARR(e, c));
    }
    fail(e, "texture type", "texture type %u (3D array)", t);
    *dim = 1;
    return "0.0";
}
/* shadow coordinate vector: GLSL wants the reference as the last component (except cube arrays) */
static const char *shadow_coords(Mx *e, unsigned t, const char *coords, int dim, const char *dref) {
    if (t == 7) return coords;                                   /* cube array: reference is a separate argument */
    if (t == 0) return S(e, "vec3(%s, 0.0, %s)", coords, dref);
    return S(e, "vec%d(%s, %s)", dim + 1, coords, dref);
}
static const char *tex_offset(Mx *e, unsigned t, const char *reg, int bits) {      /* signed 4-bit (TEX/TLD/TXD) or 6-bit (TLD4) fields */
    int step = bits == 4 ? 4 : 8;
    if (t == 6 || t == 7) { fail(e, "texture offset", "offset on a cube texture"); return "0"; }
    if (t <= 1) return S(e, "bitfieldExtract(int(%s), 0, %d)", reg, bits);
    if (t <= 3) return S(e, "ivec2(bitfieldExtract(int(%s), 0, %d), bitfieldExtract(int(%s), %d, %d))", reg, bits, reg, step, bits);
    return S(e, "ivec3(bitfieldExtract(int(%s), 0, %d), bitfieldExtract(int(%s), %d, %d), bitfieldExtract(int(%s), %d, %d))", reg, bits, reg, step, bits, reg, 2 * step, bits);
}
/* Vulkan target: GLSL takes textureOffset / texelFetchOffset offsets only as constant expressions, the GPU takes them
 * from a register. A filtered sample at an offset is the sample at coords + offset / size of the level sampled (the level
 * the implicit LOD selects: textureQueryLod(...).x, rounded to the nearest level; with an explicit LOD that level); a fetch
 * at an offset is the fetch at coords + offset. The offset register RZ stays a constant (glslang folds it). */
static int off_dynamic(Mx *e, unsigned reg) { return e->in->target == 1 && reg != 255; }
static const char *tex_off_coords(Mx *e, unsigned t, const char *smp, const char *coords, const char *off, const char *lod) {
    static const char *const ft[5] = {"float", "vec2", "vec2", "vec3", "vec3"}, *const sw[5] = {"", ".x", ".xy", ".xy", ".xyz"}, *const fo[5] = {"float", "float", "vec2", "vec2", "vec3"};
    if (t > 4) return coords;
    const char *uv = t == 0 ? "cc" : t == 1 ? "cc.x" : t == 3 ? "cc.xy" : "cc";
    emit(e, "%s cc = %s; ", ft[t], coords);
    if (lod) emit(e, "int lv = int(%s); ", lod);
    else if (e->in->stage == MX_FRAGMENT) emit(e, "int lv = int(textureQueryLod(%s, %s).x + 0.5); ", smp, uv);
    else emit(e, "int lv = 0; ");
    emit(e, "%s += %s(%s) / %s(textureSize(%s, lv)%s); ", uv, fo[t], off, fo[t], smp, sw[t]);
    return "cc";
}
static const char *fetch_off_coords(unsigned t, Mx *e, const char *coords, const char *off) {      /* integer coords + offset */
    switch (t) {
    case 0: return S(e, "(%s + %s)", coords, off);
    case 1: return S(e, "(%s + ivec2(%s, 0))", coords, off);
    case 3: return S(e, "(%s + ivec3(%s, 0))", coords, off);
    default: return S(e, "(%s + %s)", coords, off);
    }
}
/* stores the components selected by mask into consecutive registers from rd */
static void tex_store_mask(Mx *e, unsigned mask, unsigned rd, int is_int) {
    unsigned r = rd;
    for (unsigned k = 0; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        if (r != 255) emit(e, " %s = %s;", D(e, r), is_int ? S(e, "uint(tv[%u])", k) : S(e, "U(tv[%u])", k));
        r = r == 255 ? 255 : r + 1;
    }
}
/* TEXS / TLDS / TLD4S: components into rd0, rd0+1, rd1, rd1+1, or packed as halves */
static void tex_store_pair(Mx *e, unsigned mask, unsigned rd0, unsigned rd1, int f16, int is_int) {
    unsigned idx = 0;
    const char *v[4] = {"0.0", "0.0", "0.0", "0.0"};
    for (unsigned k = 0; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        const char *val = is_int ? S(e, "uint(tv[%u])", k) : S(e, "U(tv[%u])", k);
        if (f16) { v[idx] = S(e, "tv[%u]", k); idx++; continue; }
        unsigned r = idx == 0 ? rd0 : idx == 1 ? (rd0 == 255 ? 255 : rd0 + 1) : idx == 2 ? rd1 : (rd1 == 255 ? 255 : rd1 + 1);
        if (r != 255) emit(e, " %s = %s;", D(e, r), val);
        idx++;
    }
    if (f16) {
        if (idx >= 1 && rd0 != 255) emit(e, " %s = packHalf2x16(vec2(%s, %s));", D(e, rd0), v[0], idx >= 2 ? v[1] : "0.0");
        if (idx >= 3 && rd1 != 255) emit(e, " %s = packHalf2x16(vec2(%s, %s));", D(e, rd1), v[2], idx >= 4 ? v[3] : "0.0");
    }
}
static const char *tv_decl(int is_int, int sint) { return is_int ? (sint ? "ivec4 tv = " : "uvec4 tv = ") : "vec4 tv = "; }

/* TEX (bound): coords at Ra, then [lod/bias] [offset] [dref] from Rb */
static void op_tex(Mx *e, uint64_t i) {
    unsigned t = MXB(i, 28, 3), mask = MXB(i, 31, 4), dc = MXB(i, 50, 1), aoffi = MXB(i, 54, 1), blod = MXB(i, 55, 3), meta = MXB(i, 20, 8);
    int unit = tex_unit(e, i), dim;
    if (unit < 0) return;
    if (MXB(i, 58, 1)) { fail(e, "TEX.LC", "TEX with LOD clamp"); return; }
    if (blod == 4 || blod == 5 || !k_tex_base[t]) { fail(e, "TEX", "TEX type %u lod mode %u", t, blod); return; }
    const char *coords = tex_coords(e, t, MXB(i, 8, 8), &dim), *lod = NULL, *off = NULL, *dref = NULL;
    if (blod >= 2) lod = FR(e, meta++);
    unsigned offreg = aoffi ? meta : 255;
    if (aoffi) off = tex_offset(e, t, X(e, meta++), 4);
    if (dc) dref = FR(e, meta++);
    int explicit_lod = blod == 1 || blod == 3 || blod == 7, bias = blod == 2 || blod == 6, is_int = tex_is_int(e, unit, dc);
    if (e->in->stage != MX_FRAGMENT && !explicit_lod) { explicit_lod = 1; bias = 0; lod = NULL; }
    const char *lodv = blod == 1 || !lod ? "0.0" : lod, *smp = SMP(e, unit, k_tex_base[t], dc), *call;
    if (dc) {
        if (off && off_dynamic(e, offreg)) { fail(e, "TEX.AOFFI", "shadow lookup with an offset from a register (Vulkan GLSL needs a constant)"); return; }
        const char *p = shadow_coords(e, t, coords, dim, dref);
        if (t == 7 && (explicit_lod || off || bias)) { fail(e, "TEX", "shadow cube array with lod/offset/bias"); return; }
        if (t == 7) call = S(e, "texture(%s, %s, %s)", smp, p, dref);
        else if (explicit_lod) call = off ? S(e, "textureLodOffset(%s, %s, %s, %s)", smp, p, lodv, off) : S(e, "textureLod(%s, %s, %s)", smp, p, lodv);
        else call = off ? S(e, "textureOffset(%s, %s, %s%s%s)", smp, p, off, bias ? ", " : "", bias ? lod : "") : S(e, "texture(%s, %s%s%s)", smp, p, bias ? ", " : "", bias ? lod : "");
        emit(e, "float sv = %s; vec4 tv = vec4(sv, sv, sv, 1.0);", call);
    } else {
        if (off && off_dynamic(e, offreg) && t <= 4) { coords = tex_off_coords(e, t, smp, coords, off, explicit_lod ? lodv : NULL); off = NULL; }
        if (explicit_lod) call = off ? S(e, "textureLodOffset(%s, %s, %s, %s)", smp, coords, lodv, off) : S(e, "textureLod(%s, %s, %s)", smp, coords, lodv);
        else call = off ? S(e, "textureOffset(%s, %s, %s%s%s)", smp, coords, off, bias ? ", " : "", bias ? lod : "") : S(e, "texture(%s, %s%s%s)", smp, coords, bias ? ", " : "", bias ? lod : "");
        emit(e, "%s%s;", tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    }
    tex_store_mask(e, mask, MXB(i, 0, 8), is_int);
    if (MXB(i, 51, 3) != 7) emit(e, " %s = true;", PD(e, MXB(i, 51, 3)));     /* sparse residency: always resident */
}

/* TEXS: compact sample, 14 encodings (suyu texture_fetch_swizzled.cpp) */
static void op_texs(Mx *e, uint64_t i) {
    static const uint8_t rg[8] = {1, 2, 4, 8, 3, 9, 10, 12}, rgba[5] = {7, 11, 13, 14, 15};
    unsigned enc = MXB(i, 53, 4), a = MXB(i, 8, 8), b = MXB(i, 20, 8), rd0 = MXB(i, 0, 8), rd1 = MXB(i, 28, 8), sw = MXB(i, 50, 3);
    int unit = tex_unit(e, i), shadow = enc == 4 || enc == 5 || enc == 6 || enc == 9;
    if (unit < 0) return;
    unsigned mask = rd1 == 255 ? rg[sw] : (sw < 5 ? rgba[sw] : 0);
    if (!mask) { fail(e, "TEXS", "TEXS swizzle %u", sw); return; }
    int vs = e->in->stage != MX_FRAGMENT, is_int = tex_is_int(e, unit, shadow);
    /* the 2D sampler is named only by the encodings that use it. SMP() DECLARES the sampler in the Vulkan text;
     * asked for every encoding it left an unused "sampler2D" on the unit of a 3D / cube / array fetch (the real one then
     * sat at binding 240 + unit and the back end bound a dummy to the unused 2D one). */
    const char *call = NULL, *s2 = enc >= 1 && enc <= 6 ? SMP(e, unit, "2D", shadow) : NULL;
    switch (enc) {
    case 0: call = S(e, "textureLod(%s, %s, 0.0)", SMP(e, unit, "1D", 0), FR(e, a)); break;
    case 1: call = vs ? S(e, "textureLod(%s, vec2(%s, %s), 0.0)", s2, FR(e, a), FR(e, b)) : S(e, "texture(%s, vec2(%s, %s))", s2, FR(e, a), FR(e, b)); break;
    case 2: call = S(e, "textureLod(%s, vec2(%s, %s), 0.0)", s2, FR(e, a), FR(e, b)); break;
    case 3: call = S(e, "textureLod(%s, vec2(%s, %s), %s)", s2, FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 4: call = vs ? S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", s2, FR(e, a), FR(e, a + 1), FR(e, b)) : S(e, "texture(%s, vec3(%s, %s, %s))", s2, FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 5: call = S(e, "textureLod(%s, vec3(%s, %s, %s), %s)", s2, FR(e, a), FR(e, a + 1), FR(e, b + 1), FR(e, b)); break;
    case 6: call = S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", s2, FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 7: call = vs ? S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", SMP(e, unit, "2DArray", 0), FR(e, a + 1), FR(e, b), ARR(e, a))
                      : S(e, "texture(%s, vec3(%s, %s, %s))", SMP(e, unit, "2DArray", 0), FR(e, a + 1), FR(e, b), ARR(e, a)); break;
    case 8: call = S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", SMP(e, unit, "2DArray", 0), FR(e, a + 1), FR(e, b), ARR(e, a)); break;
    case 9: call = S(e, "textureLod(%s, vec4(%s, %s, %s, %s), 0.0)", SMP(e, unit, "2DArray", 1), FR(e, a + 1), FR(e, b), ARR(e, a), FR(e, b + 1)); break;
    case 10: call = vs ? S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", SMP(e, unit, "3D", 0), FR(e, a), FR(e, a + 1), FR(e, b))
                       : S(e, "texture(%s, vec3(%s, %s, %s))", SMP(e, unit, "3D", 0), FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 11: call = S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", SMP(e, unit, "3D", 0), FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 12: call = vs ? S(e, "textureLod(%s, vec3(%s, %s, %s), 0.0)", SMP(e, unit, "Cube", 0), FR(e, a), FR(e, a + 1), FR(e, b))
                       : S(e, "texture(%s, vec3(%s, %s, %s))", SMP(e, unit, "Cube", 0), FR(e, a), FR(e, a + 1), FR(e, b)); break;
    case 13: call = S(e, "textureLod(%s, vec3(%s, %s, %s), %s)", SMP(e, unit, "Cube", 0), FR(e, a), FR(e, a + 1), FR(e, b), FR(e, b + 1)); break;
    default: fail(e, "TEXS", "TEXS encoding %u", enc); return;
    }
    if (shadow) emit(e, "float sv = %s; vec4 tv = vec4(sv, sv, sv, 1.0);", call);
    else emit(e, "%s%s;", tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    tex_store_pair(e, mask, rd0, rd1, !MXB(i, 59, 1), is_int);
}

/* MxIn.screen_scale, fragment stage: the integer coordinates of a texelFetch from a 2D sampler are in the game's
 * texel grid; the image may be smaller (internal resolution scale). mx_tf maps them with the ratio real size / the size
 * the game was told (c2 words 96 + 4u, the table TXQ reads); it returns them unchanged when the two are equal. An offset
 * is added first (then *off is cleared). */
static const char *tf_scaled(Mx *e, int unit, const char *smp, const char *coords, const char **off) {
    if (!e->in->screen_scale || e->in->target != 1 || e->in->stage != MX_FRAGMENT) return coords;
    unsigned w = 96 + 4 * (unsigned)unit;
    e->uses_tf = 1; e->cbufs |= 4;
    if (off && *off) { coords = S(e, "(%s + %s)", coords, *off); *off = NULL; }
    return S(e, "mx_tf(%s, textureSize(%s, 0), c2[%uu], c2[%uu])", coords, smp, w, w + 1);
}

/* TLD (bound): integer coords, [lod] [offset] [sample] from Rb */
static void op_tld(Mx *e, uint64_t i) {
    unsigned t = MXB(i, 28, 3), mask = MXB(i, 31, 4), c = MXB(i, 8, 8), meta = MXB(i, 20, 8), ms = MXB(i, 50, 1);
    int unit = tex_unit(e, i);
    if (unit < 0) return;
    if (MXB(i, 54, 1)) { fail(e, "TLD.CL", "TLD with LOD clamp"); return; }
    const char *coords, *base;
    switch (t) {
    case 0: coords = IR(e, c); base = "1D"; break;
    case 1: coords = S(e, "ivec2(%s, int(%s & 0xffffu))", IR(e, c + 1), X(e, c)); base = "1DArray"; break;
    case 2: coords = S(e, "ivec2(%s, %s)", IR(e, c), IR(e, c + 1)); base = ms ? "2DMS" : "2D"; break;
    case 3: coords = S(e, "ivec3(%s, %s, int(%s & 0xffffu))", IR(e, c + 1), IR(e, c + 2), X(e, c)); base = ms ? "2DMSArray" : "2DArray"; break;
    case 4: coords = S(e, "ivec3(%s, %s, %s)", IR(e, c), IR(e, c + 1), IR(e, c + 2)); base = "3D"; break;
    default: fail(e, "TLD", "TLD texture type %u", t); return;
    }
    unsigned offreg = MXB(i, 35, 1) ? meta + MXB(i, 55, 1) : 255;
    const char *lod = MXB(i, 55, 1) ? IR(e, meta++) : "0", *off = MXB(i, 35, 1) ? tex_offset(e, t, X(e, meta++), 4) : NULL, *smpl = ms ? IR(e, meta++) : NULL;
    int is_int = tex_is_int(e, unit, 0);
    const char *smp = SMP(e, unit, base, 0), *call;
    if (t == 2 && !ms) coords = tf_scaled(e, unit, smp, coords, &off);
    if (off && !ms && off_dynamic(e, offreg)) { coords = fetch_off_coords(t, e, coords, off); off = NULL; }
    if (ms) call = S(e, "texelFetch(%s, %s, %s)", smp, coords, smpl);
    else call = off ? S(e, "texelFetchOffset(%s, %s, %s, %s)", smp, coords, lod, off) : S(e, "texelFetch(%s, %s, %s)", smp, coords, lod);
    emit(e, "%s%s;", tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    tex_store_mask(e, mask, MXB(i, 0, 8), is_int);
    if (MXB(i, 51, 3) != 7) emit(e, " %s = true;", PD(e, MXB(i, 51, 3)));
}

static void op_tlds(Mx *e, uint64_t i) {
    static const uint8_t rg[8] = {1, 2, 4, 8, 3, 9, 10, 12}, rgba[5] = {7, 11, 13, 14, 15};
    unsigned enc = MXB(i, 53, 4), a = MXB(i, 8, 8), b = MXB(i, 20, 8), rd0 = MXB(i, 0, 8), rd1 = MXB(i, 28, 8), sw = MXB(i, 50, 3);
    int unit = tex_unit(e, i);
    if (unit < 0) return;
    unsigned mask = rd1 == 255 ? rg[sw] : (sw < 5 ? rgba[sw] : 0);
    if (!mask) { fail(e, "TLDS", "TLDS swizzle %u", sw); return; }
    int is_int = tex_is_int(e, unit, 0);
    const char *call, *off;
    switch (enc) {
    case 0: call = S(e, "texelFetch(%s, %s, 0)", SMP(e, unit, "1D", 0), IR(e, a)); break;
    case 1: call = S(e, "texelFetch(%s, %s, %s)", SMP(e, unit, "1D", 0), IR(e, a), IR(e, b)); break;
#define TLDS_RS (e->in->screen_scale && e->in->target == 1 && e->in->stage == MX_FRAGMENT)       /* see tf_scaled */
    case 2: if (TLDS_RS) { const char *sm = SMP(e, unit, "2D", 0); call = S(e, "texelFetch(%s, %s, 0)", sm, tf_scaled(e, unit, sm, S(e, "ivec2(%s, %s)", IR(e, a), IR(e, b)), NULL)); break; }
        call = S(e, "texelFetch(%s, ivec2(%s, %s), 0)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, b)); break;
    case 4: off = tex_offset(e, 2, X(e, b), 4);
        if (TLDS_RS) { const char *sm = SMP(e, unit, "2D", 0); call = S(e, "texelFetch(%s, %s, 0)", sm, tf_scaled(e, unit, sm, S(e, "ivec2(%s, %s)", IR(e, a), IR(e, a + 1)), &off)); break; }
        if (off_dynamic(e, b)) { call = S(e, "texelFetch(%s, ivec2(%s, %s) + %s, 0)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, a + 1), off); break; }
        call = S(e, "texelFetchOffset(%s, ivec2(%s, %s), 0, %s)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, a + 1), off); break;
    case 5: if (TLDS_RS) { const char *sm = SMP(e, unit, "2D", 0); call = S(e, "texelFetch(%s, %s, %s)", sm, tf_scaled(e, unit, sm, S(e, "ivec2(%s, %s)", IR(e, a), IR(e, a + 1)), NULL), IR(e, b)); break; }
        call = S(e, "texelFetch(%s, ivec2(%s, %s), %s)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, a + 1), IR(e, b)); break;
    case 6: call = S(e, "texelFetch(%s, ivec2(%s, %s), %s)", SMP(e, unit, "2DMS", 0), IR(e, a), IR(e, a + 1), IR(e, b)); break;
    case 7: call = S(e, "texelFetch(%s, ivec3(%s, %s, %s), 0)", SMP(e, unit, "3D", 0), IR(e, a), IR(e, a + 1), IR(e, b)); break;
    case 8: call = S(e, "texelFetch(%s, ivec3(%s, %s, int(%s & 0xffffu)), 0)", SMP(e, unit, "2DArray", 0), IR(e, b), IR(e, b + 1), X(e, a)); break;
    case 12: off = tex_offset(e, 2, X(e, b + 1), 4);
        if (TLDS_RS) { const char *sm = SMP(e, unit, "2D", 0); call = S(e, "texelFetch(%s, %s, %s)", sm, tf_scaled(e, unit, sm, S(e, "ivec2(%s, %s)", IR(e, a), IR(e, a + 1)), &off), IR(e, b)); break; }
        if (off_dynamic(e, b + 1)) { call = S(e, "texelFetch(%s, ivec2(%s, %s) + %s, %s)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, a + 1), off, IR(e, b)); break; }
        call = S(e, "texelFetchOffset(%s, ivec2(%s, %s), %s, %s)", SMP(e, unit, "2D", 0), IR(e, a), IR(e, a + 1), IR(e, b), off); break;
    default: fail(e, "TLDS", "TLDS encoding %u", enc); return;
    }
    emit(e, "%s%s;", tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    tex_store_pair(e, mask, rd0, rd1, !MXB(i, 59, 1), is_int);
}

/* TLD4 (bound): gather */
static void op_tld4(Mx *e, uint64_t i) {
    unsigned t = MXB(i, 28, 3), mask = MXB(i, 31, 4), dc = MXB(i, 50, 1), meta = MXB(i, 20, 8), comp = MXB(i, 56, 2), ot = MXB(i, 54, 2);
    int unit = tex_unit(e, i), dim;
    if (unit < 0) return;
    if (t != 2 && t != 3 && t != 6 && t != 7) { fail(e, "TLD4", "gather on texture type %u", t); return; }
    if (ot == 3 || (ot == 2 && t != 2 && t != 3)) { fail(e, "TLD4.PTP", "gather offset mode %u on texture type %u", ot, t); return; }
    const char *coords = tex_coords(e, t, MXB(i, 8, 8), &dim), *off = ot == 1 ? tex_offset(e, t, X(e, meta++), 6) : NULL, *call;
    const char *p1 = ot == 2 ? X(e, meta++) : NULL, *p2 = ot == 2 ? X(e, meta++) : NULL, *dref = dc ? FR(e, meta++) : NULL;
    int is_int = tex_is_int(e, unit, dc);
    const char *smp = SMP(e, unit, k_tex_base[t], dc);
    if (ot == 2) {
        /* four independent texel offsets (x, y pairs of signed 6-bit fields at bits 0, 8, 16, 24 of two registers). GLSL's
         * textureGatherOffsets wants constants, so each texel is taken as the (i0, j0) texel (.w) of an offset gather. */
        static const char *const o[4] = {"o1.xy", "o1.zw", "o2.xy", "o2.zw"};
        emit(e, "ivec4 o1 = ivec4(bitfieldExtract(int(%s), 0, 6), bitfieldExtract(int(%s), 8, 6), bitfieldExtract(int(%s), 16, 6), bitfieldExtract(int(%s), 24, 6));", p1, p1, p1, p1);
        emit(e, " ivec4 o2 = ivec4(bitfieldExtract(int(%s), 0, 6), bitfieldExtract(int(%s), 8, 6), bitfieldExtract(int(%s), 16, 6), bitfieldExtract(int(%s), 24, 6));", p2, p2, p2, p2);
        emit(e, " %s tv;", dc ? "vec4" : is_int ? ((unit_bit(e->in->sint_tex, unit)) ? "ivec4" : "uvec4") : "vec4");
        for (int k = 0; k < 4; k++) {
            if (dc) emit(e, " tv[%d] = textureGatherOffset(%s, %s, %s, %s).w;", k, smp, coords, dref, o[k]);
            else emit(e, " tv[%d] = textureGatherOffset(%s, %s, %s, %u).w;", k, smp, coords, o[k], comp);
        }
        tex_store_mask(e, mask, MXB(i, 0, 8), is_int);
        if (MXB(i, 51, 3) != 7) emit(e, " %s = true;", PD(e, MXB(i, 51, 3)));
        return;
    }
    if (dc) call = off ? S(e, "textureGatherOffset(%s, %s, %s, %s)", smp, coords, dref, off) : S(e, "textureGather(%s, %s, %s)", smp, coords, dref);
    else call = off ? S(e, "textureGatherOffset(%s, %s, %s, %u)", smp, coords, off, comp) : S(e, "textureGather(%s, %s, %u)", smp, coords, comp);
    emit(e, "%s%s;", dc ? "vec4 tv = " : tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    tex_store_mask(e, mask, MXB(i, 0, 8), is_int);
    if (MXB(i, 51, 3) != 7) emit(e, " %s = true;", PD(e, MXB(i, 51, 3)));
}

static void op_tld4s(Mx *e, uint64_t i) {
    unsigned a = MXB(i, 8, 8), b = MXB(i, 20, 8), comp = MXB(i, 52, 2), dc = MXB(i, 50, 1), aoffi = MXB(i, 51, 1);
    int unit = tex_unit(e, i);
    if (unit < 0) return;
    int is_int = tex_is_int(e, unit, dc);
    const char *smp = SMP(e, unit, "2D", dc), *call;
    if (aoffi) {
        const char *off = tex_offset(e, 2, X(e, b), 6), *co = S(e, "vec2(%s, %s)", FR(e, a), FR(e, a + 1));
        call = dc ? S(e, "textureGatherOffset(%s, %s, %s, %s)", smp, co, FR(e, b + 1), off) : S(e, "textureGatherOffset(%s, %s, %s, %u)", smp, co, off, comp);
    } else if (dc) call = S(e, "textureGather(%s, vec2(%s, %s), %s)", smp, FR(e, a), FR(e, a + 1), FR(e, b));
    else call = S(e, "textureGather(%s, vec2(%s, %s), %u)", smp, FR(e, a), FR(e, b), comp);
    emit(e, "%s%s;", dc ? "vec4 tv = " : tv_decl(is_int, unit_bit(e->in->sint_tex, unit)), call);
    tex_store_pair(e, 15, MXB(i, 0, 8), MXB(i, 28, 8), MXB(i, 55, 1), is_int);    /* precision bit: 1 = F16 here */
}

static void op_tmml_unit(Mx *e, uint64_t i, int unit);
static void op_tmml(Mx *e, uint64_t i) { int unit = tex_unit(e, i); if (unit >= 0) op_tmml_unit(e, i, unit); }
/* TMML (b), SMO: the texture handle is in register Rb (bits 20..27; [doc] yuzu texture_mipmap_level.cpp: the bindless form
 * reads the handle from meta_reg, coordinates as in the bound form). Vulkan has no bindless samplers, so where the handle
 * comes from must be known: the last write of that register in the same block must be an unpredicated MOV from a
 * constant buffer word. From c[2][word w], an entry of the texture handle table (SMO's 173 stages: MOV r3, c[2][0x20]
 * right before), it is unit (w - 8) / 2 as for a bound TMML. From one of the game's uniform buffers c[3..17], at an
 * even word w < 16 (the game's primitive drawer, SystemData/primitive_drawer_nvn_shader.bin: the 64-bit handle at c[3][0]),
 * it is unit 32 + 8 * buffer + w / 2 (Vulkan only): the back end reads the handle from that buffer at each draw. */
/* The unit a bindless texture instruction at word pc names through handle register h, or -1 (after fail()): see TMML (b). */
static int bindless_unit(Mx *e, unsigned pc, unsigned h, const char *name) {
    if (e->leader[pc]) { fail(e, name, "%s at word %u starts a block: its handle register r%u is not known", name, pc, h); return -1; }
    for (unsigned k = pc - 1; k >= 1; k--) {      /* back to the block's first instruction (words k % 4 == 0 are scheduling words) */
        if (k % 4 == 0) continue;
        uint64_t w = *(const uint64_t *)(e->base + (size_t)k * 8);
        if (MXB(w, 0, 8) != h) { if (e->leader[k]) break; continue; }
        unsigned off = MXB(w, 20, 14), cb = MXB(w, 34, 5);
        if (decode(w) != MXOP_MOV_cbuf || MXB(w, 16, 3) != 7) break;
        if (cb == 2 && off >= 8 && !(off & 1) && (off - 8) / 2 < 32) {
            e->cbufs |= 4;
            e->tex_units |= 1u << ((off - 8) / 2);
            return (int)(off - 8) / 2;
        }
        if (e->in->target == 1 && cb >= 3 && cb < 18 && !(off & 1) && off < 16) {
            e->cbufs |= 1u << cb;
            return 32 + (int)(cb * 8 + off / 2);
        }
        break;
    }
    fail(e, name, "%s at word %u: the handle register r%u is not a MOV from the texture handle table c[2] or from the first 64 bytes of a uniform buffer, in the same block", name, pc, h);
    return -1;
}
static void op_tmml_b(Mx *e, unsigned pc, uint64_t i) {
    int unit = bindless_unit(e, pc, MXB(i, 20, 8), "TMML (b)");
    if (unit >= 0) op_tmml_unit(e, i, unit);
}
/* TEX (b): the bindless form of TEX ([doc] yuzu texture_fetch.cpp TEX_b): the handle in register Rb (bits 20..27),
 * the other meta operands (lod, offset, depth reference) in the registers after it; AOFFI, the LOD mode and LC at bits 36,
 * 37..39 and 40 instead of 54, 55..57 and 58; every other field as in TEX. The handle is resolved as for TMML (b), and the
 * instruction is translated as the bound TEX of that unit. */
static void op_tex_b(Mx *e, unsigned pc, uint64_t i) {
    unsigned h = MXB(i, 20, 8);
    int unit = bindless_unit(e, pc, h, "TEX (b)");
    if (unit < 0) return;
    uint64_t b = 0xc000000000000000ull;                       /* TEX */
    b |= i & 0x0000000fffffffffull & ~(0xffull << 20);        /* bits 0..35: dest, coordinates, type, mask, NDV */
    b |= (uint64_t)((h + 1) & 0xff) << 20;                    /* the meta operands after the handle register */
    b |= (uint64_t)(8 + 2 * unit) << 36;                      /* the unit's word of the texture handle table */
    b |= i & (0x1full << 49);                                 /* NODEP, DC, sparse predicate */
    b |= (uint64_t)MXB(i, 36, 1) << 54 | (uint64_t)MXB(i, 37, 3) << 55 | (uint64_t)MXB(i, 40, 1) << 58;
    e->dyn_unit_ok = unit >= 32;
    op_tex(e, b);
    e->dyn_unit_ok = 0;
}
static void op_tmml_unit(Mx *e, uint64_t i, int unit) {
    unsigned t = MXB(i, 28, 3), c = MXB(i, 8, 8), mask = MXB(i, 31, 4), r = MXB(i, 0, 8);
    if (e->in->stage != MX_FRAGMENT) { fail(e, "TMML", "LOD query outside a fragment shader"); return; }
    const char *coords;
    switch (t) {
    case 0: coords = FR(e, c); break;
    case 1: coords = FR(e, c + 1); break;
    case 2: coords = S(e, "vec2(%s, %s)", FR(e, c), FR(e, c + 1)); break;
    case 3: coords = S(e, "vec2(%s, %s)", FR(e, c + 1), FR(e, c + 2)); break;
    case 4: case 6: coords = S(e, "vec3(%s, %s, %s)", FR(e, c), FR(e, c + 1), FR(e, c + 2)); break;
    case 7: coords = S(e, "vec3(%s, %s, %s)", FR(e, c + 1), FR(e, c + 2), FR(e, c + 3)); break;
    default: fail(e, "TMML", "type %u", t); return;
    }
    emit(e, "vec2 q = textureQueryLod(%s, %s);", SMP(e, unit, k_tex_base[t], 0), coords);
    for (unsigned k = 0; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        if (r != 255) emit(e, " %s = %s;", D(e, r), k < 2 ? S(e, "uint(int(q[%u] * 256.0))", k) : "0u");      /* fixed point 8.8 */
        r = r == 255 ? 255 : r + 1;
    }
}

static void op_txd(Mx *e, uint64_t i) {
    unsigned t = MXB(i, 28, 3), mask = MXB(i, 31, 4), c = MXB(i, 8, 8), d = MXB(i, 20, 8);
    int unit = tex_unit(e, i);
    if (unit < 0) return;
    if (MXB(i, 50, 1)) { fail(e, "TXD.LC", "TXD with LOD clamp"); return; }
    const char *coords, *dx, *dy;
    unsigned last;
    switch (t) {
    case 0: coords = FR(e, c); last = c + 1; dx = FR(e, d); dy = FR(e, d + 1); break;
    case 1: coords = S(e, "vec2(%s, %s)", FR(e, c), ARR(e, c + 1)); last = c + 1; dx = FR(e, d); dy = FR(e, d + 1); break;
    case 2: coords = S(e, "vec2(%s, %s)", FR(e, c), FR(e, c + 1)); last = c + 2;
        dx = S(e, "vec2(%s, %s)", FR(e, d), FR(e, d + 2)); dy = S(e, "vec2(%s, %s)", FR(e, d + 1), FR(e, d + 3)); break;
    case 3: coords = S(e, "vec3(%s, %s, %s)", FR(e, c), FR(e, c + 1), ARR(e, c + 2)); last = c + 2;
        dx = S(e, "vec2(%s, %s)", FR(e, d), FR(e, d + 2)); dy = S(e, "vec2(%s, %s)", FR(e, d + 1), FR(e, d + 3)); break;
    default: fail(e, "TXD", "TXD texture type %u", t); return;
    }
    const char *off = NULL;
    if (MXB(i, 35, 1)) {
        const char *rl = X(e, last);
        off = t <= 1 ? S(e, "bitfieldExtract(int(%s), 16, 4)", rl) : S(e, "ivec2(bitfieldExtract(int(%s), 16, 4), bitfieldExtract(int(%s), 20, 4))", rl, rl);
    }
    int is_int = tex_is_int(e, unit, 0);
    const char *smp = SMP(e, unit, k_tex_base[t], 0);
    emit(e, "%s%s;", tv_decl(is_int, unit_bit(e->in->sint_tex, unit)),
         off ? S(e, "textureGradOffset(%s, %s, %s, %s, %s)", smp, coords, dx, dy, off) : S(e, "textureGrad(%s, %s, %s, %s)", smp, coords, dx, dy));
    tex_store_mask(e, mask, MXB(i, 0, 8), is_int);
    if (MXB(i, 51, 3) != 7) emit(e, " %s = true;", PD(e, MXB(i, 51, 3)));
}

/* TXQ (bound): dimensions from the table nvn_gl.c keeps in c2 (words 96 + 4u: width, height, depth/layers, levels | is3d << 16) */
static void op_txq(Mx *e, uint64_t i) {
    unsigned mode = MXB(i, 22, 3), mask = MXB(i, 31, 4), r = MXB(i, 0, 8);
    int unit = tex_unit(e, i);
    if (unit < 0) return;
    if (mode != 1) { fail(e, "TXQ", "TXQ mode %u", mode); return; }
    unsigned w = 96 + 4 * (unsigned)unit;
    emit(e, "uint lq = %s; uint lv = c2[%uu]; uvec4 tq = uvec4(c2[%uu] == 0u ? 0u : max(c2[%uu] >> lq, 1u), c2[%uu] == 0u ? 0u : max(c2[%uu] >> lq, 1u),"
            " (lv >> 16) != 0u ? max(c2[%uu] >> lq, 1u) : c2[%uu], lv & 0xffffu);", A8, w + 3, w, w, w + 1, w + 1, w + 2, w + 2);
    for (unsigned k = 0; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        if (r != 255) emit(e, " %s = tq[%u];", D(e, r), k);
        r = r == 255 ? 255 : r + 1;
    }
}

/* ---------------------------------------------------------------- half floats */
static const char *HX(Mx *e, const char *r, unsigned sw, int absv, int neg) {
    const char *v = S(e, "H(%s, %uu)", r, sw);
    if (absv) v = S(e, "abs(%s)", v);
    if (neg) v = S(e, "(-%s)", v);
    return v;
}
static void hres(Mx *e, uint64_t i, const char *expr, int sat, unsigned merge, int fmz_zero, const char *za, const char *zb, const char *zc) {
    emit(e, "vec2 h = %s;", expr);
    if (fmz_zero && !sat) emit(e, " h = mix(h, %s, bvec2(%s.x == 0.0 || %s.x == 0.0, %s.y == 0.0 || %s.y == 0.0));", zc, za, zb, za, zb);
    if (sat) emit(e, " h = clamp(h, 0.0, 1.0);");
    unsigned rd = MXB(i, 0, 8);
    emit(e, " %s = HM(%s, h, %uu);", D(e, rd), X(e, rd), merge);
}

/* ---------------------------------------------------------------- one instruction */
/* GLSL layout qualifier of an OpenGL internal format usable for images; *cls: 0 float / normalised, 1 unsigned integer, 2 signed integer.
 * NULL when the format is unknown (0) or has no image format qualifier. */
static const char *img_qualifier(uint16_t fmt, int *cls) {
    static const struct { uint16_t gl; uint8_t cls; const char *q; } t[] = {
        {0x8814, 0, "rgba32f"}, {0x881A, 0, "rgba16f"}, {0x8230, 0, "rg32f"}, {0x822F, 0, "rg16f"}, {0x8C3A, 0, "r11f_g11f_b10f"}, {0x822E, 0, "r32f"}, {0x822D, 0, "r16f"},
        {0x805B, 0, "rgba16"}, {0x8059, 0, "rgb10_a2"}, {0x8058, 0, "rgba8"}, {0x822C, 0, "rg16"}, {0x822B, 0, "rg8"}, {0x822A, 0, "r16"}, {0x8229, 0, "r8"},
        {0x8F9B, 0, "rgba16_snorm"}, {0x8F97, 0, "rgba8_snorm"}, {0x8F99, 0, "rg16_snorm"}, {0x8F95, 0, "rg8_snorm"}, {0x8F98, 0, "r16_snorm"}, {0x8F94, 0, "r8_snorm"},
        {0x8D70, 1, "rgba32ui"}, {0x8D76, 1, "rgba16ui"}, {0x906F, 1, "rgb10_a2ui"}, {0x8D7C, 1, "rgba8ui"}, {0x823C, 1, "rg32ui"}, {0x823A, 1, "rg16ui"}, {0x8238, 1, "rg8ui"},
        {0x8236, 1, "r32ui"}, {0x8234, 1, "r16ui"}, {0x8232, 1, "r8ui"},
        {0x8D82, 2, "rgba32i"}, {0x8D88, 2, "rgba16i"}, {0x8D8E, 2, "rgba8i"}, {0x823B, 2, "rg32i"}, {0x8239, 2, "rg16i"}, {0x8237, 2, "rg8i"}, {0x8235, 2, "r32i"}, {0x8233, 2, "r16i"}, {0x8231, 2, "r8i"}};
    if (cls) *cls = 0;
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) if (t[k].gl == fmt) { if (cls) *cls = t[k].cls; return t[k].q; }
    return NULL;
}

static void insn(Mx *e, unsigned pc, uint64_t i) {
    int op = decode(i);
    unsigned pred = MXB(i, 16, 3), pneg = MXB(i, 19, 1);
    const char *b, *c;
    e->insns++;
    if (pred == 7 && pneg) return;                                /* never executed */
    if (pred != 7) emit(e, "if (%s) ", PR(e, pred, (int)pneg));
    emit(e, "{ ");
    switch (op) {
    /* ---- moves, selects, predicates */
    case MXOP_MOV_reg: case MXOP_MOV_cbuf: case MXOP_MOV_imm: case MXOP_MOV32I: {
        unsigned m = op == MXOP_MOV32I ? MXB(i, 12, 4) : MXB(i, 39, 4);
        b = op == MXOP_MOV_reg ? R20 : op == MXOP_MOV_cbuf ? CB(e, i) : op == MXOP_MOV_imm ? I20(e, i) : I32(e, i);
        if (m == 0xf || m == 1) emit(e, "%s = %s;", DST, b);       /* other masks: suyu ignores the move too */
        break;
    }
    case MXOP_SEL_reg: case MXOP_SEL_cbuf: case MXOP_SEL_imm:
        b = op == MXOP_SEL_reg ? R20 : op == MXOP_SEL_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "%s = %s ? %s : %s;", DST, PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1)), A8, b);
        break;
    case MXOP_PSETP: {
        const char *pa = PR(e, MXB(i, 12, 3), (int)MXB(i, 15, 1)), *pb = PR(e, MXB(i, 29, 3), (int)MXB(i, 32, 1)), *pc_ = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        const char *ra = BOP(e, BOP(e, pa, pb, MXB(i, 24, 2)), pc_, MXB(i, 45, 2)), *rb = BOP(e, BOP(e, S(e, "!%s", pa), pb, MXB(i, 24, 2)), pc_, MXB(i, 45, 2));
        emit(e, "bool qa = %s, qb = %s;", ra, rb);
        if (PD(e, MXB(i, 3, 3))) emit(e, " %s = qa;", PD(e, MXB(i, 3, 3)));
        if (PD(e, MXB(i, 0, 3))) emit(e, " %s = qb;", PD(e, MXB(i, 0, 3)));
        break;
    }
    case MXOP_PSET: {
        const char *pa = PR(e, MXB(i, 12, 3), (int)MXB(i, 15, 1)), *pb = PR(e, MXB(i, 29, 3), (int)MXB(i, 32, 1)), *pc_ = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "uint res = %s ? %s : 0u; %s = res;", BOP(e, BOP(e, pa, pb, MXB(i, 24, 2)), pc_, MXB(i, 45, 2)), MXB(i, 44, 1) ? "0x3f800000u" : "0xffffffffu", DST);
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " cz = res == 0u; cs = %s; ccf = false; cov = false;", MXB(i, 44, 1) ? "false" : "res != 0u"); }
        break;
    }
    case MXOP_CSETP: {
        const char *f = FLOW(e, MXB(i, 8, 5)), *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "bool qa = %s, qb = %s;", BOP(e, f, bp, MXB(i, 45, 2)), BOP(e, S(e, "!%s", f), bp, MXB(i, 45, 2)));
        if (PD(e, MXB(i, 3, 3))) emit(e, " %s = qa;", PD(e, MXB(i, 3, 3)));
        if (PD(e, MXB(i, 0, 3))) emit(e, " %s = qb;", PD(e, MXB(i, 0, 3)));
        break;
    }
    case MXOP_CSET: {
        const char *f = FLOW(e, MXB(i, 8, 5)), *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "uint res = %s ? %s : 0u; %s = res;", BOP(e, f, bp, MXB(i, 45, 2)), MXB(i, 44, 1) ? "0x3f800000u" : "0xffffffffu", DST);
        if (MXB(i, 47, 1)) emit(e, " cz = res == 0u; cs = %s; ccf = false; cov = false;", MXB(i, 44, 1) ? "false" : "res != 0u");
        break;
    }
    case MXOP_P2R_imm: {
        unsigned mask = MXB(i, 20, 19) | (MXB(i, 56, 1) ? 0xfff80000u : 0), off = MXB(i, 41, 2) * 8, cc = MXB(i, 40, 1), n = cc ? 4 : 7;
        static const char *const fl[4] = {"cz", "cs", "ccf", "cov"};
        emit(e, "uint res = %s & 0x%08xu;", A8, ~(mask << off));
        for (unsigned k = 0; k < n; k++) if (mask >> k & 1) emit(e, " if (%s) res |= 0x%08xu;", cc ? fl[k] : PR(e, k, 0), 1u << (k + off));
        if (cc) e->uses_cc = 1;
        emit(e, " %s = res;", DST);
        break;
    }
    case MXOP_R2P_reg: case MXOP_R2P_cbuf: case MXOP_R2P_imm: {
        unsigned off = MXB(i, 41, 2) * 8, cc = MXB(i, 40, 1), n = cc ? 4 : 7;
        static const char *const fl[4] = {"cz", "cs", "ccf", "cov"};
        b = op == MXOP_R2P_reg ? R20 : op == MXOP_R2P_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "uint src = %s, msk = %s;", A8, b);
        for (unsigned k = 0; k < n; k++) emit(e, " if ((msk >> %uu & 1u) != 0u) %s = (src >> %uu & 1u) != 0u;", k, cc ? fl[k] : PD(e, k), k + off);
        if (cc) e->uses_cc = 1;
        break;
    }
    case MXOP_S2R: {
        unsigned sr = MXB(i, 20, 8);
        const char *v;
        switch (sr) {
        case 0x00: v = "gl_SubGroupInvocationARB"; break;
        case 0x11: v = e->in->stage == MX_GEOMETRY || e->in->stage == MX_TESS_CONTROL ? "uint(gl_InvocationID)" : "0u"; break;
        case 0x12: v = "0x3f800000u"; break;                     /* y direction: +1 (the window origin is applied by glClipControl) */
        case 0x13: v = e->in->stage == MX_FRAGMENT ? "(gl_HelperInvocation ? 0xffffffffu : 0u)" : "0u"; break;
        case 0x1c: v = "0u"; break;                              /* affinity */
        case 0x1d:                                               /* invocation info (suyu: patch vertices << 16 in tessellation; 0x00ff0000 otherwise) */
            if (e->in->stage == MX_GEOMETRY) { static const unsigned nv[5] = {1, 2, 3, 4, 6}; v = S(e, "0x%08xu", nv[(unsigned)e->in->gs_in_prim % 5] << 16); }      /* the vertices of the input primitive */
            else v = "0x00ff0000u";
            break;
        case 0x1e: case 0x1f: v = "0x3f800000u"; break;          /* viewport w scale factors: 1.0 */
        case 0x20: v = "(MX_LID_X | (gl_LocalInvocationID.y << 16) | (gl_LocalInvocationID.z << 26))"; break;
        case 0x21: v = "MX_LID_X"; break;
        case 0x22: v = "gl_LocalInvocationID.y"; break;
        case 0x23: v = "gl_LocalInvocationID.z"; break;
        case 0x25: v = "MX_GID_X"; break;
        case 0x26: v = "gl_WorkGroupID.y"; break;
        case 0x27: v = "gl_WorkGroupID.z"; break;
        case 0x38: v = "uint(gl_SubGroupEqMaskARB)"; break;
        case 0x39: v = "uint(gl_SubGroupLtMaskARB)"; break;
        case 0x3a: v = "uint(gl_SubGroupLeMaskARB)"; break;
        case 0x3b: v = "uint(gl_SubGroupGtMaskARB)"; break;
        case 0x3c: v = "uint(gl_SubGroupGeMaskARB)"; break;
        case 0x50: case 0x52: v = "0u"; break;                   /* clocks */
        default: v = NULL; break;
        }
        if (!v) { fail(e, "S2R", "special register 0x%x", sr); break; }
        if (sr >= 0x20 && sr <= 0x27 && e->in->stage != MX_COMPUTE) v = "0u";
        emit(e, "%s = %s;", DST, v);
        break;
    }
    case MXOP_VOTE: {
        const char *p = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1)), *f = MXB(i, 48, 2) == 0 ? "allInvocationsARB" : MXB(i, 48, 2) == 1 ? "anyInvocationARB" : "allInvocationsEqualARB";
        emit(e, "bool vp = %s;", p);
        if (PD(e, MXB(i, 45, 3))) emit(e, " %s = %s(vp);", PD(e, MXB(i, 45, 3)), f);
        if (MXB(i, 0, 8) != 255) emit(e, " %s = uint(ballotARB(vp));", DST);
        break;
    }
    case MXOP_NOP: case MXOP_DEPBAR: case MXOP_MEMBAR: case MXOP_VOTE_vtg: case MXOP_CCTL: case MXOP_CCTLL: case MXOP_SSY: case MXOP_PBK: case MXOP_PCNT:
        break;
    case MXOP_BAR:                                                /* compute, the work group barrier (three stages of the game, with shared memory) */
        /* [doc] suyu barrier_operations.cpp: only BAR.SYNC with immediate inputs 0 and 0 (mode bits 0x9B00000000 = 0x8000000000,
         * a immediate bit 43 with value bits 8-15, b immediate bit 44 with value bits 20-31; predicate bits 39-41, negation 42).
         * GLSL barrier() also orders the shared memory accesses. A split work group (mx_split_x) would break it. */
        if ((i & 0x0000009B00000000ull) != 0x0000008000000000ull || !MXB(i, 43, 1) || MXB(i, 8, 8) || !MXB(i, 44, 1) || MXB(i, 20, 12) || (MXB(i, 39, 3) != 7 && MXB(i, 42, 1))) {
            fail(e, "BAR", "BAR form %016llx", (unsigned long long)i); break;
        }
        if (e->in->stage != MX_COMPUTE || mx_split_x(e->in->local_size, e->in->smem_bytes) != 1) { fail(e, "BAR", "BAR outside a compute stage or in a split work group"); break; }
        emit(e, "barrier();");
        break;

    /* ---- float arithmetic */
    case MXOP_FADD_reg: case MXOP_FADD_cbuf: case MXOP_FADD_imm:
        b = op == MXOP_FADD_reg ? R20 : op == MXOP_FADD_cbuf ? CB(e, i) : F20(e, i);
        fres(e, i, S(e, "%s + %s", FN(e, A8, MXB(i, 46, 1), MXB(i, 48, 1)), FN(e, b, MXB(i, 49, 1), MXB(i, 45, 1))), MXB(i, 50, 1), MXB(i, 47, 1));
        break;
    case MXOP_FADD32I:
        fres(e, i, S(e, "%s + %s", FN(e, A8, MXB(i, 54, 1), MXB(i, 56, 1)), FN(e, I32(e, i), MXB(i, 57, 1), MXB(i, 53, 1))), 0, MXB(i, 52, 1));
        break;
    case MXOP_FMUL_reg: case MXOP_FMUL_cbuf: case MXOP_FMUL_imm: case MXOP_FMUL32I: {
        static const char *const scale[8] = {"", " * 0.5", " * 0.25", " * 0.125", " * 8.0", " * 4.0", " * 2.0", ""};
        int is32 = op == MXOP_FMUL32I, fmz = (is32 ? MXB(i, 53, 2) : MXB(i, 44, 2)) == 2, sat = (int)(is32 ? MXB(i, 55, 1) : MXB(i, 50, 1));
        b = op == MXOP_FMUL_reg ? R20 : op == MXOP_FMUL_cbuf ? CB(e, i) : op == MXOP_FMUL_imm ? F20(e, i) : I32(e, i);
        const char *a = S(e, "(F(%s)%s)", A8, is32 ? "" : scale[MXB(i, 41, 3)]), *bb = FN(e, b, 0, is32 ? 0 : (int)MXB(i, 48, 1));
        fres(e, i, fmz_mul(e, a, bb, fmz), sat, (int)(is32 ? MXB(i, 52, 1) : MXB(i, 47, 1)));
        break;
    }
    case MXOP_FFMA_reg: case MXOP_FFMA_rc: case MXOP_FFMA_cr: case MXOP_FFMA_imm: case MXOP_FFMA32I: {
        int is32 = op == MXOP_FFMA32I, fmz = MXB(i, 53, 2) == 2, sat = (int)(is32 ? MXB(i, 55, 1) : MXB(i, 50, 1));
        switch (op) {
        case MXOP_FFMA_reg: b = R20; c = R39; break;
        case MXOP_FFMA_rc: b = R39; c = CB(e, i); break;
        case MXOP_FFMA_cr: b = CB(e, i); c = R39; break;
        case MXOP_FFMA_imm: b = F20(e, i); c = R39; break;
        default: b = I32(e, i); c = X(e, MXB(i, 0, 8)); break;
        }
        const char *a = FN(e, A8, 0, is32 ? (int)MXB(i, 56, 1) : 0), *bb = FN(e, b, 0, is32 ? 0 : (int)MXB(i, 48, 1)), *cc = FN(e, c, 0, (int)(is32 ? MXB(i, 57, 1) : MXB(i, 49, 1)));
        const char *expr = fmz && !sat ? S(e, "((%s == 0.0 || %s == 0.0) ? %s : fma(%s, %s, %s))", a, bb, cc, a, bb, cc) : S(e, "fma(%s, %s, %s)", a, bb, cc);
        fres(e, i, expr, sat, (int)(is32 ? MXB(i, 52, 1) : MXB(i, 47, 1)));
        break;
    }
    case MXOP_FMNMX_reg: case MXOP_FMNMX_cbuf: case MXOP_FMNMX_imm: {
        b = op == MXOP_FMNMX_reg ? R20 : op == MXOP_FMNMX_cbuf ? CB(e, i) : F20(e, i);
        const char *fa = FN(e, A8, MXB(i, 46, 1), MXB(i, 48, 1)), *fb = FN(e, b, MXB(i, 49, 1), MXB(i, 45, 1));
        fres(e, i, S(e, "%s ? min(%s, %s) : max(%s, %s)", PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1)), fa, fb, fa, fb), 0, MXB(i, 47, 1));
        break;
    }
    case MXOP_FSET_reg: case MXOP_FSET_cbuf: case MXOP_FSET_imm: {
        b = op == MXOP_FSET_reg ? R20 : op == MXOP_FSET_cbuf ? CB(e, i) : F20(e, i);
        const char *fa = FN(e, A8, MXB(i, 54, 1), MXB(i, 43, 1)), *fb = FN(e, b, MXB(i, 44, 1), MXB(i, 53, 1));
        emit(e, "uint res = %s ? %s : 0u; %s = res;", BOP(e, FCMP(e, MXB(i, 48, 4), fa, fb), PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1)), MXB(i, 45, 2)),
             MXB(i, 52, 1) ? "0x3f800000u" : "0xffffffffu", DST);
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " cz = res == 0u; cs = %s; ccf = false; cov = false;", MXB(i, 52, 1) ? "false" : "res != 0u"); }
        break;
    }
    case MXOP_FSETP_reg: case MXOP_FSETP_cbuf: case MXOP_FSETP_imm: {
        b = op == MXOP_FSETP_reg ? R20 : op == MXOP_FSETP_cbuf ? CB(e, i) : F20(e, i);
        const char *fa = FN(e, A8, MXB(i, 7, 1), MXB(i, 43, 1)), *fb = FN(e, b, MXB(i, 44, 1), MXB(i, 6, 1)), *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "bool cmp = %s;", FCMP(e, MXB(i, 48, 4), fa, fb));
        if (PD(e, MXB(i, 3, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 3, 3)), BOP(e, "cmp", bp, MXB(i, 45, 2)));
        if (PD(e, MXB(i, 0, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 0, 3)), BOP(e, "!cmp", bp, MXB(i, 45, 2)));
        break;
    }
    case MXOP_FCMP_reg: case MXOP_FCMP_rc: case MXOP_FCMP_cr: case MXOP_FCMP_imm: {
        const char *sa, *operand;
        switch (op) {
        case MXOP_FCMP_reg: sa = R20; operand = R39; break;
        case MXOP_FCMP_rc: sa = R39; operand = CB(e, i); break;
        case MXOP_FCMP_cr: sa = CB(e, i); operand = R39; break;
        default: sa = F20(e, i); operand = R39; break;
        }
        emit(e, "%s = %s ? %s : %s;", DST, FCMP(e, MXB(i, 48, 4), S(e, "F(%s)", operand), "0.0"), A8, sa);
        break;
    }
    case MXOP_MUFU: {
        static const char *const fn[9] = {"cos(%s)", "sin(%s)", "exp2(%s)", "log2(%s)", "(1.0 / %s)", "inversesqrt(%s)", NULL, NULL, "sqrt(%s)"};
        unsigned o = MXB(i, 20, 4);
        if (o > 8 || !fn[o]) { fail(e, "MUFU", "MUFU operation %u", o); break; }
        fres(e, i, S(e, fn[o], FN(e, A8, MXB(i, 46, 1), MXB(i, 48, 1))), MXB(i, 50, 1), 0);
        break;
    }
    case MXOP_RRO_reg: case MXOP_RRO_cbuf: case MXOP_RRO_imm:           /* range reduction before MUFU.SIN/COS/EX2: passed through (as suyu) */
        b = op == MXOP_RRO_reg ? R20 : op == MXOP_RRO_cbuf ? CB(e, i) : F20(e, i);
        fres(e, i, FN(e, b, MXB(i, 49, 1), MXB(i, 45, 1)), 0, 0);
        break;
    case MXOP_F2F_reg: case MXOP_F2F_cbuf: case MXOP_F2F_imm: {
        static const char *const rnd[4] = {"roundEven", "floor", "ceil", "trunc"};
        unsigned dsz = MXB(i, 8, 2), ssz = MXB(i, 10, 2), rop = MXB(i, 39, 4) & 0xb, sel = MXB(i, 41, 1);
        if (dsz == 3 || ssz == 3 || !dsz || !ssz) { fail(e, "F2F.F64", "F2F with sizes %u -> %u", ssz, dsz); break; }
        if (op == MXOP_F2F_imm && ssz == 1) { fail(e, "F2F (imm)", "F2F immediate half"); break; }
        b = op == MXOP_F2F_reg ? R20 : op == MXOP_F2F_cbuf ? CB(e, i) : F20(e, i);
        const char *src = ssz == 1 ? S(e, "unpackHalf2x16(%s)[%u]", b, sel) : S(e, "F(%s)", b);
        if (MXB(i, 49, 1)) src = S(e, "abs(%s)", src);
        if (MXB(i, 45, 1)) src = S(e, "(-%s)", src);
        emit(e, "float f = %s;", src);
        if (ssz == dsz && rop >= 8) emit(e, " f = %s(f);", rnd[rop & 3]);
        if (MXB(i, 50, 1)) emit(e, " f = clamp(f, 0.0, 1.0);");
        if (dsz == 1) emit(e, " %s = packHalf2x16(vec2(f, 0.0));", DST); else emit(e, " %s = U(f);", DST);
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " cz = f == 0.0 || isnan(f); cs = f < 0.0 || isnan(f); ccf = false; cov = false;"); }
        break;
    }
    case MXOP_F2I_reg: case MXOP_F2I_cbuf: {
        static const char *const rnd[4] = {"roundEven", "floor", "ceil", "trunc"};
        unsigned dsz = MXB(i, 8, 2), ssz = MXB(i, 10, 2), sgn = MXB(i, 12, 1);
        if (dsz == 3 || ssz == 3 || !dsz || !ssz) { fail(e, "F2I.64", "F2I with sizes %u -> %u", ssz, dsz); break; }
        b = op == MXOP_F2I_reg ? R20 : CB(e, i);
        const char *src = ssz == 1 ? S(e, "unpackHalf2x16(%s)[%u]", b, MXB(i, 41, 1)) : S(e, "F(%s)", b);
        if (MXB(i, 45, 1)) src = S(e, "abs(%s)", src);                 /* F2I: abs at 45, neg at 49 */
        if (MXB(i, 49, 1)) src = S(e, "(-%s)", src);
        emit(e, "%s = mx_f2i(%s(%s), %uu);", DST, rnd[MXB(i, 39, 2)], src, (dsz == 1 ? 2u : 0u) + sgn);
        if (MXB(i, 47, 1)) setcc_int(e, D(e, MXB(i, 0, 8)));
        break;
    }
    case MXOP_I2F_reg: case MXOP_I2F_cbuf: case MXOP_I2F_imm: {
        unsigned fsz = MXB(i, 8, 2), isz = MXB(i, 10, 2), sgn = MXB(i, 13, 1), sel = MXB(i, 41, 2);
        if (isz == 3 || fsz == 3 || !fsz) { fail(e, "I2F.64", "I2F with sizes %u -> %u", isz, fsz); break; }
        b = op == MXOP_I2F_reg ? R20 : op == MXOP_I2F_cbuf ? CB(e, i) : I20(e, i);
        const char *v;
        if (isz == 0) v = sgn ? S(e, "bitfieldExtract(int(%s), %u, 8)", b, sel * 8) : S(e, "bitfieldExtract(%s, %u, 8)", b, sel * 8);
        else if (isz == 1) v = sgn ? S(e, "bitfieldExtract(int(%s), %u, 16)", b, sel * 8) : S(e, "bitfieldExtract(%s, %u, 16)", b, sel * 8);
        else v = sgn ? S(e, "int(%s)", b) : b;
        if (MXB(i, 49, 1) && sgn) v = S(e, "abs(%s)", v);
        emit(e, "float f = float(%s);", v);
        if (MXB(i, 45, 1)) emit(e, " f = -f;");
        if (fsz == 1) emit(e, " %s = packHalf2x16(vec2(f, 0.0));", DST); else emit(e, " %s = U(f);", DST);
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " cz = f == 0.0; cs = f < 0.0; ccf = false; cov = false;"); }
        break;
    }
    case MXOP_I2I_reg: case MXOP_I2I_cbuf: case MXOP_I2I_imm: {
        static const unsigned wd[4] = {8, 16, 32, 0};
        unsigned dw = wd[MXB(i, 8, 2)], sw = wd[MXB(i, 10, 2)], ds = MXB(i, 12, 1), ss = MXB(i, 13, 1), sel = MXB(i, 41, 3);
        if (!dw || !sw) { fail(e, "I2I", "I2I width"); break; }
        b = op == MXOP_I2I_reg ? R20 : op == MXOP_I2I_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "int v = %s;", sw == 32 ? S(e, "int(%s)", b) : ss ? S(e, "bitfieldExtract(int(%s), %u, %u)", b, sel * 8, sw) : S(e, "int(bitfieldExtract(%s, %u, %u))", b, sel * 8, sw));
        if (MXB(i, 49, 1)) emit(e, " v = abs(v);");
        if (MXB(i, 45, 1)) emit(e, " v = -v;");
        if (MXB(i, 50, 1)) {
            unsigned max = ds ? (dw == 32 ? 0x7fffffffu : (1u << (dw - 1)) - 1) : (dw == 32 ? 0xffffffffu : (1u << dw) - 1), min = ds && ss ? (dw == 32 ? 0x80000000u : 0u - (1u << (dw - 1))) : 0;
            if (ds && ss) emit(e, " uint res = uint(clamp(v, int(0x%08xu), int(0x%08xu)));", min, max);
            else emit(e, " uint res = clamp(%s, 0x%08xu, 0x%08xu);", ss ? "uint(max(v, 0))" : "uint(v)", min, max);
        } else emit(e, " uint res = %s;", dw == 32 ? "uint(v)" : S(e, "bitfieldExtract(uint(v), 0, %u)", dw));
        emit(e, " %s = res;", DST);
        if (MXB(i, 47, 1)) setcc_int(e, "res");
        break;
    }

    /* ---- integer arithmetic */
    case MXOP_IADD_reg: case MXOP_IADD_cbuf: case MXOP_IADD_imm: case MXOP_IADD32I: {
        int is32 = op == MXOP_IADD32I, po = is32 ? MXB(i, 55, 2) == 3 : MXB(i, 48, 2) == 3, x = (int)(is32 ? MXB(i, 53, 1) : MXB(i, 43, 1)), cc = (int)(is32 ? MXB(i, 52, 1) : MXB(i, 47, 1));
        int nega = !po && (is32 ? MXB(i, 56, 1) : MXB(i, 49, 1)), negb = !is32 && !po && MXB(i, 48, 1);
        if (is32 ? MXB(i, 54, 1) : MXB(i, 50, 1)) { fail(e, "IADD.SAT", "saturating IADD"); break; }
        b = op == MXOP_IADD_reg ? R20 : op == MXOP_IADD_cbuf ? CB(e, i) : op == MXOP_IADD_imm ? I20(e, i) : I32(e, i);
        const char *a = nega ? S(e, "(0u - %s)", A8) : A8, *bb = negb ? S(e, "(0u - %s)", b) : b, *ci = x ? "uint(ccf)" : "0u";
        if (x) e->uses_cc = 1;
        if (po) ci = x ? "(uint(ccf) + 1u)" : "1u";
        if (cc) emit(e, "%s = mx_addc(%s, %s, %s);", DST, a, bb, ci);
        else emit(e, "%s = %s + %s%s%s;", DST, a, bb, strcmp(ci, "0u") ? " + " : "", strcmp(ci, "0u") ? ci : "");
        if (cc) e->uses_cc = 1;
        break;
    }
    case MXOP_IADD3_reg: case MXOP_IADD3_cbuf: case MXOP_IADD3_imm: {
        const char *a = A8, *c3 = R39;
        b = op == MXOP_IADD3_reg ? R20 : op == MXOP_IADD3_cbuf ? CB(e, i) : I20(e, i);
        unsigned shift = 0;
        if (op == MXOP_IADD3_reg) {
            static const char *const half[3] = {"%s", "(%s & 0xffffu)", "(%s >> 16)"};
            if (MXB(i, 35, 2) == 3 || MXB(i, 33, 2) == 3 || MXB(i, 31, 2) == 3 || MXB(i, 37, 2) == 3) { fail(e, "IADD3", "IADD3 half/shift mode 3"); break; }
            a = S(e, half[MXB(i, 35, 2)], a); b = S(e, half[MXB(i, 33, 2)], b); c3 = S(e, half[MXB(i, 31, 2)], c3);
            shift = MXB(i, 37, 2);
        }
        if (MXB(i, 51, 1)) a = S(e, "(0u - %s)", a);
        if (MXB(i, 50, 1)) b = S(e, "(0u - %s)", b);
        if (MXB(i, 49, 1)) c3 = S(e, "(0u - %s)", c3);
        if (MXB(i, 48, 1)) { e->uses_cc = 1; emit(e, "uint l1 = %s + %s + uint(ccf);", a, b); } else emit(e, "uint l1 = %s + %s;", a, b);
        if (shift == 1) emit(e, " l1 = l1 >> 16;");                 /* suyu also models a 33rd bit here; ignored */
        else if (shift == 2) emit(e, " l1 = l1 << 16;");
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " %s = mx_addc(l1, %s, 0u);", DST, c3); }
        else emit(e, " %s = l1 + %s;", DST, c3);
        break;
    }
    case MXOP_ISCADD_reg: case MXOP_ISCADD_cbuf: case MXOP_ISCADD_imm: case MXOP_ISCADD32I: {
        int is32 = op == MXOP_ISCADD32I, nega = !is32 && MXB(i, 49, 1), negb = !is32 && MXB(i, 48, 1), po = nega && negb;
        unsigned sh = is32 ? MXB(i, 53, 5) : MXB(i, 39, 5);
        b = op == MXOP_ISCADD_reg ? R20 : op == MXOP_ISCADD_cbuf ? CB(e, i) : op == MXOP_ISCADD_imm ? I20(e, i) : I32(e, i);
        const char *a = A8;
        if (po) b = S(e, "(%s + 1u)", b);
        else { if (nega) a = S(e, "(0u - %s)", a); if (negb) b = S(e, "(0u - %s)", b); }
        if (is32 ? MXB(i, 52, 1) : MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, "%s = mx_addc(%s << %uu, %s, 0u);", DST, a, sh, b); }
        else emit(e, "%s = (%s << %uu) + %s;", DST, a, sh, b);
        break;
    }
    case MXOP_XMAD_reg: case MXOP_XMAD_rc: case MXOP_XMAD_cr: case MXOP_XMAD_imm: {
        unsigned hb, psl, mrg, x, sel;
        const char *sb, *sc;
        switch (op) {
        case MXOP_XMAD_reg: sb = R20; sc = R39; hb = MXB(i, 35, 1); psl = MXB(i, 36, 1); mrg = MXB(i, 37, 1); x = MXB(i, 38, 1); sel = MXB(i, 50, 3); break;
        case MXOP_XMAD_rc: sb = R39; sc = CB(e, i); hb = MXB(i, 52, 1); psl = 0; mrg = 0; x = MXB(i, 54, 1); sel = MXB(i, 50, 2); break;
        case MXOP_XMAD_cr: sb = CB(e, i); sc = R39; hb = MXB(i, 52, 1); psl = MXB(i, 55, 1); mrg = MXB(i, 56, 1); x = MXB(i, 54, 1); sel = MXB(i, 50, 2); break;
        default: sb = S(e, "0x%xu", MXB(i, 20, 16)); sc = R39; hb = 0; psl = MXB(i, 36, 1); mrg = MXB(i, 37, 1); x = MXB(i, 38, 1); sel = MXB(i, 50, 3); break;
        }
        unsigned sa = MXB(i, 48, 1), sbs = MXB(i, 49, 1), ha = MXB(i, 53, 1);
        const char *oa = sa ? S(e, "uint(bitfieldExtract(int(%s), %u, 16))", A8, ha * 16) : S(e, "bitfieldExtract(%s, %u, 16)", A8, ha * 16);
        const char *ob = sbs ? S(e, "uint(bitfieldExtract(int(%s), %u, 16))", sb, hb * 16) : S(e, "bitfieldExtract(%s, %u, 16)", sb, hb * 16);
        const char *ocv;
        switch (sel) {
        case 0: ocv = sc; break;
        case 1: ocv = S(e, "(%s & 0xffffu)", sc); break;
        case 2: ocv = S(e, "(%s >> 16)", sc); break;
        case 4: ocv = S(e, "((%s << 16) + %s)", sb, sc); break;
        default: ocv = NULL; break;
        }
        if (!ocv) { fail(e, "XMAD.CSFU", "XMAD select mode %u", sel); break; }
        emit(e, "uint prod = %s * %s%s;", oa, ob, psl ? " << 16" : "");
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " uint res = mx_addc(prod, %s, %s);", ocv, x ? "uint(ccf)" : "0u"); }
        else emit(e, " uint res = prod + %s%s;", ocv, x ? " + uint(ccf)" : "");
        if (x) e->uses_cc = 1;
        if (mrg) emit(e, " res = (res & 0xffffu) | (%s << 16);", sb);
        emit(e, " %s = res;", DST);
        break;
    }
    case MXOP_IMNMX_reg: case MXOP_IMNMX_cbuf: case MXOP_IMNMX_imm: {
        if (MXB(i, 43, 2)) { fail(e, "IMNMX.XLO", "IMNMX mode %u", MXB(i, 43, 2)); break; }
        b = op == MXOP_IMNMX_reg ? R20 : op == MXOP_IMNMX_cbuf ? CB(e, i) : I20(e, i);
        const char *p = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        if (MXB(i, 48, 1)) emit(e, "uint res = uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)));", p, A8, b, A8, b);
        else emit(e, "uint res = %s ? min(%s, %s) : max(%s, %s);", p, A8, b, A8, b);
        emit(e, " %s = res;", DST);
        if (MXB(i, 47, 1)) setcc_int(e, "res");
        break;
    }
    case MXOP_ISET_reg: case MXOP_ISET_cbuf: case MXOP_ISET_imm: {
        b = op == MXOP_ISET_reg ? R20 : op == MXOP_ISET_cbuf ? CB(e, i) : I20(e, i);
        const char *cmp = MXB(i, 43, 1) ? ICMPX(e, MXB(i, 49, 3), (int)MXB(i, 48, 1), A8, b) : ICMP(e, MXB(i, 49, 3), (int)MXB(i, 48, 1), A8, b);
        emit(e, "uint res = %s ? %s : 0u; %s = res;", BOP(e, cmp, PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1)), MXB(i, 45, 2)), MXB(i, 44, 1) ? "0x3f800000u" : "0xffffffffu", DST);
        if (MXB(i, 47, 1)) { e->uses_cc = 1; emit(e, " cz = res == 0u; cs = %s; ccf = false; cov = false;", MXB(i, 44, 1) ? "false" : "res != 0u"); }
        break;
    }
    case MXOP_ISETP_reg: case MXOP_ISETP_cbuf: case MXOP_ISETP_imm: {
        b = op == MXOP_ISETP_reg ? R20 : op == MXOP_ISETP_cbuf ? CB(e, i) : I20(e, i);
        const char *cmp = MXB(i, 43, 1) ? ICMPX(e, MXB(i, 49, 3), (int)MXB(i, 48, 1), A8, b) : ICMP(e, MXB(i, 49, 3), (int)MXB(i, 48, 1), A8, b), *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "bool cmp = %s;", cmp);
        if (PD(e, MXB(i, 3, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 3, 3)), BOP(e, "cmp", bp, MXB(i, 45, 2)));
        if (PD(e, MXB(i, 0, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 0, 3)), BOP(e, "!cmp", bp, MXB(i, 45, 2)));
        break;
    }
    case MXOP_VMAD: case MXOP_VSETP: {
        /* video instructions (suyu video_multiply_add.cpp, video_set_predicate.cpp): operand A from register 8, B from register
         * 20 (bit 50) or a 16-bit immediate; each operand's selector (bits 36-37 / 28-29) and width (bits 37-38 / 29-30) share
         * a bit. SMO: only the lightning geometry stages use them (the input vertex count of the invocation info register). */
        int sa = (int)MXB(i, 48, 1), sbs = (int)MXB(i, 49, 1);
        const char *va = VOP(e, A8, MXB(i, 36, 2), MXB(i, 37, 2), sa), *vb;
        if (MXB(i, 50, 1)) vb = VOP(e, R20, MXB(i, 28, 2), MXB(i, 29, 2), sbs);
        else if (op == MXOP_VSETP && MXB(i, 28, 2)) { fail(e, "VSETP immediate", "VSETP with an immediate whose bits 8-9 are set (suyu reads them as a selector)"); break; }
        else vb = VOP(e, S(e, "0x%xu", MXB(i, 20, 16)), 0, 2, sbs);
        if (op == MXOP_VMAD) {
            if (MXB(i, 47, 1) || MXB(i, 51, 2) || MXB(i, 53, 3)) { fail(e, "VMAD.CC/SCALE/NEG/SAT", "VMAD with CC, a scale, a negation or SAT"); break; }
            emit(e, "%s = %s * %s + %s;", DST, va, vb, R39);
        } else {
            /* compare 0 F, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 T from bits 43-44 and 47 (bits 45-46 are the boolean operation);
             * signed when B is */
            unsigned cop = MXB(i, 43, 2) | MXB(i, 47, 1) << 2;
            const char *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
            emit(e, "bool cmp = %s;", ICMP(e, cop, sbs, va, vb));
            if (PD(e, MXB(i, 3, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 3, 3)), BOP(e, "cmp", bp, MXB(i, 45, 2)));
            if (PD(e, MXB(i, 0, 3))) emit(e, " %s = %s;", PD(e, MXB(i, 0, 3)), BOP(e, "!cmp", bp, MXB(i, 45, 2)));
        }
        break;
    }
    case MXOP_ICMP_reg: case MXOP_ICMP_rc: case MXOP_ICMP_cr: case MXOP_ICMP_imm: {
        const char *sa, *operand;
        switch (op) {
        case MXOP_ICMP_reg: sa = R20; operand = R39; break;
        case MXOP_ICMP_rc: sa = R39; operand = CB(e, i); break;
        case MXOP_ICMP_cr: sa = CB(e, i); operand = R39; break;
        default: sa = I20(e, i); operand = R39; break;
        }
        emit(e, "%s = %s ? %s : %s;", DST, ICMP(e, MXB(i, 49, 3), (int)MXB(i, 48, 1), operand, "0u"), A8, sa);
        break;
    }
    case MXOP_SHL_reg: case MXOP_SHL_cbuf: case MXOP_SHL_imm:
        if (MXB(i, 43, 1) || MXB(i, 47, 1)) { fail(e, "SHL.X/CC", "SHL with X or CC"); break; }
        b = op == MXOP_SHL_reg ? R20 : op == MXOP_SHL_cbuf ? CB(e, i) : I20(e, i);
        if (MXB(i, 39, 1)) emit(e, "%s = %s << (%s & 31u);", DST, A8, b);
        else emit(e, "uint sh = %s; %s = sh >= 32u ? 0u : %s << sh;", b, DST, A8);
        break;
    case MXOP_SHR_reg: case MXOP_SHR_cbuf: case MXOP_SHR_imm: {
        if (MXB(i, 43, 1) || MXB(i, 47, 1)) { fail(e, "SHR.X/CC", "SHR with X or CC"); break; }
        b = op == MXOP_SHR_reg ? R20 : op == MXOP_SHR_cbuf ? CB(e, i) : I20(e, i);
        const char *base = MXB(i, 40, 1) ? S(e, "bitfieldReverse(%s)", A8) : A8;
        if (MXB(i, 48, 1)) emit(e, "uint sh = %s; %s = uint(int(%s) >> int(%s));", b, DST, base, MXB(i, 39, 1) ? "sh & 31u" : "min(sh, 31u)");
        else emit(e, "uint sh = %s; %s = %s;", b, DST, MXB(i, 39, 1) ? S(e, "%s >> (sh & 31u)", base) : S(e, "sh >= 32u ? 0u : %s >> sh", base));
        break;
    }
    case MXOP_SHF_l_reg: case MXOP_SHF_l_imm: case MXOP_SHF_r_reg: case MXOP_SHF_r_imm: {
        int right = op == MXOP_SHF_r_reg || op == MXOP_SHF_r_imm;
        unsigned ms = MXB(i, 37, 2);
        if (MXB(i, 47, 1) || MXB(i, 48, 2) || ms == 1) { fail(e, "SHF", "SHF with CC/X or undefined max shift"); break; }
        b = (op == MXOP_SHF_l_reg || op == MXOP_SHF_r_reg) ? R20 : I20(e, i);
        emit(e, "uint sh = %s; sh = %s; uint64_t v = packUint2x32(uvec2(%s, %s));", b, MXB(i, 50, 1) ? (ms == 0 ? "sh & 31u" : "sh & 63u") : (ms == 0 ? "min(sh, 32u)" : "min(sh, 63u)"), A8, R39);
        if (!right) emit(e, " v = sh >= 64u ? 0ul : v << sh; %s = unpackUint2x32(v).y;", DST);
        else if (ms == 3) emit(e, " v = uint64_t(int64_t(v) >> sh); %s = unpackUint2x32(v).x;", DST);
        else emit(e, " v = sh >= 64u ? 0ul : v >> sh; %s = unpackUint2x32(v).x;", DST);
        break;
    }
    case MXOP_BFE_reg: case MXOP_BFE_cbuf: case MXOP_BFE_imm: {
        b = op == MXOP_BFE_reg ? R20 : op == MXOP_BFE_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "uint res = mx_bfe(%s, %s, %s); %s = res;", MXB(i, 40, 1) ? S(e, "bitfieldReverse(%s)", A8) : A8, b, MXB(i, 48, 1) ? "true" : "false", DST);
        if (MXB(i, 47, 1)) setcc_int(e, "res");
        break;
    }
    case MXOP_BFI_reg: case MXOP_BFI_rc: case MXOP_BFI_cr: case MXOP_BFI_imm: {
        const char *ctl, *base;
        switch (op) {
        case MXOP_BFI_reg: ctl = R20; base = R39; break;
        case MXOP_BFI_rc: ctl = R39; base = CB(e, i); break;
        case MXOP_BFI_cr: ctl = CB(e, i); base = R39; break;
        default: ctl = I20(e, i); base = R39; break;
        }
        emit(e, "uint res = mx_bfi(%s, %s, %s); %s = res;", base, A8, ctl, DST);
        if (MXB(i, 47, 1)) setcc_int(e, "res");
        break;
    }
    case MXOP_LOP_reg: case MXOP_LOP_cbuf: case MXOP_LOP_imm: case MXOP_LOP32I: {
        static const char *const lop[3] = {"&", "|", "^"};
        int is32 = op == MXOP_LOP32I;
        unsigned o = is32 ? MXB(i, 53, 2) : MXB(i, 41, 2);
        if (is32 ? MXB(i, 57, 1) : MXB(i, 43, 1)) { fail(e, "LOP.X", "LOP with X"); break; }
        b = op == MXOP_LOP_reg ? R20 : op == MXOP_LOP_cbuf ? CB(e, i) : op == MXOP_LOP_imm ? I20(e, i) : I32(e, i);
        const char *a = (is32 ? MXB(i, 55, 1) : MXB(i, 39, 1)) ? S(e, "~%s", A8) : A8, *bb = (is32 ? MXB(i, 56, 1) : MXB(i, 40, 1)) ? S(e, "~%s", b) : b;
        emit(e, "uint res = %s; %s = res;", o == 3 ? bb : S(e, "%s %s %s", a, lop[o], bb), DST);
        if (!is32 && PD(e, MXB(i, 48, 3))) {
            unsigned po = MXB(i, 44, 2);
            emit(e, " %s = %s;", PD(e, MXB(i, 48, 3)), po == 0 ? "false" : po == 1 ? "true" : po == 2 ? "res == 0u" : "res != 0u");
        }
        if (is32 ? MXB(i, 52, 1) : MXB(i, 47, 1)) setcc_int(e, "res");
        break;
    }
    case MXOP_LOP3_reg: case MXOP_LOP3_cbuf: case MXOP_LOP3_imm: {
        unsigned lut = op == MXOP_LOP3_reg ? MXB(i, 28, 8) : MXB(i, 48, 8);
        if ((op == MXOP_LOP3_reg && MXB(i, 38, 1)) || MXB(i, 47, 1)) { fail(e, "LOP3.X/CC", "LOP3 with X or CC"); break; }
        b = op == MXOP_LOP3_reg ? R20 : op == MXOP_LOP3_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "uint la = %s, lb = %s, lc = %s, res = 0u;", A8, b, R39);
        for (unsigned k = 0; k < 8; k++)
            if (lut >> k & 1) emit(e, " res |= %sla & %slb & %slc;", (0xF0 >> k & 1) ? "" : "~", (0xCC >> k & 1) ? "" : "~", (0xAA >> k & 1) ? "" : "~");
        emit(e, " %s = res;", DST);
        if (op == MXOP_LOP3_reg && PD(e, MXB(i, 48, 3))) {
            unsigned po = MXB(i, 36, 2);
            emit(e, " %s = %s;", PD(e, MXB(i, 48, 3)), po == 0 ? "false" : po == 1 ? "true" : po == 2 ? "res == 0u" : "res != 0u");
        }
        break;
    }
    case MXOP_POPC_reg: case MXOP_POPC_cbuf: case MXOP_POPC_imm:
        b = op == MXOP_POPC_reg ? R20 : op == MXOP_POPC_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "%s = uint(bitCount(%s%s));", DST, MXB(i, 40, 1) ? "~" : "", b);
        break;
    case MXOP_FLO_reg: case MXOP_FLO_cbuf: case MXOP_FLO_imm:
        if (MXB(i, 47, 1)) { fail(e, "FLO.CC", "FLO with CC"); break; }
        b = op == MXOP_FLO_reg ? R20 : op == MXOP_FLO_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "uint res = uint(findMSB(%s(%s%s)));", MXB(i, 48, 1) ? "int" : "uint", MXB(i, 40, 1) ? "~" : "", b);
        if (MXB(i, 41, 1)) emit(e, " if (res != 0xffffffffu) res ^= 31u;");
        emit(e, " %s = res;", DST);
        break;

    /* ---- memory and attributes */
    case MXOP_LDC: {
        unsigned cb = MXB(i, 36, 5), sz = MXB(i, 48, 3), mode = MXB(i, 44, 2), rd = MXB(i, 0, 8);
        int off = (int)(int16_t)MXB(i, 20, 16);
        if (mode != 0) { fail(e, "LDC.IS", "LDC mode %u", mode); break; }
        e->cbufs |= 1u << cb;
        emit(e, "uint ad = %s + 0x%08xu, w = c%u[ad >> 2];", A8, (uint32_t)off, cb);
        switch (sz) {
        case 0: emit(e, " %s = bitfieldExtract(w, int((ad & 3u) * 8u), 8);", DST); break;
        case 1: emit(e, " %s = uint(bitfieldExtract(int(w), int((ad & 3u) * 8u), 8));", DST); break;
        case 2: emit(e, " %s = bitfieldExtract(w, int((ad & 2u) * 8u), 16);", DST); break;
        case 3: emit(e, " %s = uint(bitfieldExtract(int(w), int((ad & 2u) * 8u), 16));", DST); break;
        case 4: emit(e, " %s = w;", DST); break;
        case 5: emit(e, " %s = w; %s = c%u[(ad >> 2) + 1u];", DST, D(e, rd == 255 ? 255 : rd + 1), cb); break;
        default: fail(e, "LDC", "LDC size %u", sz); break;
        }
        break;
    }
    case MXOP_ALD: {
        unsigned a = MXB(i, 20, 10), n = MXB(i, 47, 2) + 1, rd = MXB(i, 0, 8);
        if (MXB(i, 8, 8) != 255) { fail(e, "ALD.indexed", "indexed attribute load"); break; }
        if (MXB(i, 31, 1)) { fail(e, "ALD.P", "patch attribute load"); break; }
        if (e->in->stage == MX_TESS_CONTROL || e->in->stage == MX_TESS_EVAL) { fail(e, "ALD (vertex array)", "attribute load in stage %d", e->in->stage); break; }
        if (e->in->stage == MX_GEOMETRY && MXB(i, 32, 1)) { fail(e, "ALD.O", "output read in a geometry stage"); break; }
        /* in a geometry stage the input vertex is the register at bits 39-46 ([doc] yuzu load_store_attribute.cpp ALD) */
        const char *vtx = e->in->stage == MX_GEOMETRY ? (MXB(i, 39, 8) == 255 ? "0u" : X(e, MXB(i, 39, 8))) : NULL;
        for (unsigned k = 0; k < n; k++) {
            unsigned ad = a + k * 4;
            const char *v = MXB(i, 32, 1) && e->in->stage == MX_VERTEX && ad >= 0x80 && ad < 0x280 ? S(e, "U(out_attr%u[%u])", (ad - 0x80) >> 4, (ad >> 2) & 3) : attr_in(e, ad, vtx);
            if (rd != 255) emit(e, " %s = %s;", D(e, rd + k), v);
        }
        break;
    }
    case MXOP_AST: {
        unsigned a = MXB(i, 20, 10), n = MXB(i, 47, 2) + 1, rs = MXB(i, 0, 8);
        if (MXB(i, 8, 8) != 255) { fail(e, "AST.indexed", "indexed attribute store"); break; }
        if (MXB(i, 31, 1)) { fail(e, "AST.P", "patch attribute store"); break; }
        for (unsigned k = 0; k < n; k++) attr_out(e, a + k * 4, X(e, rs == 255 ? 255 : rs + k));
        break;
    }
    case MXOP_SHFL: {                                              /* warp shuffle on a Vulkan subgroup (NVIDIA: 32 lanes, as a Maxwell warp) */
        /* fields (decoded from SMO's own SHFL.DOWN r10, r8, 1, 0x1f; [doc] yuzu warp_shuffle.cpp): mode bits 30-31 (0 IDX, 1 UP,
         * 2 DOWN, 3 BFLY), lane operand b = imm bits 20-24 when bit 28 is set else register bits 20-27, clamp/segment
         * operand c = imm bits 34-46 when bit 29 is set else register bits 39-46, predicate out bits 48-50. Lane rules as
         * PTX shfl: maxLane = (lane & seg) | (c & ~seg), minLane = lane & seg, seg = c bits 8-12; out of range: own value. */
        if (e->in->target != 1) { fail(e, "SHFL", "warp shuffle in the OpenGL text"); break; }
        unsigned mode = MXB(i, 30, 2), pd = MXB(i, 48, 3);
        const char *b = MXB(i, 28, 1) ? S(e, "%uu", MXB(i, 20, 5)) : R20, *cc = MXB(i, 29, 1) ? S(e, "%uu", MXB(i, 34, 13)) : X(e, MXB(i, 39, 8));
        e->uses_shfl = 1;
        emit(e, "uint ln = gl_SubgroupInvocationID, bv = (%s) & 31u, cv = (%s) & 31u, sg = ((%s) >> 8) & 31u, mx = (ln & sg) | (cv & ~sg), j; bool pv;", b, cc, cc);
        if (mode == 0) emit(e, " j = (ln & sg) | (bv & ~sg); pv = j <= mx;");
        else if (mode == 1) emit(e, " j = ln - bv; pv = int(j) >= int(mx);");
        else if (mode == 2) emit(e, " j = ln + bv; pv = j <= mx;");
        else emit(e, " j = ln ^ bv; pv = j <= mx;");
        emit(e, " if (!pv) j = ln; uint sv = subgroupShuffle(%s, j);", A8);
        if (MXB(i, 0, 8) != 255) emit(e, " %s = sv;", DST);
        if (pd != 7) emit(e, " %s = pv;", PD(e, pd));
        break;
    }
    case MXOP_ISBERD: {                                            /* geometry stage, the attribute buffer entry of an input vertex */
        /* [doc] yuzu internal_stage_buffer_entry_read.cpp: the value passes through (the input vertex number ALD then takes);
         * skew bit 31, O bit 32, mode bits 33-34 and shift bits 47-48 are not supported, as there */
        if (MXB(i, 31, 2) || MXB(i, 33, 2) || MXB(i, 47, 2)) { fail(e, "ISBERD", "ISBERD form %016llx", (unsigned long long)i); break; }
        emit(e, "%s = %s;", DST, A8);
        break;
    }
    case MXOP_OUT_reg: case MXOP_OUT_imm: case MXOP_OUT_cbuf: {    /* geometry stage, emit the vertex and / or end the primitive */
        /* [doc] yuzu output_geometry.cpp: emit bit 39, cut bit 40, stream in Rb / the immediate / the constant (only stream 0
         * here: a stream other than 0 needs a stream-out the renderer does not have); Rd gets 0 */
        if (e->in->stage != MX_GEOMETRY) { fail(e, "OUT", "OUT outside a geometry stage"); break; }
        if (op == MXOP_OUT_imm && MXB(i, 20, 19)) { fail(e, "OUT stream", "OUT to stream %u", MXB(i, 20, 19)); break; }
        if (MXB(i, 39, 1)) emit(e, " MX_GS_FILL EmitVertex();");
        if (MXB(i, 40, 1)) emit(e, " EndPrimitive();");
        if (MXB(i, 0, 8) != 255) emit(e, " %s = 0u;", DST);
        break;
    }
    case MXOP_FSWZADD: {                                           /* swizzled add, one of four forms per lane of the 2x2 quad */
        /* [doc] yuzu floating_point_swizzled_add.cpp / its GLSL backend: d = a * A[k] + b * B[k], k = (swizzle >> (2 * quad lane)) & 3,
         * A = {1, -1, 1, 0}, B = {1, 1, -1, 1} (a + b, b - a, a - b, b); swizzle bits 28-35, NDV bit 38 (not handled, as yuzu),
         * CC bit 47; the quad lane is gl_SubgroupInvocationID & 3 (Vulkan's quad of a fragment shader). Found offline in one of
         * SMO's fragment stages (tests/check_all_shaders.py). */
        if (e->in->target != 1) { fail(e, "FSWZADD", "quad swizzle in the OpenGL text"); break; }
        if (MXB(i, 38, 1)) { fail(e, "FSWZADD", "FSWZADD.NDV"); break; }
        unsigned sw = MXB(i, 28, 8);
        e->uses_shfl = 1;                                          /* gl_SubgroupInvocationID */
        fres(e, i, S(e, "F(%s) * float[4](1.0, -1.0, 1.0, 0.0)[(%uu >> ((gl_SubgroupInvocationID & 3u) << 1)) & 3u] + F(%s) * float[4](1.0, 1.0, -1.0, 1.0)[(%uu >> ((gl_SubgroupInvocationID & 3u) << 1)) & 3u]",
                         A8, sw, R20, sw), 0, MXB(i, 47, 1));
        break;
    }
    case MXOP_AL2P: {                                              /* attribute address = Ra + signed 11-bit offset ([doc] yuzu attribute_logical_to_physical: an add) */
        int off = (int)(MXB(i, 20, 11) << 21) >> 21;
        emit(e, "%s = %s + 0x%08xu;", DST, A8, (uint32_t)off);
        break;
    }
    case MXOP_IPA: {
        unsigned a = MXB(i, 30, 8) * 4, mode = MXB(i, 54, 2);
        if (e->in->stage != MX_FRAGMENT) { fail(e, "IPA", "IPA outside a fragment shader"); break; }
        if (MXB(i, 38, 1) && MXB(i, 8, 8) != 255) {
            /* indexed IPA, the address in Ra + the immediate (its Ra comes from AL2P). The inputs that can be read are
             * the ones the program header declares (an interpolation mode at +0x18 + n); each is selected at run time and
             * gets the same perspective handling as a fixed IPA ([doc] yuzu ipa.cpp: indexed reads of generic attributes). */
            emit(e, "uint ad = %s + %uu; float f = 0.0;", A8, a);
            for (unsigned n = 0; n < 32; n++) {
                unsigned m = e->in->code[0x18 + n], eff = 0;
                if (!m) continue;
                for (int k = 0; k < 4; k++) if ((m >> (k * 2)) & 3) eff = (m >> (k * 2)) & 3;
                e->in_attr |= 1u << n; e->in_comp[n] |= (uint8_t)m ? 15 : 0;
                emit(e, " if ((ad >> 4) == %uu) f = in_attr%u[(ad >> 2) & 3u]%s;", (0x80 >> 4) + n, n, (eff == 2 || eff == 0) ? " * gl_FragCoord.w" : "");
            }
            if (mode == 1) emit(e, " f *= F(%s);", R20);
            if (MXB(i, 51, 1)) emit(e, " f = clamp(f, 0.0, 1.0);");
            emit(e, " %s = U(f);", DST);
            break;
        }
        const char *v = attr_in(e, a, 0);
        emit(e, "float f = F(%s);", v);
        if (a >= 0x80 && a < 0x280) {
            unsigned n = (a - 0x80) >> 4, m = e->in->code[0x18 + n], eff = 0;
            for (int k = 0; k < 4; k++) if ((m >> (k * 2)) & 3) eff = (m >> (k * 2)) & 3;
            if (eff == 2 || eff == 0) emit(e, " f *= gl_FragCoord.w;");      /* hardware IPA returns attribute / w; the shader multiplies by w */
        }
        if (mode == 1) emit(e, " f *= F(%s);", R20);
        if (MXB(i, 51, 1)) emit(e, " f = clamp(f, 0.0, 1.0);");
        emit(e, " %s = U(f);", DST);
        break;
    }
    case MXOP_LDL: case MXOP_STL: case MXOP_LDS: case MXOP_STS: {       /* local memory (per invocation) and shared memory (per work group) */
        unsigned sz = MXB(i, 48, 3), r = MXB(i, 0, 8);
        int rel = (int)(MXB(i, 20, 24) << 8) >> 8, shared = op == MXOP_LDS || op == MXOP_STS, ld = op == MXOP_LDL || op == MXOP_LDS;
        unsigned words = shared ? e->smem_words : e->lmem_words;
        const char *arr = shared ? "smem" : "lmem";
        if (shared && e->in->stage != MX_COMPUTE) { fail(e, mx_op_name[op], "shared memory outside a compute shader"); break; }
        if (!words) { fail(e, mx_op_name[op], "%s memory access but the program declares none", shared ? "shared" : "local"); break; }
        if (shared) e->uses_smem = 1; else e->uses_lmem = 1;
        const char *ad = MXB(i, 8, 8) == 255 ? S(e, "0x%08xu", MXB(i, 20, 24)) : S(e, "(%s + 0x%08xu)", A8, (uint32_t)rel);
        emit(e, "uint ad = %s;", ad);
        if (sz < 4) {                                              /* 8 / 16 bits, zero or sign extended; a store replaces only those bits */
            unsigned bits = sz < 2 ? 8 : 16;
            const char *sh = sz < 2 ? "int((ad & 3u) * 8u)" : "int((ad & 2u) * 8u)", *idx = S(e, "min(ad >> 2, %uu)", words - 1);
            if (ld) {
                if (r != 255) emit(e, " uint w = ad < %uu ? %s[%s] : 0u; %s = %s;", words * 4, arr, idx, D(e, r),
                                   sz & 1 ? S(e, "uint(bitfieldExtract(int(w), %s, %u))", sh, bits) : S(e, "bitfieldExtract(w, %s, %u)", sh, bits));
            } else emit(e, " if (ad < %uu) %s[%s] = bitfieldInsert(%s[%s], %s, %s, %u);", words * 4, arr, idx, arr, idx, X(e, r), sh, bits);
            break;
        }
        unsigned n = sz == 4 ? 1 : sz == 5 ? 2 : sz == 6 ? 4 : 0;
        if (!n) { fail(e, mx_op_name[op], "size %u", sz); break; }
        for (unsigned k = 0; k < n; k++) {
            const char *idx = S(e, "min((ad >> 2) + %uu, %uu)", k, words - 1);
            if (ld) { if (r != 255) emit(e, " %s = ad + %uu < %uu ? %s[%s] : 0u;", D(e, r + k), k * 4, words * 4, arr, idx); }
            else emit(e, " if (ad + %uu < %uu) %s[%s] = %s;", k * 4, words * 4, arr, idx, X(e, r == 255 ? 255 : r + k));
        }
        break;
    }
    case MXOP_LEA_lo_reg: case MXOP_LEA_lo_cbuf: case MXOP_LEA_lo_imm: {         /* base + (+-offset << scale) */
        if (MXB(i, 46, 1) || MXB(i, 47, 1) || MXB(i, 48, 3) != 7) { fail(e, "LEA.LO X/CC/P", "LEA.LO with extend, condition code or predicate output"); break; }
        b = op == MXOP_LEA_lo_reg ? R20 : op == MXOP_LEA_lo_cbuf ? CB(e, i) : I20(e, i);
        emit(e, "%s = %s + ((%s%s) << %uu);", DST, b, MXB(i, 45, 1) ? "-" : "", A8, MXB(i, 39, 5));
        break;
    }
    case MXOP_LEA_hi_reg: case MXOP_LEA_hi_cbuf: {          /* base + low word of ((+-(hi:lo)) >> (32 - scale)) */
        int cbuf = op == MXOP_LEA_hi_cbuf;
        unsigned scale = cbuf ? MXB(i, 51, 5) : MXB(i, 28, 5), neg = cbuf ? MXB(i, 56, 1) : MXB(i, 37, 1), x = cbuf ? MXB(i, 57, 1) : MXB(i, 38, 1);
        if (x || MXB(i, 47, 1) || MXB(i, 48, 3) != 7) { fail(e, "LEA.HI X/CC/P", "LEA.HI with extend, condition code or predicate output"); break; }
        b = cbuf ? CB(e, i) : R20;
        emit(e, "uint64_t lo = packUint2x32(uvec2(%s, %s));%s %s = %s + unpackUint2x32(lo >> %uu).x;", A8, R39, neg ? " lo = ~lo + 1ul;" : "", DST, b, 32 - scale);
        break;
    }
    case MXOP_SULD: case MXOP_SUST: {                       /* image load / store */
        /* type (bits 33..35): 0 1D, 1 buffer, 2 1D array, 3 2D, 4 2D array, 5 3D; .D (bit 52): typed, size in bits 20..22;
         * otherwise .P: the texture's own format, component mask in bits 20..23 */
        static const char *const ity[6] = {"1D", "Buffer", "1DArray", "2D", "2DArray", "3D"};
        static const uint16_t typed_fmt[7] = {0x8232, 0x8231, 0x8234, 0x8233, 0x8236, 0x823C, 0x8D70};   /* R8UI R8I R16UI R16I R32UI RG32UI RGBA32UI */
        unsigned ty = MXB(i, 33, 3), typed = MXB(i, 52, 1), szf = MXB(i, 20, 3), swz = MXB(i, 20, 4), off = MXB(i, 36, 13), r = MXB(i, 0, 8), cr = MXB(i, 8, 8);
        int ld = op == MXOP_SULD;
        const char *nm = ld ? "SULD" : "SUST";
        if (!MXB(i, 51, 1)) { fail(e, ld ? "SULD (bindless)" : "SUST (bindless)", "%s with an image handle in a register", nm); break; }
        if (MXB(i, 49, 2)) { fail(e, nm, "%s with clamp mode %u", nm, MXB(i, 49, 2)); break; }
        if (ty > 5 || (typed && (szf > 6 || MXB(i, 23, 1)))) { fail(e, nm, "%s type %u size %u", nm, ty, szf); break; }
        if (off < 72 || (off & 1) || (off - 72) / 2 >= 8) { fail(e, nm, "image handle at constant buffer word %u", off); break; }
        unsigned k = (off - 72) / 2;
        uint16_t fmt = typed ? typed_fmt[szf] : e->in->image_format[k];
        const char *coords;
        switch (ty) {
        case 0: case 1: coords = IR(e, cr); break;
        case 2: coords = S(e, "ivec2(%s, int(%s & 0xffffu))", IR(e, cr), X(e, cr + 1)); break;
        case 3: coords = S(e, "ivec2(%s, %s)", IR(e, cr), IR(e, cr + 1)); break;
        case 4: coords = S(e, "ivec3(%s, %s, int(%s & 0xffffu))", IR(e, cr), IR(e, cr + 1), X(e, cr + 2)); break;
        default: coords = S(e, "ivec3(%s, %s, %s)", IR(e, cr), IR(e, cr + 1), IR(e, cr + 2)); break;
        }
        if ((e->images >> k & 1) && (e->image_type[k] != ty || e->image_fmt[k] != fmt)) { fail(e, nm, "image %u used with two types or formats", k); break; }
        if (ld && !img_qualifier(fmt, NULL)) { fail(e, "SULD.P", "typeless image load from image %u whose format (0x%x) is not known", k, fmt); break; }
        e->images |= 1u << k; e->image_type[k] = (uint8_t)ty; e->image_fmt[k] = fmt;
        if (ld) e->images_read |= 1u << k; else e->images_written |= 1u << k;
        (void)ity;
        int cls = 0;
        img_qualifier(fmt, &cls);                                 /* 0 float, 1 uint, 2 int */
        if (ld) {
            emit(e, "%svec4 tv = imageLoad(mx_img%u, %s);", cls == 1 ? "u" : cls == 2 ? "i" : "", k, coords);
            if (typed) { unsigned n = szf == 5 ? 2 : szf == 6 ? 4 : 1; for (unsigned q = 0; q < n; q++) if (r != 255) emit(e, " %s = uint(tv[%u]);", D(e, r + q), q); }
            else {
                unsigned rr = r;
                if (!swz) { fail(e, "SULD.P", "SULD with an empty component mask"); break; }
                for (unsigned q = 0; q < 4; q++) { if (!(swz >> q & 1)) continue; if (rr != 255) { emit(e, " %s = %s;", D(e, rr), cls ? S(e, "uint(tv[%u])", q) : S(e, "U(tv[%u])", q)); rr++; } }
            }
        } else {
            const char *v[4] = {"0u", "0u", "0u", "0u"};
            unsigned n = typed ? (szf == 5 ? 2 : szf == 6 ? 4 : 1) : 4;
            if (!typed && swz != 15) { fail(e, "SUST.P (partial)", "typeless image store with component mask %u", swz); break; }
            for (unsigned q = 0; q < n; q++) v[q] = X(e, r == 255 ? 255 : r + q);
            if (cls == 0) emit(e, "imageStore(mx_img%u, %s, vec4(F(%s), F(%s), F(%s), F(%s)));", k, coords, v[0], v[1], v[2], v[3]);
            else emit(e, "imageStore(mx_img%u, %s, %svec4(%s, %s, %s, %s));", k, coords, cls == 1 ? "u" : "i", v[0], v[1], v[2], v[3]);
        }
        break;
    }
    case MXOP_SUATOM: {                                     /* image atomic (two compute stages: an add on a 1D integer image) */
        /* [doc] suyu surface_atomic_operations.cpp: bindless bit 54, operation bits 29..32 (0 add, 1 min, 2 max, 3 inc, 4 dec, 5 and,
         * 6 or, 7 xor, 8 exchange), type bits 33..35 (as SULD), size bits 51..53 (0 U32, 1 S32, 6 SD32 only: a 32-bit integer
         * image), clamp bits 49..50 (0 only), destination 0..7 (the old value), coordinates from register 8..15, operand register
         * 20..27, handle offset bits 36..48. Increment and decrement (wrapping) and the array types are not supported, as there. */
        static const char *const fn[9] = {"imageAtomicAdd", "imageAtomicMin", "imageAtomicMax", NULL, NULL, "imageAtomicAnd", "imageAtomicOr", "imageAtomicXor", "imageAtomicExchange"};
        unsigned aop = MXB(i, 29, 4), ty = MXB(i, 33, 3), sz = MXB(i, 51, 3), off = MXB(i, 36, 13), r = MXB(i, 0, 8), cr = MXB(i, 8, 8);
        if (MXB(i, 54, 1)) { fail(e, "SUATOM (bindless)", "SUATOM with an image handle in a register"); break; }
        if (MXB(i, 49, 2) || aop > 8 || !fn[aop] || (sz != 0 && sz != 1 && sz != 6) || ty == 2 || ty == 4 || ty > 5) { fail(e, "SUATOM", "SUATOM form %016llx", (unsigned long long)i); break; }
        if (off < 72 || (off & 1) || (off - 72) / 2 >= 8) { fail(e, "SUATOM", "image handle at constant buffer word %u", off); break; }
        unsigned k = (off - 72) / 2;
        uint16_t fmt = sz == 1 && (aop == 1 || aop == 2) ? 0x8235 : 0x8236;      /* r32i for a signed minimum / maximum, else r32ui */
        const char *coords = ty == 3 ? S(e, "ivec2(%s, %s)", IR(e, cr), IR(e, cr + 1)) : ty == 5 ? S(e, "ivec3(%s, %s, %s)", IR(e, cr), IR(e, cr + 1), IR(e, cr + 2)) : IR(e, cr);
        if ((e->images >> k & 1) && (e->image_type[k] != ty || e->image_fmt[k] != fmt)) { fail(e, "SUATOM", "image %u used with two types or formats", k); break; }
        e->images |= 1u << k; e->image_type[k] = (uint8_t)ty; e->image_fmt[k] = fmt; e->images_read |= 1u << k; e->images_written |= 1u << k;
        if (fmt == 0x8235) emit(e, "uint av = uint(%s(mx_img%u, %s, int(%s)));", fn[aop], k, coords, X(e, MXB(i, 20, 8)));
        else emit(e, "uint av = %s(mx_img%u, %s, %s);", fn[aop], k, coords, X(e, MXB(i, 20, 8)));
        if (r != 255) emit(e, " %s = av;", DST);
        break;
    }
    case MXOP_LDG: case MXOP_STG: {
        unsigned sz = MXB(i, 48, 3), r = MXB(i, 0, 8);
        int rel = (int)(MXB(i, 20, 24) << 8) >> 8, ld = op == MXOP_LDG;
        if (MXB(i, 8, 8) == 255) { fail(e, ld ? "LDG (absolute)" : "STG (absolute)", "global memory access at an absolute address"); break; }
        e->uses_gmem = 1;
        emit(e, "uint ad = %s + 0x%08xu;", A8, (uint32_t)rel);      /* .E: the high word of the address is not needed (tagged 32-bit addresses) */
        if (sz < 4) {
            static const char *const fm[4] = {"bitfieldExtract(w, int((ad & 3u) * 8u), 8)", "uint(bitfieldExtract(int(w), int((ad & 3u) * 8u), 8))",
                                              "bitfieldExtract(w, int((ad & 2u) * 8u), 16)", "uint(bitfieldExtract(int(w), int((ad & 2u) * 8u), 16))"};
            if (!ld) {                                             /* replaces 8 or 16 bits of the word (not atomic, like the hardware's byte store seen through 32-bit blocks) */
                e->uses_gstore = 1;
                emit(e, " mx_stg(ad, bitfieldInsert(mx_ldg(ad), %s, %s, %u));", X(e, r), sz < 2 ? "int((ad & 3u) * 8u)" : "int((ad & 2u) * 8u)", sz < 2 ? 8u : 16u);
                break;
            }
            emit(e, " uint w = mx_ldg(ad); %s = %s;", DST, fm[sz]);
            break;
        }
        unsigned n = sz == 4 ? 1 : sz == 5 ? 2 : 4;
        for (unsigned k = 0; k < n; k++) {
            if (ld) { if (r != 255) emit(e, " %s = mx_ldg(ad + %uu);", D(e, r + k), k * 4); }
            else { e->uses_gstore = 1; emit(e, " mx_stg(ad + %uu, %s);", k * 4, X(e, r == 255 ? 255 : r + k)); }
        }
        break;
    }
    case MXOP_KIL:
        if (e->in->stage != MX_FRAGMENT) { fail(e, "KIL", "KIL outside a fragment shader"); break; }
        emit(e, "if (%s) discard;", FLOW(e, MXB(i, 0, 5)));
        break;

    /* ---- textures */
    case MXOP_TEX: op_tex(e, i); break;
    case MXOP_TEX_b: op_tex_b(e, pc, i); break;
    case MXOP_TEXS: op_texs(e, i); break;
    case MXOP_TLD: op_tld(e, i); break;
    case MXOP_TLDS: op_tlds(e, i); break;
    case MXOP_TLD4: op_tld4(e, i); break;
    case MXOP_TLD4S: op_tld4s(e, i); break;
    case MXOP_TMML: op_tmml(e, i); break;
    case MXOP_TMML_b: op_tmml_b(e, pc, i); break;
    case MXOP_TXD: op_txd(e, i); break;
    case MXOP_TXQ: op_txq(e, i); break;

    /* ---- half floats (pairs in one register) */
    case MXOP_HADD2_reg: case MXOP_HADD2_cbuf: case MXOP_HADD2_imm: case MXOP_HADD2_32I: {
        const char *ha, *hb;
        unsigned merge = MXB(i, 49, 2);
        int sat;
        if (op == MXOP_HADD2_32I) {
            ha = HX(e, A8, MXB(i, 53, 2), 0, (int)MXB(i, 56, 1)); hb = HX(e, I32(e, i), 0, 0, 0); merge = 0; sat = (int)MXB(i, 52, 1);
        } else {
            ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), (int)MXB(i, 43, 1));
            if (op == MXOP_HADD2_reg) { hb = HX(e, R20, MXB(i, 28, 2), (int)MXB(i, 30, 1), (int)MXB(i, 31, 1)); sat = (int)MXB(i, 32, 1); }
            else if (op == MXOP_HADD2_cbuf) { hb = HX(e, CB(e, i), 1, (int)MXB(i, 54, 1), (int)MXB(i, 56, 1)); sat = (int)MXB(i, 52, 1); }
            else { hb = S(e, "unpackHalf2x16(0x%08xu)", (MXB(i, 20, 9) << 6) | (MXB(i, 29, 1) << 15) | (MXB(i, 30, 9) << 22) | (MXB(i, 56, 1) << 31)); sat = (int)MXB(i, 52, 1); }
        }
        hres(e, i, S(e, "%s + %s", ha, hb), sat, merge, 0, NULL, NULL, NULL);
        break;
    }
    case MXOP_HMUL2_reg: case MXOP_HMUL2_cbuf: case MXOP_HMUL2_imm: case MXOP_HMUL2_32I: {
        const char *ha, *hb;
        unsigned merge = MXB(i, 49, 2), prec = MXB(i, 39, 2);
        int sat;
        if (op == MXOP_HMUL2_32I) { ha = HX(e, A8, MXB(i, 53, 2), 0, 0); hb = HX(e, I32(e, i), 0, 0, 0); merge = 0; sat = (int)MXB(i, 52, 1); prec = MXB(i, 55, 2); }
        else if (op == MXOP_HMUL2_reg) { ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), 0); hb = HX(e, R20, MXB(i, 28, 2), (int)MXB(i, 30, 1), (int)MXB(i, 31, 1)); sat = (int)MXB(i, 32, 1); }
        else if (op == MXOP_HMUL2_cbuf) { ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), (int)MXB(i, 43, 1)); hb = HX(e, CB(e, i), 1, (int)MXB(i, 54, 1), 0); sat = (int)MXB(i, 52, 1); }
        else { ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), (int)MXB(i, 43, 1)); hb = S(e, "unpackHalf2x16(0x%08xu)", (MXB(i, 20, 9) << 6) | (MXB(i, 29, 1) << 15) | (MXB(i, 30, 9) << 22) | (MXB(i, 56, 1) << 31)); sat = (int)MXB(i, 52, 1); }
        emit(e, "vec2 ma = %s, mb = %s; ", ha, hb);
        hres(e, i, "ma * mb", sat, merge, prec == 2, "ma", "mb", "vec2(0.0)");
        break;
    }
    case MXOP_HFMA2_reg: case MXOP_HFMA2_rc: case MXOP_HFMA2_cr: case MXOP_HFMA2_imm: case MXOP_HFMA2_32I: {
        const char *ha, *hb, *hc;
        unsigned merge = MXB(i, 49, 2), prec;
        int sat;
        ha = HX(e, A8, op == MXOP_HFMA2_32I ? MXB(i, 53, 2) : MXB(i, 47, 2), 0, 0);
        switch (op) {
        case MXOP_HFMA2_reg: hb = HX(e, R20, MXB(i, 28, 2), 0, (int)MXB(i, 31, 1)); hc = HX(e, R39, MXB(i, 35, 2), 0, (int)MXB(i, 30, 1)); sat = (int)MXB(i, 32, 1); prec = MXB(i, 37, 2); break;
        case MXOP_HFMA2_rc: hb = HX(e, R39, MXB(i, 53, 2), 0, (int)MXB(i, 56, 1)); hc = HX(e, CB(e, i), 1, 0, (int)MXB(i, 51, 1)); sat = (int)MXB(i, 52, 1); prec = MXB(i, 57, 2); break;
        case MXOP_HFMA2_cr: hb = HX(e, CB(e, i), 1, 0, (int)MXB(i, 56, 1)); hc = HX(e, R39, MXB(i, 53, 2), 0, (int)MXB(i, 51, 1)); sat = (int)MXB(i, 52, 1); prec = MXB(i, 57, 2); break;
        case MXOP_HFMA2_imm:
            hb = S(e, "unpackHalf2x16(0x%08xu)", (MXB(i, 20, 9) << 6) | (MXB(i, 29, 1) << 15) | (MXB(i, 30, 9) << 22) | (MXB(i, 56, 1) << 31));
            hc = HX(e, R39, MXB(i, 53, 2), 0, (int)MXB(i, 51, 1)); sat = (int)MXB(i, 52, 1); prec = MXB(i, 57, 2); break;
        default: hb = HX(e, I32(e, i), 0, 0, 0); hc = HX(e, X(e, MXB(i, 0, 8)), 0, 0, (int)MXB(i, 52, 1)); sat = 0; prec = MXB(i, 55, 2); merge = 0; break;
        }
        emit(e, "vec2 ma = %s, mb = %s, mc = %s; ", ha, hb, hc);
        hres(e, i, "fma(ma, mb, mc)", sat, merge, prec == 2, "ma", "mb", "mc");
        break;
    }
    case MXOP_HSET2_reg: case MXOP_HSET2_cbuf: case MXOP_HSET2_imm: {
        const char *ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), (int)MXB(i, 43, 1)), *hb;
        unsigned cmp, bf;
        if (op == MXOP_HSET2_reg) { hb = HX(e, R20, MXB(i, 28, 2), (int)MXB(i, 30, 1), (int)MXB(i, 31, 1)); cmp = MXB(i, 35, 4); bf = MXB(i, 49, 1); }
        else if (op == MXOP_HSET2_cbuf) { hb = HX(e, CB(e, i), 1, 0, (int)MXB(i, 56, 1)); cmp = MXB(i, 49, 4); bf = MXB(i, 53, 1); }
        else { hb = S(e, "unpackHalf2x16(0x%08xu)", (MXB(i, 20, 9) << 6) | (MXB(i, 29, 1) << 15) | (MXB(i, 30, 9) << 22) | (MXB(i, 56, 1) << 31)); cmp = MXB(i, 49, 4); bf = MXB(i, 53, 1); }
        const char *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "vec2 ha = %s, hb = %s; %s = (%s ? %s : 0u) | (%s ? %s : 0u);", ha, hb, DST, BOP(e, FCMP(e, cmp, "ha.x", "hb.x"), bp, MXB(i, 45, 2)), bf ? "0x3c00u" : "0xffffu",
             BOP(e, FCMP(e, cmp, "ha.y", "hb.y"), bp, MXB(i, 45, 2)), bf ? "0x3c000000u" : "0xffff0000u");
        break;
    }
    case MXOP_HSETP2_reg: case MXOP_HSETP2_cbuf: case MXOP_HSETP2_imm: {
        const char *ha = HX(e, A8, MXB(i, 47, 2), (int)MXB(i, 44, 1), (int)MXB(i, 43, 1)), *hb;
        unsigned cmp, hand;
        if (op == MXOP_HSETP2_reg) { hb = HX(e, R20, MXB(i, 28, 2), (int)MXB(i, 30, 1), (int)MXB(i, 31, 1)); cmp = MXB(i, 35, 4); hand = MXB(i, 49, 1); }
        else if (op == MXOP_HSETP2_cbuf) { hb = HX(e, CB(e, i), 1, (int)MXB(i, 54, 1), (int)MXB(i, 56, 1)); cmp = MXB(i, 49, 4); hand = MXB(i, 53, 1); }
        else { hb = S(e, "unpackHalf2x16(0x%08xu)", (MXB(i, 20, 9) << 6) | (MXB(i, 29, 1) << 15) | (MXB(i, 30, 9) << 22) | (MXB(i, 56, 1) << 31)); cmp = MXB(i, 49, 4); hand = MXB(i, 53, 1); }
        const char *bp = PR(e, MXB(i, 39, 3), (int)MXB(i, 42, 1));
        emit(e, "vec2 ha = %s, hb = %s; bool bl = %s, bh = %s;", ha, hb, BOP(e, FCMP(e, cmp, "ha.x", "hb.x"), bp, MXB(i, 45, 2)), BOP(e, FCMP(e, cmp, "ha.y", "hb.y"), bp, MXB(i, 45, 2)));
        if (hand) emit(e, " bool ba = bl && bh; bl = ba; bh = !ba;");
        if (PD(e, MXB(i, 3, 3))) emit(e, " %s = bl;", PD(e, MXB(i, 3, 3)));
        if (PD(e, MXB(i, 0, 3))) emit(e, " %s = bh;", PD(e, MXB(i, 0, 3)));
        break;
    }

    /* ---- control flow (targets were resolved by the analysis) */
    case MXOP_BRA: {
        int off = (int)(MXB(i, 20, 24) << 8) >> 8;
        unsigned tgt = (unsigned)((int)pc + 1 + off / 8);
        if ((tgt & 3) == 0) tgt++;
        if (MXB(i, 0, 5) == 0 || MXB(i, 0, 5) == 28) break;       /* never taken (the analysis did not follow it either) */
        if (e->structured) emit(e, "if (%s) { go = %uu; }", FLOW(e, MXB(i, 0, 5)), getenv("SMO_MX_MUTATE") ? 0u : tgt);      /* SMO_MX_MUTATE: a planted bug for tools/test/mx_equiv.c's self-check (jumps ignored) */
        else emit(e, "if (%s) { pc = %uu; continue; }", FLOW(e, MXB(i, 0, 5)), tgt);
        break;
    }
    case MXOP_SYNC: case MXOP_BRK: case MXOP_CONT:
        if (MXB(i, 0, 5) == 0 || MXB(i, 0, 5) == 28) break;
        if (!e->jt[pc]) { fail(e, mx_op_name[op], "%s without a matching token at word %u", mx_op_name[op], pc); break; }
        if (e->structured) emit(e, "if (%s) { go = %uu; }", FLOW(e, MXB(i, 0, 5)), getenv("SMO_MX_MUTATE") ? 0u : e->jt[pc]);
        else emit(e, "if (%s) { pc = %uu; continue; }", FLOW(e, MXB(i, 0, 5)), e->jt[pc]);
        break;
    case MXOP_BRX: {
        int off = (int)(MXB(i, 20, 24) << 8) >> 8;
        /* A table entry may point at a scheduling word (every 4th word of the code is one, not an instruction): execution
         * then starts at the word after it. brx_targets() above makes the case LABEL of such a target t + 1; the value
         * computed here at run time must get the same step, or the branch finds no case and the stage ends with its
         * outputs at their defaults. Found by ssbu-recomp in Super Smash Bros. Ultimate: a layout shader's texture coordinate table
         * (13 entries) has entries on scheduling words, and one of them left its stage with texture coordinate (0,0) = a
         * transparent texel. */
        emit(e, "if (%s) { pc = (%uu + %s + 0x%08xu) >> 3; if ((pc & 3u) == 0u) pc++; continue; }", FLOW(e, MXB(i, 0, 5)), pc * 8 + 8, A8, (uint32_t)off);
        break;
    }
    case MXOP_EXIT:
        emit(e, "if (%s) { %s return; }", FLOW(e, MXB(i, 0, 5)), e->exitcode);
        break;

    default:
        fail(e, mx_op_name[op], "instruction %s (%016llx) at word %u is not translated", mx_op_name[op], (unsigned long long)i, pc);
        break;
    }
    emit(e, " }\n");
}

/* ---------------------------------------------------------------- control flow analysis */
static int stk_id(Mx *e, const Stk *s) {
    for (int k = 0; k < e->nstk; k++) if (e->stks[k].n == s->n && !memcmp(e->stks[k].tok, s->tok, (size_t)s->n) && !memcmp(e->stks[k].tgt, s->tgt, sizeof s->tgt[0] * (size_t)s->n)) return k;
    if (e->nstk == e->capstk) { e->capstk = e->capstk * 2 + 16; e->stks = realloc(e->stks, sizeof *e->stks * (size_t)e->capstk); }
    e->stks[e->nstk] = *s;
    return e->nstk++;
}
#define MXW(k) (*(const uint64_t *)(e->base + (size_t)(k) * 8))
/* suyu indirect_branch_table_track.cpp: BRX Ra <- LDC Ra, c[n][Rb + off] <- SHL Rb, Rc, imm <- IMNMX Rc, Rd, max */
static int brx_targets(Mx *e, unsigned pc, uint64_t i, unsigned *out, int max) {
    unsigned reg = MXB(i, 8, 8), k = pc, cb = 0, ldc_src = 0, shl_src = 0, n = 0;
    int stage = 0, cboff = 0;
    while (k > 1 && stage < 3) {
        k--;
        if ((k & 3) == 0) continue;
        uint64_t w = MXW(k);
        int o = decode(w);
        if (stage == 0 && o == MXOP_LDC && MXB(w, 0, 8) == reg && MXB(w, 48, 3) == 4 && MXB(w, 44, 2) == 0) { cb = MXB(w, 36, 5); cboff = (int)(int16_t)MXB(w, 20, 16); ldc_src = MXB(w, 8, 8); stage = 1; }
        else if (stage == 1 && o == MXOP_SHL_imm && MXB(w, 0, 8) == ldc_src) { shl_src = MXB(w, 8, 8); stage = 2; }
        else if (stage == 2 && o == MXOP_IMNMX_imm && MXB(w, 0, 8) == shl_src) { if (MXB(w, 56, 1)) return -1; n = MXB(w, 20, 19) + 1; stage = 3; }
    }
    if (stage != 3) return -1;
    if (cb != 1 || !e->in->cdata) return -2;
    int off = (int)(MXB(i, 20, 24) << 8) >> 8, cnt = 0;
    for (unsigned q = 0; q < n && cnt < max; q++) {
        uint32_t byte = (uint32_t)cboff + q * 4, v;
        if (byte + 4 > e->in->cdata_size) return -3;
        memcpy(&v, e->in->cdata + byte, 4);
        unsigned t = (pc * 8 + 8 + (uint32_t)off + v) >> 3;
        if ((t & 3) == 0) t++;
        out[cnt++] = t;
    }
    return cnt;
}
static void analyze(Mx *e) {
    unsigned *wpc = malloc(sizeof(unsigned) * ((size_t)e->words * 2 + 64)), nw = 0, cap = e->words * 2 + 64;
    uint16_t *wst = malloc(sizeof(uint16_t) * cap);
    Stk empty;
    memset(&empty, 0, sizeof empty);
    wpc[nw] = 1; wst[nw++] = (uint16_t)stk_id(e, &empty);
    e->leader[1] = 1;
    while (nw && !e->fail) {
        nw--;
        unsigned pc = wpc[nw];
        int sid = wst[nw];
        for (; pc < e->words; pc++) {
            if ((pc & 3) == 0) continue;
            if (e->reach[pc]) break;                              /* joins an analysed path (first stack wins, as in suyu) */
            e->reach[pc] = 1; e->stk_of[pc] = (uint16_t)sid;
            uint64_t i = MXW(pc);
            int op = decode(i), never = MXB(i, 16, 3) == 7 && MXB(i, 19, 1), always = MXB(i, 16, 3) == 7 && !MXB(i, 19, 1);
            unsigned ft = MXB(i, 0, 5);
            if (never) continue;
            if (op == MXOP_SSY || op == MXOP_PBK || op == MXOP_PCNT || op == MXOP_PEXIT) {
                int off = (int)(MXB(i, 20, 24) << 8) >> 8;
                unsigned tgt = (unsigned)((int)pc + 1 + off / 8);
                if ((tgt & 3) == 0) tgt++;
                if (MXB(i, 5, 1)) { fail(e, mx_op_name[op], "%s through a constant buffer", mx_op_name[op]); break; }
                Stk s = e->stks[sid];
                if (s.n == MAXSTK) { fail(e, "flow stack", "flow stack deeper than %d", MAXSTK); break; }
                s.tok[s.n] = op == MXOP_SSY ? 1 : op == MXOP_PBK ? 2 : op == MXOP_PCNT ? 3 : 4;
                s.tgt[s.n++] = tgt;
                sid = stk_id(e, &s);
                continue;
            }
            if (op == MXOP_BRA) {
                int off = (int)(MXB(i, 20, 24) << 8) >> 8;
                unsigned tgt = (unsigned)((int)pc + 1 + off / 8);
                if ((tgt & 3) == 0) tgt++;
                if (MXB(i, 5, 1)) { fail(e, "BRA.CC", "branch through a constant buffer"); break; }
                if (ft == 0 || ft == 28) continue;                   /* flow test never true */
                if (tgt >= e->words) { fail(e, "BRA", "branch target %u outside the code", tgt); break; }
                e->leader[tgt] = 1; e->any_jump = 1;
                if (nw >= cap) break;
                wpc[nw] = tgt; wst[nw++] = (uint16_t)sid;
                if (always && ft == 15) break;
                continue;
            }
            if (op == MXOP_SYNC || op == MXOP_BRK || op == MXOP_CONT) {
                uint8_t tok = op == MXOP_SYNC ? 1 : op == MXOP_BRK ? 2 : 3;
                Stk s = e->stks[sid];
                int k = s.n - 1;
                while (k >= 0 && s.tok[k] != tok) k--;
                if (k < 0) { fail(e, mx_op_name[op], "%s with no matching token at word %u", mx_op_name[op], pc); break; }
                if (ft == 0 || ft == 28) continue;
                unsigned tgt = s.tgt[k];
                s.n = k;
                e->jt[pc] = tgt; e->leader[tgt] = 1; e->any_jump = 1;
                if (tgt >= e->words) { fail(e, mx_op_name[op], "target outside the code"); break; }
                if (nw >= cap) break;
                wpc[nw] = tgt; wst[nw++] = (uint16_t)stk_id(e, &s);
                if (always && ft == 15) break;
                continue;
            }
            if (op == MXOP_BRX) {
                unsigned tg[256];
                int n = brx_targets(e, pc, i, tg, 256);
                if (n < 0) { fail(e, "BRX", n == -2 ? "BRX table not in constant buffer 1" : n == -3 ? "BRX table outside the constants" : "BRX table not found"); break; }
                for (int k = 0; k < n; k++) {
                    if (tg[k] >= e->words) { fail(e, "BRX", "BRX target outside the code"); break; }
                    e->leader[tg[k]] = 1;
                    if (nw < cap) { wpc[nw] = tg[k]; wst[nw++] = (uint16_t)sid; }
                }
                e->any_jump = 1;
                if (always && ft == 15) break;
                continue;
            }
            if (op == MXOP_EXIT) { if (always && ft == 15) break; continue; }
            if (op == MXOP_CAL || op == MXOP_JCAL || op == MXOP_RET || op == MXOP_JMP || op == MXOP_JMX || op == MXOP_LONGJMP || op == MXOP_PRET) {
                fail(e, mx_op_name[op], "subroutine / absolute jump %s", mx_op_name[op]);
                break;
            }
        }
    }
    free(wpc); free(wst);
}

/* STRUCTURED EMISSION. The pc switch inside for(;;) makes every register loop-carried across all blocks; on a
 * 35,000-instruction SMO fragment stage (4,944 forward branches, 1 backward) the NVIDIA compiler did not finish in
 * minutes. Instead: code in address order; a jump to T sets "go = T"; every run of instructions is guarded by
 * "if (go == 0u)" and ends after a jump; label T clears go ("if (go == T) go = 0u;"). A backward jump to T makes
 * [T, last backward jump to T] a loop: "for (;;) { ... if (go == T) { go = 0u; continue; } break; }"; a jump out of a
 * loop leaves it through the guards and the break; a jump into a loop is skipped up to its label as anywhere else.
 * Not planned (the pc switch stays): BRX, and loops that overlap without nesting. SMO_MX_NOSTRUCT=1: always the switch. */
static int is_jump_op(int op) { return op == MXOP_BRA || op == MXOP_SYNC || op == MXOP_BRK || op == MXOP_CONT; }
static unsigned jump_target(Mx *e, unsigned pc, uint64_t i, int op) {
    if (op == MXOP_BRA) { int off = (int)(MXB(i, 20, 24) << 8) >> 8; unsigned tgt = (unsigned)((int)pc + 1 + off / 8); if ((tgt & 3) == 0) tgt++; return tgt; }
    return e->jt[pc];
}
static int plan_structured(Mx *e) {
    if (getenv("SMO_MX_NOSTRUCT")) return 0;
    e->loop_end = calloc(e->words + 8, sizeof *e->loop_end);
    for (unsigned pc = 1; pc < e->words; pc++) {
        if (!e->reach[pc]) continue;
        uint64_t i = MXW(pc);
        int op = decode(i);
        if (MXB(i, 16, 3) == 7 && MXB(i, 19, 1)) continue;          /* never executed */
        if (op == MXOP_BRX) return 0;
        if (!is_jump_op(op) || MXB(i, 0, 5) == 0 || MXB(i, 0, 5) == 28) continue;
        unsigned t = jump_target(e, pc, i, op);
        if (!t || t >= e->words) return 0;
        if (t <= pc && pc > e->loop_end[t]) e->loop_end[t] = pc;    /* a backward jump: loop [t, pc] */
    }
    for (unsigned a = 1; a < e->words; a++) {                       /* loops must nest */
        if (!e->loop_end[a]) continue;
        for (unsigned b = a + 1; b <= e->loop_end[a]; b++) if (e->loop_end[b] && e->loop_end[b] > e->loop_end[a]) return 0;
    }
    return 1;
}
/* ---------------------------------------------------------------- whole shader */
static const char *const k_prelude =
    "#define F(x) uintBitsToFloat(x)\n#define U(x) floatBitsToUint(x)\n"
    "bool cz = false, cs = false, ccf = false, cov = false;\n"
    "vec2 H(uint r, uint s) { vec2 v = unpackHalf2x16(r); return s == 0u ? v : s == 1u ? vec2(F(r)) : s == 2u ? v.xx : v.yy; }\n"
    "uint HM(uint d, vec2 v, uint m) { uint p = packHalf2x16(v); return m == 0u ? p : m == 1u ? U(v.x) : m == 2u ? ((d & 0xffff0000u) | (p & 0xffffu)) : ((d & 0xffffu) | (p & 0xffff0000u)); }\n"
    "uint mx_addc(uint a, uint b, uint ci) { uint c1, c2; uint r = uaddCarry(a, b, c1); r = uaddCarry(r, ci, c2); cz = r == 0u; cs = int(r) < 0; ccf = (c1 | c2) != 0u; cov = (((a ^ r) & (b ^ r)) >> 31) != 0u; return r; }\n"
    "uint mx_bfe(uint base, uint ctl, bool sgn) { uint off = ctl & 255u, cnt = (ctl >> 8) & 255u;\n"
    "  if (cnt == 0u) return 0u;\n"
    "  if (off >= 32u) return sgn && int(base) < 0 ? 0xffffffffu : 0u;\n"
    "  if (off + cnt >= 32u) return sgn ? uint(int(base) >> off) : base >> off;\n"
    "  return sgn ? uint(bitfieldExtract(int(base), int(off), int(cnt))) : bitfieldExtract(base, int(off), int(cnt)); }\n"
    "uint mx_bfi(uint base, uint ins, uint ctl) { uint off = ctl & 255u, cnt = (ctl >> 8) & 255u;\n"
    "  if (off >= 32u || cnt == 0u) return base; cnt = min(cnt, 32u - off); return bitfieldInsert(base, ins, int(off), int(cnt)); }\n"
    "uint mx_f2i(float f, uint kind) {   /* kind: 0 u32, 1 s32, 2 u16, 3 s16; NaN -> 0 */\n"
    "  if (isnan(f)) return 0u;\n"
    "  if (kind == 1u) return f >= 2147483648.0 ? 0x7fffffffu : f <= -2147483648.0 ? 0x80000000u : uint(int(f));\n"
    "  if (kind == 0u) return f >= 4294967296.0 ? 0xffffffffu : f <= 0.0 ? 0u : uint(f);\n"
    "  if (kind == 3u) return uint(int(clamp(f, -32768.0, 32767.0)));\n"
    "  return uint(clamp(f, 0.0, 65535.0)); }\n";

/* IPA "multiply" by 1 / w undoes the * w of IPA "pass": every read of a perspective varying is emitted as
 * f = attr; f *= gl_FragCoord.w; f *= F(rK) with rK = 1.0 / gl_FragCoord[3] (the shader's MUFU.RCP of the position's
 * IPA), two multiplies the driver cannot fold (w * (1 / w) is not 1 in floats). Measured over the game's 20,436
 * fragment stages: 262,424 such reads in 19,091 of them. Both multiplies are dropped where rK surely holds 1 / w: after
 * a line that set rK to the reciprocal of a register set to gl_FragCoord[3], and before the next write of rK, along
 * lines that only fall through: unconditional instruction lines ("{ ... }"), predicated ones ("if (pN) { ... }", which
 * forget what they may write; a predicated jump or discard only means the lines after it run on the other path), and
 * the "if (go == 0u) {" / "}" around them, and a jump target ("if (go == N) go = 0u;") before any jump. A later jump
 * target, a return or anything else forgets everything but an rK set before the first jump, up to its next write: with
 * only forward jumps the lines run in text order, so every path from there has gone through that line. A loop ("for",
 * "continue") forgets that too. The varying is then the value Vulkan interpolated, as on the console up to
 * rounding; the same fold as Savage Wind's glsl_fold_ipa_w (-8 % ACO instructions there). */
static int fold_reg_after(const char *p, const char *pre) {       /* "<pre>rN" at p: N, else -1 */
    size_t n = strlen(pre);
    if (strncmp(p, pre, n)) return -1;
    p += n;
    if (*p != 'r' || p[1] < '0' || p[1] > '9') return -1;
    return atoi(p + 1);
}
static int fold_target(const char *line, size_t n) {          /* "if (go == N) go = 0u;": where a jump lands */
    return n > 19 && !strncmp(line, "if (go == ", 10) && !strncmp(line + n - 9, " go = 0u;", 9);
}
static void fold_ipa_w(char *s, size_t *len) {
    unsigned char st[256];                                    /* 0 unknown, 1 gl_FragCoord.w, 2 1 / gl_FragCoord.w */
    unsigned char keep[256];                                  /* 1 / w on every path to here: set before any jump */
    memset(st, 0, sizeof st); memset(keep, 0, sizeof keep);
    int entry = 1;                                             /* no jump or jump target seen yet */
    char *w = s;
    for (char *line = s; *line;) {
        char *eol = strchr(line, '\n');
        size_t n = eol ? (size_t)(eol - line + 1) : strlen(line);
        char *start = w;
        memmove(w, line, n); w += n;
        line += n;
        size_t body = n - (eol != NULL);
        if ((body == 1 && start[0] == '}') || (body == 15 && !strncmp(start, "if (go == 0u) {", 15))) continue;
        if (entry && fold_target(start, body)) continue;       /* no jump to it yet */
        int pred = !strncmp(start, "if (p", 5) || !strncmp(start, "if (!p", 6);
        char last = start[body];
        start[body] = 0;                                        /* the line as a string while it is looked at */
        if ((start[0] != '{' && !pred) || strstr(start, "continue") || strstr(start, "break") || strstr(start, "return")) {
            int fwd = fold_target(start, body) ||
                      ((strstr(start, "return") || strstr(start, "break")) && !strstr(start, "continue") && !strstr(start, "for ("));
            for (int r = 0; r < 256; r++) st[r] = (keep[r] &= fwd) ? 2 : 0;    /* a loop runs lines again: no more */
            entry = 0;
            start[body] = last;
            continue;
        }
        if (strstr(start, "go = ")) entry = 0;
        int set = -1, kind = 0, k;
        if (pred) {}
        else if ((k = fold_reg_after(start, "{ float f = F(U(gl_FragCoord[3])); ")) >= 0) { set = k; kind = 1; }
        else if (!strncmp(start, "{ float f = (1.0 / (F(r", 23)) {
            int src = atoi(start + 23);
            const char *q = strstr(start, "))); ");
            if (q && (k = fold_reg_after(q, "))); ")) >= 0 && src < 256 && st[src] == 1) { set = k; kind = 2; }
        }
        for (char *m = start; (m = strstr(m, "f *= gl_FragCoord.w; f *= F(r"));) {
            int r = atoi(m + 29);
            char *close = strchr(m + 29, ')');
            if (r < 256 && st[r] == 2 && close && close[1] == ';') {
                size_t cut = (size_t)(close + 2 - m) + (close[2] == ' ');
                memmove(m, m + cut, strlen(m + cut) + 1);
            } else m++;
        }
        size_t kept = strlen(start);
        for (char *m = start; (m = strstr(m, " r"));) {          /* every register this line writes: "rN = " */
            m += 2;
            if (*m < '0' || *m > '9') continue;
            int r = atoi(m);
            while (*m >= '0' && *m <= '9') m++;
            if (m[0] == ' ' && m[1] == '=' && m[2] != '=' && r < 256) { st[r] = r == set ? (unsigned char)kind : 0; keep[r] = r == set && kind == 2 && entry; }
        }
        w = start + kept;
        if (eol) *w++ = '\n';
    }
    *w = 0;
    *len = (size_t)(w - s);
}

int mx_translate(const MxIn *in, MxOut *out) {
    Mx *e = calloc(1, sizeof *e);
    char *res = NULL;
    memset(out, 0, sizeof *out);
    e->in = in; e->code = in->code;
    int hdr = in->stage == MX_COMPUTE ? 0 : 0x50;                  /* a compute stage has no program header */
    if (in->code_len < hdr + 0x10 || !in->code) { snprintf(out->why, sizeof out->why, "no code"); strcpy(out->op, "no code"); free(e); return 0; }
    e->base = in->code + hdr;
    e->words = (unsigned)(in->code_len - hdr) / 8;
    e->reach = calloc(e->words + 8, 1); e->leader = calloc(e->words + 8, 1);
    e->jt = calloc(e->words + 8, sizeof *e->jt); e->stk_of = calloc(e->words + 8, sizeof *e->stk_of);
    if (in->stage == MX_COMPUTE) {
        e->lmem_words = in->lmem_bytes > 65536 ? 16384 : (in->lmem_bytes + 3) / 4;
        e->smem_words = in->smem_bytes > (1u << 20) ? (1u << 18) : (in->smem_bytes + 3) / 4;
    } else {
        uint32_t lo, hi;
        memcpy(&lo, in->code + 4, 4); memcpy(&hi, in->code + 8, 4);
        uint64_t lsize = (lo & 0xffffff) | ((uint64_t)(hi & 0xffffff) << 24);
        e->lmem_words = lsize > 65536 ? 16384 : (unsigned)((lsize + 3) / 4);
    }
    {
        static const unsigned hw[6] = {0, 4, 3, 1, 2, 0};          /* hardware stage order: vertex, tess control, tess eval, geometry, fragment */
        e->ssbo_word0 = in->stage == MX_COMPUTE ? 0x310 / 4 : (0x110 + 0x100 * hw[in->stage]) / 4;
    }
    if (in->stage == MX_GEOMETRY && in->target != 1) fail(e, "geometry stage", "a geometry stage in the OpenGL text");      /* Vulkan only */
    else if (in->stage != MX_VERTEX && in->stage != MX_FRAGMENT && in->stage != MX_COMPUTE && in->stage != MX_GEOMETRY) fail(e, "tessellation stage", "stage %d is not supported", in->stage);

    /* the code that ends the shader (EXIT): fragment outputs from the registers, alpha test, depth */
    if (in->stage == MX_FRAGMENT) {
        uint32_t omap, flags;
        unsigned reg = 0;
        size_t n = 0;
        memcpy(&omap, in->code + 0x48, 4); memcpy(&flags, in->code + 0x4c, 4);
        for (unsigned rt = 0; rt < 8; rt++) {
            if (!((omap >> (rt * 4)) & 15)) continue;
            n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "frag%u = vec4(", rt);
            for (unsigned k = 0; k < 4; k++, reg++) {
                if ((omap >> (rt * 4 + k)) & 1) { e->regs[reg] = 1; n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "F(r%u)%s", reg, k < 3 ? ", " : ""); }
                else n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "0.0%s", k < 3 ? ", " : "");
            }
            n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "); ");
            if (rt == 0) { e->uses_alpha = 1; n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "if (!mx_alpha_ok(frag0.a)) discard; "); }
        }
        if (flags & 1) { e->regs[reg] = 1; n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "gl_SampleMask[0] = int(r%u); ", reg); }
        if (flags & 2) { e->regs[reg + 1] = 1; n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "gl_FragDepth = F(r%u); ", reg + 1); }
        if (in->fragdebug && (omap & 15)) n += (size_t)snprintf(e->exitcode + n, sizeof e->exitcode - n, "frag0 = vec4(1.0, 0.0, 1.0, 1.0); ");
    }

    if (!e->fail) analyze(e);
    if (!e->fail && e->any_jump && plan_structured(e)) e->structured = 1;
    if (e->structured) {
        unsigned stack[64]; int ns = 0, open = 0;
        for (unsigned pc = 1; pc < e->words && !e->fail; pc++) {
            if ((e->loop_end[pc] || (e->leader[pc] && e->reach[pc])) && open) { emit(e, "}\n"); open = 0; }
            if (e->loop_end[pc]) {
                if (ns == 64) { fail(e, "loops", "loops nested deeper than 64"); break; }
                stack[ns++] = pc; emit(e, "for (;;) {\n"); e->blocks++;
            }
            if (e->reach[pc]) {
                if (e->leader[pc]) { emit(e, "if (go == %uu) go = 0u;\n", pc); e->blocks++; }
                if (!open) { emit(e, "if (go == 0u) {\n"); open = 1; }
                uint64_t i = MXW(pc);
                insn(e, pc, i);
                if (is_jump_op(decode(i))) { emit(e, "}\n"); open = 0; }
            }
            while (ns && e->loop_end[stack[ns - 1]] == pc) {
                if (open) { emit(e, "}\n"); open = 0; }
                emit(e, "if (go == %uu) { go = 0u; continue; } break; }\n", stack[ns - 1]); ns--;
            }
        }
        if (open) emit(e, "}\n");
    } else
    for (unsigned pc = 1; pc < e->words && !e->fail; pc++) {
        if (!e->reach[pc]) continue;
        if (e->leader[pc] && e->any_jump) { emit(e, "case %uu:\n", pc); e->blocks++; }
        insn(e, pc, MXW(pc));
    }
    if (e->fail) {
        snprintf(out->why, sizeof out->why, "%s", e->why);
        snprintf(out->op, sizeof out->op, "%s", e->failop);
        goto done;
    }

    if (in->stage == MX_FRAGMENT && in->target == 1 && e->s && !getenv("SMO_MX_NO_IPA_FOLD")) fold_ipa_w(e->s, &e->len);
    /* assemble: header, declarations, helpers, main */
    {
        Mx h;
        memset(&h, 0, sizeof h);
        h.in = in;
        if (in->stage == MX_VERTEX) e->out_attr |= in->force_out;      /* declared and set to (0, 0, 0, 1) below */
        const int vk = in->target == 1;
        const char *set0 = vk ? "set = 0, " : "", *set1 = vk ? "set = 1, " : "";
        if (vk) emit(&h, "#version 460\n#extension GL_ARB_gpu_shader_int64 : enable\n#extension GL_EXT_texture_shadow_lod : enable\n#extension GL_ARB_shader_ballot : enable\n#extension GL_ARB_shader_group_vote : enable\n");
        else
        emit(&h, "#version 460 compatibility\n#extension GL_ARB_bindless_texture : require\n#extension GL_NV_gpu_shader5 : enable\n#extension GL_ARB_gpu_shader_int64 : enable\n"
                 "#extension GL_ARB_shader_ballot : enable\n#extension GL_ARB_shader_group_vote : enable\n#extension GL_EXT_texture_shadow_lod : enable\n");
        if (e->uses_layer_out || e->uses_vp_out) emit(&h, "#extension GL_ARB_shader_viewport_layer_array : require\n");
        if (e->uses_shfl) emit(&h, "#extension GL_KHR_shader_subgroup_basic : require\n#extension GL_KHR_shader_subgroup_shuffle : require\n");      /* SHFL */
        emit(&h, "%s", k_prelude);
        if (vk && in->stage == MX_FRAGMENT && in->screen_scale && e->uses_fcs) emit(&h, "layout(push_constant) uniform MXPC { uvec2 mx_alpha; float mx_point_size; float mx_fcs; };\n");      /* */
        else
        if (vk && (in->stage == MX_VERTEX || (in->stage == MX_FRAGMENT && e->uses_alpha))) emit(&h, "layout(push_constant) uniform MXPC { uvec2 mx_alpha; float mx_point_size; };\n");
        if (vk && e->uses_tf) emit(&h, "ivec2 mx_tf(ivec2 p, ivec2 real, uint gw, uint gh) { ivec2 g = ivec2(int(gw), int(gh)); return (g.x <= 0 || g.y <= 0 || g == real) ? p : ivec2(floor((vec2(p) + 0.5) * vec2(real) / vec2(g))); }\n");
        if (vk && in->stage == MX_VERTEX) emit(&h, "float mx_psize;\n");
        for (int k = 0; vk && k < e->nsmp; k++) emit(&h, "layout(set = 1, binding = %u) uniform %s mx_t%u_%s;\n", (unsigned)e->smp[k].binding, e->smp[k].type, (unsigned)e->smp[k].unit, e->smp[k].type);
        for (unsigned n = 0; n < 18; n++) if (e->cbufs >> n & 1) emit(&h, "layout(std430, %sbinding = %u) readonly buffer CB%u { uint c%u[]; };\n", set0, in->binding_base + n, n, n);
        if (in->stage == MX_VERTEX) {
            int nclip = 0;
            for (int k = 0; k < 8; k++) if (e->clip_out >> k & 1) nclip = k + 1;
            /* invariant position. A depth pre-pass and the colour pass that tests EQUAL against it run two programs
             * with the same position code; Maxwell executes it identically, a GLSL compiler may not (contraction, order)
             * unless the output is invariant. Without it SMO's title-screen Mario passed the EQUAL test only at random
             * pixels: black speckles everywhere and no Mario (boot93, pass 11). */
            emit(&h, "out gl_PerVertex { %svec4 gl_Position; float gl_PointSize;", in->target == 1 ? "invariant " : "");
            if (nclip) emit(&h, " float gl_ClipDistance[%d];", nclip);
            emit(&h, " };\n");
            for (unsigned n = 0; n < 32; n++)
                if (e->in_attr >> n & 1) emit(&h, "layout(location = %u) in %s in_attr%u;\n", n, in->int_attrs >> n & 1 ? (in->sint_attrs >> n & 1 ? "ivec4" : "uvec4") : "vec4", n);
            for (unsigned n = 0; n < 32; n++) if (e->out_attr >> n & 1) emit(&h, "layout(location = %u) out vec4 out_attr%u;\n", n, n);
        } else if (in->stage == MX_FRAGMENT) {
            uint32_t omap;
            memcpy(&omap, in->code + 0x48, 4);
            for (unsigned n = 0; n < 32; n++) {
                if (!(e->in_attr >> n & 1)) continue;
                unsigned m = in->code[0x18 + n], eff = 0;
                for (int k = 0; k < 4; k++) if ((m >> (k * 2)) & 3) eff = (m >> (k * 2)) & 3;
                emit(&h, "layout(location = %u) %sin vec4 in_attr%u;\n", n, eff == 1 ? "flat " : eff == 3 ? "noperspective " : "", n);
            }
            for (unsigned n = 0; n < 8; n++) if ((omap >> (n * 4)) & 15) emit(&h, "layout(location = %u) out vec4 frag%u;\n", n, n);
            if (e->uses_alpha) {
                if (!vk) emit(&h, "layout(location = 0) uniform uvec2 mx_alpha;\n");
                emit(&h, "bool mx_alpha_ok(float a) { uint f = mx_alpha.x; float r = F(mx_alpha.y);\n"
                         "  return f == 0u || f >= 8u ? true : f == 1u ? false : f == 2u ? a < r : f == 3u ? a == r : f == 4u ? a <= r : f == 5u ? a > r : f == 6u ? a != r : a >= r; }\n");
            }
        } else if (in->stage == MX_GEOMETRY) {
            /* program header ([doc] yuzu shader_header.h): word 2 bits 24-31 threads (invocations) per input primitive,
             * word 3 bits 24-27 output topology (1 points, 6 line strip, 7 triangle strip), word 4 bits 0-11 maximum output
             * vertices. The input primitive is the draw's (in->gs_in_prim). Outputs as a vertex stage's; outputs the next
             * stage reads that the code never writes (force_out) are set to (0, 0, 0, 1) before every EmitVertex (MX_GS_FILL). */
            static const char *const prim[5] = {"points", "lines", "triangles", "lines_adjacency", "triangles_adjacency"};
            uint32_t w2, w3, w4;
            memcpy(&w2, in->code + 8, 4); memcpy(&w3, in->code + 12, 4); memcpy(&w4, in->code + 16, 4);
            unsigned topo = (w3 >> 24) & 15, maxv = w4 & 0xfff, inv = (w2 >> 24) & 0xff;
            const char *ot = topo == 1 ? "points" : topo == 6 ? "line_strip" : topo == 7 ? "triangle_strip" : NULL;
            if (!ot) fail(e, "geometry output topology", "output topology %u", topo);
            emit(&h, "layout(%s) in;\n", prim[(unsigned)in->gs_in_prim % 5]);
            if (inv > 1) emit(&h, "layout(invocations = %u) in;\n", inv > 32 ? 32 : inv);
            emit(&h, "layout(%s, max_vertices = %u) out;\n", ot ? ot : "points", maxv ? maxv : 1);
            emit(&h, "in gl_PerVertex { vec4 gl_Position; float gl_PointSize; } gl_in[];\n");
            int nclip = 0;
            for (int k = 0; k < 8; k++) if (e->clip_out >> k & 1) nclip = k + 1;
            emit(&h, "out gl_PerVertex { invariant vec4 gl_Position; float gl_PointSize;");
            if (nclip) emit(&h, " float gl_ClipDistance[%d];", nclip);
            emit(&h, " };\n");
            uint32_t fill = in->force_out & ~e->out_attr;
            e->out_attr |= in->force_out;
            for (unsigned n = 0; n < 32; n++) if (e->in_attr >> n & 1) emit(&h, "layout(location = %u) in vec4 in_attr%u[];\n", n, n);
            for (unsigned n = 0; n < 32; n++) if (e->out_attr >> n & 1) emit(&h, "layout(location = %u) out vec4 out_attr%u;\n", n, n);
            emit(&h, "#define MX_GS_FILL");
            for (unsigned n = 0; n < 32; n++) if (fill >> n & 1) emit(&h, " out_attr%u = vec4(0.0, 0.0, 0.0, 1.0);", n);
            emit(&h, "\n");
        }
        if (in->stage == MX_COMPUTE) {
            {   unsigned k = mx_split_x(in->local_size, in->smem_bytes), lx = in->local_size[0] ? in->local_size[0] : 1;      /* a split group (mx_translate.h) */
                emit(&h, "layout(local_size_x = %u, local_size_y = %u, local_size_z = %u) in;\n", k > 1 ? lx / k : lx, in->local_size[1] ? in->local_size[1] : 1, in->local_size[2] ? in->local_size[2] : 1);
                if (k > 1) emit(&h, "#define MX_LID_X (gl_LocalInvocationID.x + (gl_WorkGroupID.x %% %uu) * %uu)\n#define MX_GID_X (gl_WorkGroupID.x / %uu)\n", k, lx / k, k);
                else emit(&h, "#define MX_LID_X gl_LocalInvocationID.x\n#define MX_GID_X gl_WorkGroupID.x\n");
            }
            if (e->uses_smem) emit(&h, "shared uint smem[%u];\n", e->smem_words);
        }
        for (unsigned k = 0; k < 8; k++) {
            static const char *const ity[6] = {"1D", "Buffer", "1DArray", "2D", "2DArray", "3D"};
            int cls = 0;
            if (!(e->images >> k & 1)) continue;
            const char *q = img_qualifier(e->image_fmt[k], &cls);
            emit(&h, "layout(%sbinding = %u%s%s) uniform %s%simage%s mx_img%u;\n", set1, in->image_base + k, q ? ", " : "", q ? q : "", !(e->images_read >> k & 1) ? "writeonly " : "",
                 cls == 1 ? "u" : cls == 2 ? "i" : "", ity[e->image_type[k]], k);
        }
        if (e->uses_lmem) emit(&h, "uint lmem[%u];\n", e->lmem_words);
        for (unsigned k = 0; k < 16; k++) if (e->ssbos >> k & 1) emit(&h, "layout(std430, %sbinding = %u) %sbuffer SB%u { uint s%u[]; };\n", set0, in->ssbo_base + k, e->uses_gstore ? "" : "readonly ", k, k);
        if (e->uses_gstore && in->track_writes) emit(&h, "layout(std430, %sbinding = %u) buffer WM { uint mx_wm; };\n", set0, in->wmask_binding);
        if (e->uses_gmem) {
            emit(&h, "uint mx_ldg(uint ad) { uint o = (ad & 0x07ffffffu) >> 2; switch (ad >> 27) {\n");
            for (unsigned k = 0; k < 16; k++) if (e->ssbos >> k & 1) emit(&h, " case %uu: return s%u[o];\n", k + 1, k);
            for (unsigned k = 0; k < 15; k++) if (e->gubos >> k & 1) emit(&h, " case %uu: return c%u[o];\n", 17 + k, 3 + k);
            emit(&h, " } return 0u; }\n");
        }
        if (e->uses_gstore) {
            emit(&h, "void mx_stg(uint ad, uint v) { uint o = (ad & 0x07ffffffu) >> 2; switch (ad >> 27) {\n");
            for (unsigned k = 0; k < 16; k++) if (e->ssbos >> k & 1) emit(&h, " case %uu: s%u[o] = v;%s break;\n", k + 1, k, in->track_writes ? S(e, " atomicOr(mx_wm, %uu);", 1u << k) : "");
            emit(&h, " } }\n");
        }
        emit(&h, "void main() {\n");
        if (in->stage == MX_VERTEX) {
            emit(&h, " gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n");
            if (vk) emit(&h, " gl_PointSize = mx_point_size;\n");
            for (unsigned n = 0; n < 32; n++) if (e->out_attr >> n & 1) emit(&h, " out_attr%u = vec4(0.0, 0.0, 0.0, 1.0);\n", n);
        }
        int any = 0;
        for (unsigned r = 0; r < 255; r++) if (e->regs[r]) { emit(&h, "%s r%u = 0u", any ? "," : " uint", r); any = 1; }
        emit(&h, "%s uint rz = 0u;\n", any ? ";\n" : "");
        any = 0;
        for (unsigned p = 0; p < 7; p++) if (e->preds[p]) { emit(&h, "%s p%u = false", any ? "," : " bool", p); any = 1; }
        if (any) emit(&h, ";\n");
        if (e->structured) emit(&h, " uint go = 0u;\n");
        else if (e->any_jump) emit(&h, " uint pc = 1u;\n for (;;) { switch (pc) {\n");
        size_t need = h.len + e->len + strlen(e->exitcode) + 256;
        res = malloc(need);
        memcpy(res, h.s, h.len);
        memcpy(res + h.len, e->s, e->len);
        size_t n = h.len + e->len;
        if (e->any_jump && !e->structured) n += (size_t)sprintf(res + n, " default: %s return;\n } }\n}\n", e->exitcode);
        else n += (size_t)sprintf(res + n, " %s\n}\n", e->exitcode);
        res[n] = 0;
        free(h.s);
    }
    out->glsl = res;
    out->cbufs = e->cbufs; out->in_attr = e->in_attr; out->out_attr = e->out_attr; out->tex_units = e->tex_units;
    out->int_dep = in->stage == MX_VERTEX ? e->in_attr : e->tex_units;
    out->ssbos = e->ssbos; out->ssbo_written = e->uses_gstore ? e->ssbos : 0;
    out->images = e->images; out->images_read = e->images_read; out->images_written = e->images_written;
    memcpy(out->image_gl_format, e->image_fmt, sizeof out->image_gl_format); memcpy(out->image_type, e->image_type, sizeof out->image_type);
    out->insns = e->insns; out->blocks = e->blocks; out->uses_alpha = e->uses_alpha;
    out->nsmp = e->nsmp; memcpy(out->smp, e->smp, sizeof out->smp);
done:
    free(e->s); free(e->reach); free(e->leader); free(e->loop_end); free(e->jt); free(e->stk_of); free(e->stks);
    free(e);
    return res != NULL;
}
