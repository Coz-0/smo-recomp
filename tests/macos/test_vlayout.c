/* gfx/nvn_vk_vlayout.c, the vertex layout in the pipeline key (docs/macos-port.md "The vertex layout in the pipeline key"),
 * without a GPU: for random attribute, stream and buffer states, the descriptions made from the key (vl_of, vl_descs, the
 * macOS path) must equal those of the dynamic path (vin_dynamic, the Windows one) for the same state, binding for binding
 * and buffer for buffer; states that differ only in what the layout does not read give one key; and the pipeline's vertex
 * input state (vl_state) says the same. Exit code = failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a stand-in for nvn_vk_draw.c's table: some NVN formats have no Vulkan format, as there */
static VkFormat vertex_vkfmt(int nf) {
    if (nf <= 0 || nf > 0x78 || nf % 7 == 3) return VK_FORMAT_UNDEFINED;
    return (VkFormat)(VK_FORMAT_R8_UNORM + nf);
}
#include "nvn_vk_vlayout.c"

static uint32_t g_r = 12345;
static uint32_t rnd(uint32_t n) { g_r = g_r * 1103515245u + 12345u; return (g_r >> 8) % n; }

typedef struct { int nattr; uint32_t vattr[32], ints, sints; VStream vstream[32]; uint64_t voff[16]; VkBuffer vbufs[16]; } State;
static void random_state(State *s) {
    memset(s, 0, sizeof *s);
    s->nattr = (int)rnd(18);
    int nstreams = 1 + (int)rnd(4);
    for (int i = 0; i < s->nattr; i++) {
        uint32_t nf = rnd(5) ? 1 + rnd(0x78) : 0, stream = rnd(8) ? rnd((uint32_t)nstreams) : rnd(32), off = rnd(4) ? rnd(64) * 4 : rnd(1u << 18);
        s->vattr[i] = nf | stream << 8 | off << 14;
    }
    s->ints = rnd(65536); s->sints = rnd(65536) & s->ints;
    for (int i = 0; i < 32; i++) { s->vstream[i].stride = rnd(6) ? rnd(16) * 4 : 0; s->vstream[i].divisor = rnd(5) ? 0 : rnd(4); }
    for (int i = 0; i < 16; i++) {
        s->voff[i] = rnd(6) ? rnd(1u << 20) * 16 : ~0ull;
        s->vbufs[i] = (VkBuffer)(uintptr_t)(0x1000 + i * 0x10);
    }
}
/* the draw: streams with a buffer are the ones an attribute names (nvn_vk_draw.c draw_prepare) */
static void draw_streams(State *s) {
    uint32_t named = 0;
    for (int i = 0; i < 16 && i < s->nattr; i++) if (s->vattr[i] & 0xff) named |= 1u << ((s->vattr[i] >> 8) & 31);
    for (int i = 0; i < 16; i++) if (!(named >> i & 1)) s->voff[i] = ~0ull;
}
static uint32_t bound_of(const State *s) { uint32_t b = 0; for (int i = 0; i < 16; i++) if (s->voff[i] != ~0ull) b |= 1u << i; return b; }
static const VkBuffer ZERO = (VkBuffer)(uintptr_t)0x9999;

static int same_descs(const State *s, char *why) {
    VkVertexInputBindingDescription2EXT bd1[17], bd2[17];
    VkVertexInputAttributeDescription2EXT ad1[16], ad2[16];
    VkBuffer vb1[17], vb2[17];
    VkDeviceSize vo1[17], vo2[17];
    uint32_t nb1, na1, nb2, na2;
    int bstream[17];
    VLay l;
    memset(bd1, 0, sizeof bd1); memset(bd2, 0, sizeof bd2); memset(ad1, 0, sizeof ad1); memset(ad2, 0, sizeof ad2);
    vin_dynamic(s->nattr, s->vattr, s->ints, s->sints, s->vstream, s->vbufs, s->voff, ZERO, bd1, ad1, vb1, vo1, &nb1, &na1);
    vl_of(&l, s->nattr, s->vattr, s->ints, s->sints, s->vstream, bound_of(s));
    vl_descs(&l, bd2, ad2, bstream, &nb2, &na2);
    for (uint32_t b = 0; b < nb2; b++) { if (bstream[b] < 0) { vb2[b] = ZERO; vo2[b] = 65536; } else { vb2[b] = s->vbufs[bstream[b]]; vo2[b] = s->voff[bstream[b]]; } }
    if (nb1 != nb2 || na1 != na2) { sprintf(why, "counts %u/%u vs %u/%u", nb1, na1, nb2, na2); return 0; }
    for (uint32_t i = 0; i < nb1; i++) {
        if (memcmp(&bd1[i], &bd2[i], sizeof bd1[i])) { sprintf(why, "binding %u: stride %u/%u rate %d/%d divisor %u/%u", i, bd1[i].stride, bd2[i].stride, bd1[i].inputRate, bd2[i].inputRate, bd1[i].divisor, bd2[i].divisor); return 0; }
        if (vb1[i] != vb2[i] || vo1[i] != vo2[i]) { sprintf(why, "binding %u: buffer or offset", i); return 0; }
    }
    for (uint32_t i = 0; i < na1; i++)
        if (memcmp(&ad1[i], &ad2[i], sizeof ad1[i])) { sprintf(why, "attribute %u: location %u/%u binding %u/%u format %d/%d offset %u/%u", i, ad1[i].location, ad2[i].location, ad1[i].binding, ad2[i].binding, ad1[i].format, ad2[i].format, ad1[i].offset, ad2[i].offset); return 0; }
    /* the pipeline's state from the same key */
    VlState st;
    if (!vl_state(&l, &st, 1)) { sprintf(why, "vl_state refused"); return 0; }
    if (st.vi.vertexBindingDescriptionCount != nb1 || st.vi.vertexAttributeDescriptionCount != na1) { sprintf(why, "vl_state counts"); return 0; }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < nb1; i++) {
        if (st.b[i].binding != bd1[i].binding || st.b[i].stride != bd1[i].stride || st.b[i].inputRate != bd1[i].inputRate) { sprintf(why, "vl_state binding %u", i); return 0; }
        if (bd1[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && bd1[i].divisor != 1) { if (st.d[nd].binding != bd1[i].binding || st.d[nd].divisor != bd1[i].divisor) { sprintf(why, "vl_state divisor %u", i); return 0; } nd++; }
    }
    if ((nd != 0) != (st.vi.pNext != NULL) || (nd && st.dv.vertexBindingDivisorCount != nd)) { sprintf(why, "vl_state divisor list"); return 0; }
    for (uint32_t i = 0; i < na1; i++)
        if (st.a[i].location != ad1[i].location || st.a[i].binding != ad1[i].binding || st.a[i].format != ad1[i].format || st.a[i].offset != ad1[i].offset) { sprintf(why, "vl_state attribute %u", i); return 0; }
    return 1;
}

int main(void) {
    char why[200] = "";
    /* 1. equivalence over random states, as a draw makes them, and with any stream mask */
    int bad = 0, n = 200000, divs = 0;
    for (int t = 0; t < n; t++) {
        State s;
        random_state(&s);
        if (t & 1) draw_streams(&s);
        if (!same_descs(&s, why)) { if (bad++ < 3) printf("  state %d: %s\n", t, why); }
        for (int i = 0; i < 16; i++) divs += s.voff[i] != ~0ull && s.vstream[i].divisor > 1;
    }
    CHECK(bad == 0, "%d random states: the key's descriptions equal the dynamic path's (%d differ; %d streams with a divisor above 1)", n, bad, divs);

    /* 2. what the layout does not read is not in the key; what it reads is */
    State a, b;
    VLay la, lb;
    int ok_unread = 1, ok_read = 1;
    for (int t = 0; t < 20000; t++) {
        random_state(&a); draw_streams(&a);
        b = a;
        uint32_t used = 0;
        for (int i = 0; i < 16 && i < a.nattr; i++) { int nf = (int)(a.vattr[i] & 0xff), st = (int)((a.vattr[i] >> 8) & 31); if (nf && vertex_vkfmt(nf) && st < 16 && a.voff[st] != ~0ull) used |= 1u << st; }
        for (int i = 0; i < 16; i++) if (!(used >> i & 1)) { b.vstream[i].stride += 4; b.vstream[i].divisor += 1; }
        for (int i = 16; i < 32; i++) { b.vstream[i].stride += 8; }
        for (int i = 0; i < 16; i++) if (b.voff[i] != ~0ull) b.voff[i] += 256;                      /* buffer offsets are bound, not keyed */
        vl_of(&la, a.nattr, a.vattr, a.ints, a.sints, a.vstream, bound_of(&a));
        vl_of(&lb, b.nattr, b.vattr, b.ints, b.sints, b.vstream, bound_of(&b));
        if (memcmp(&la, &lb, sizeof la)) ok_unread = 0;
        if (used) {
            int st = __builtin_ctz(used);
            b = a; b.vstream[st].stride += 4;
            vl_of(&lb, b.nattr, b.vattr, b.ints, b.sints, b.vstream, bound_of(&b));
            if (!memcmp(&la, &lb, sizeof la)) ok_read = 0;
        }
    }
    CHECK(ok_unread, "strides and divisors of streams no attribute reads, and buffer offsets, leave the key as it is");
    CHECK(ok_read, "the stride of a stream an attribute reads changes the key");

    /* 3. the empty key (internal draws): no vertex input at all; a refused divisor without the feature */
    VLay z;
    memset(&z, 0, sizeof z);
    VlState st;
    CHECK(vl_state(&z, &st, 0) && st.vi.vertexBindingDescriptionCount == 0 && st.vi.vertexAttributeDescriptionCount == 0, "a zero key has no bindings and no attributes");
    State d;
    memset(&d, 0, sizeof d);
    d.nattr = 1; d.vattr[0] = 1 | 0 << 8; d.vstream[0].stride = 16; d.vstream[0].divisor = 3; d.voff[0] = 0;
    for (int i = 1; i < 16; i++) d.voff[i] = ~0ull;
    vl_of(&la, d.nattr, d.vattr, 0, 0, d.vstream, bound_of(&d));
    CHECK(!vl_state(&la, &st, 0) && vl_state(&la, &st, 1) && st.dv.vertexBindingDivisorCount == 1 && st.d[0].divisor == 3, "a divisor of 3 needs the divisor feature, and is given to the pipeline");

    printf("vlayout: %d of %d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail != 0;
}
