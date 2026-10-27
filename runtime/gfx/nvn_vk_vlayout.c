/* The vertex input of a draw (docs/macos-port.md "The vertex layout in the pipeline key"). Included by nvn_vk_draw.c (needs
 * vertex_vkfmt); tests/macos/test_vlayout.c includes it alone.
 * vin_dynamic: the descriptions for vkCmdSetVertexInputEXT, from the draw's state (VK_EXT_vertex_input_dynamic_state).
 * Without that extension (both Mac drivers) the layout is part of the pipeline key: vl_of makes the key part from the same
 * state, and vl_descs the same descriptions from the key alone, for the pipeline and for the draw's buffer binding. */
typedef struct { uint32_t stride, divisor; } VStream;
/* attr[i]: 0 no attribute (internal draws: no vertex input at all); VL_CONST_*: not supplied, (0, 0, 0, 1) from the constant
 * binding; VL_SUPPLIED | NVN format | stream << 8 | offset << 12. stride, div: of the streams a supplied attribute reads. */
typedef struct { uint32_t attr[16], stride[16], div[16]; } VLay;
#define VL_CONST_F 1u
#define VL_CONST_U 2u
#define VL_CONST_S 3u
#define VL_SUPPLIED 0x80000000u

/* bufs / offs: the buffer of each stream, offs ~0 when the stream has none; vb / vo: what to bind, binding by binding */
static void vin_dynamic(int nattr, const uint32_t *vattr, uint32_t ints, uint32_t sints, const VStream *vstream, const VkBuffer *vbufs, const uint64_t *voff,
                        VkBuffer zero, VkVertexInputBindingDescription2EXT *bd, VkVertexInputAttributeDescription2EXT *ad, VkBuffer *vb, VkDeviceSize *vo, uint32_t *pnb, uint32_t *pna) {
    uint32_t nb = 0, na = 0, bmap[32], need_const = 0;
    memset(bmap, 0xff, sizeof bmap);
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < nattr ? vattr[i] : 0;
        int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
        VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
        memset(&ad[na], 0, sizeof ad[na]);
        ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
        if (f && stream < 16 && voff[stream] != ~0ull) {
            if (bmap[stream] == 0xffffffffu) {
                bmap[stream] = nb;
                memset(&bd[nb], 0, sizeof bd[nb]);
                bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = vstream[stream].stride;
                bd[nb].inputRate = vstream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = vstream[stream].divisor ? vstream[stream].divisor : 1;
                vb[nb] = vbufs[stream]; vo[nb] = voff[stream]; nb++;
            }
            ad[na].binding = bmap[stream]; ad[na].format = f; ad[na].offset = a >> 14; na++;
        } else {                                   /* not supplied: (0, 0, 0, 1), as OpenGL's current attribute value; integer inputs take the integer copy */
            ad[na].binding = 16; ad[na].format = (ints >> i & 1) ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = (ints >> i & 1) ? 16 : 0;
            need_const = 1; na++;
        }
    }
    if (need_const) {
        memset(&bd[nb], 0, sizeof bd[nb]);
        bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
        for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
        vb[nb] = zero; vo[nb] = 65536; nb++;
    }
    *pnb = nb; *pna = na;
}

/* streams: bit s set when stream s has a buffer for the draw (vin_dynamic's voff[s] != ~0) */
static void vl_of(VLay *l, int nattr, const uint32_t *vattr, uint32_t ints, uint32_t sints, const VStream *vstream, uint32_t streams) {
    memset(l, 0, sizeof *l);
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < nattr ? vattr[i] : 0;
        int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
        VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
        if (f && stream < 16 && (streams >> stream & 1)) {
            l->attr[i] = VL_SUPPLIED | (uint32_t)nf | (uint32_t)stream << 8 | (a >> 14) << 12;
            l->stride[stream] = vstream[stream].stride; l->div[stream] = vstream[stream].divisor;
        } else l->attr[i] = (ints >> i & 1) ? ((sints >> i & 1) ? VL_CONST_S : VL_CONST_U) : VL_CONST_F;
    }
}
/* The descriptions of a key, numbered as vin_dynamic numbers them; bstream[b]: the stream of binding b, -1 the constant one. */
static void vl_descs(const VLay *l, VkVertexInputBindingDescription2EXT *bd, VkVertexInputAttributeDescription2EXT *ad, int *bstream, uint32_t *pnb, uint32_t *pna) {
    uint32_t nb = 0, na = 0, bmap[16], need_const = 0;
    memset(bmap, 0xff, sizeof bmap);
    for (int i = 0; i < 16; i++) {
        uint32_t a = l->attr[i];
        if (!a) continue;
        memset(&ad[na], 0, sizeof ad[na]);
        ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
        if (a & VL_SUPPLIED) {
            int stream = (int)((a >> 8) & 15);
            if (bmap[stream] == 0xffffffffu) {
                bmap[stream] = nb;
                memset(&bd[nb], 0, sizeof bd[nb]);
                bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = l->stride[stream];
                bd[nb].inputRate = l->div[stream] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = l->div[stream] ? l->div[stream] : 1;
                bstream[nb++] = stream;
            }
            ad[na].binding = bmap[stream]; ad[na].format = vertex_vkfmt((int)(a & 0xff)); ad[na].offset = (a >> 12) & 0x3ffff; na++;
        } else {
            ad[na].binding = 16; ad[na].format = a == VL_CONST_S ? VK_FORMAT_R32G32B32A32_SINT : a == VL_CONST_U ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = a == VL_CONST_F ? 0 : 16;
            need_const = 1; na++;
        }
    }
    if (need_const) {
        memset(&bd[nb], 0, sizeof bd[nb]);
        bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
        for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
        bstream[nb++] = -1;
    }
    *pnb = nb; *pna = na;
}
/* The pipeline's vertex input state of a key (the arrays live in *st). 0 when a divisor above 1 needs a feature the device lacks. */
typedef struct {
    VkPipelineVertexInputStateCreateInfo vi; VkPipelineVertexInputDivisorStateCreateInfoEXT dv;
    VkVertexInputBindingDescription b[17]; VkVertexInputAttributeDescription a[16]; VkVertexInputBindingDivisorDescriptionEXT d[17];
} VlState;
static int vl_state(const VLay *l, VlState *st, int has_divisor) {
    VkVertexInputBindingDescription2EXT bd[17];
    VkVertexInputAttributeDescription2EXT ad[16];
    int bstream[17];
    uint32_t nb, na, nd = 0;
    vl_descs(l, bd, ad, bstream, &nb, &na);
    memset(st, 0, sizeof *st);
    for (uint32_t i = 0; i < nb; i++) {
        st->b[i].binding = bd[i].binding; st->b[i].stride = bd[i].stride; st->b[i].inputRate = bd[i].inputRate;
        if (bd[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE && bd[i].divisor != 1) {
            if (!has_divisor) return 0;
            st->d[nd].binding = bd[i].binding; st->d[nd].divisor = bd[i].divisor; nd++;
        }
    }
    for (uint32_t i = 0; i < na; i++) { st->a[i].location = ad[i].location; st->a[i].binding = ad[i].binding; st->a[i].format = ad[i].format; st->a[i].offset = ad[i].offset; }
    st->vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    st->vi.vertexBindingDescriptionCount = nb; st->vi.pVertexBindingDescriptions = st->b;
    st->vi.vertexAttributeDescriptionCount = na; st->vi.pVertexAttributeDescriptions = st->a;
    if (nd) {
        st->dv.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
        st->dv.vertexBindingDivisorCount = nd; st->dv.pVertexBindingDivisors = st->d;
        st->vi.pNext = &st->dv;
    }
    return 1;
}
