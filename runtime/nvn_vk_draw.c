/* NVN on Vulkan, the drawing part (session 2; design: plan\phases\V1_vulkan.md). Included at the end of nvn_vk.c: it uses
 * that file's device (V), loader functions and error helpers. Render thread only.
 *
 * What is here: device features, a block allocator, the stream ring (guest memory -> one mapped buffer, at most one copy
 * of a range per batch), images and views made at first use from the front end's texture source, samplers, translated
 * stages -> SPIR-V (smo_glslang.dll, cached in ..\cache\vk\spv) -> pipelines made synchronously, state tracking with
 * dynamic state, dynamic rendering, clears, draws, buffer->texture and texture->texture copies, present by blit, the
 * screenshot of the window texture, fences. Anything else stops with its name (nvn_vk_missing).
 *
 * Debug: SMO_VK_DEBUG=1 turns on the checks that stand in for the validation layer that is not installed (image used
 * after destroy, barrier inside a pass, a view of an image in an unexpected layout) and prints every new pipeline. */
#include <stdarg.h>
#include <math.h>
#include <emmintrin.h>
#include "tex_decode.h"
#include "smo_ui.h"                                    /* UiMenuJob: the amiibo menu's pictures (vk_menu) */
static int have_targets_live(void);

#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkResetQueryPool) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache)
#define X(n) static PFN_##n n;
VKD(X)
#undef X
static PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)

static struct {
    VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids; VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc; VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc;
    const char *exts[16]; uint32_t next;
    int has_vids, has_cbc, has_pv, has_iu8, has_plr, has_vad, has_push, has_eds3, debug;
    int has_gpl, ext_gpl, ext_so;
    int has_dcc;                                   /* SMO: VK_EXT_depth_clip_control (NVN depth mode NEAR_IS_MINUS_W) */
    VkPhysicalDeviceMemoryProperties mp;
} D;
/* SMO: NVN's depth mode is honoured, read when a pipeline is made (the game sets it on the device after nvnDeviceInitialize, which
 * may be after this backend starts). 0 = NEAR_IS_MINUS_W (OpenGL clip z -w..w, NVN's default) needs VK_EXT_depth_clip_control
 * negativeOneToOne; 1 = NEAR_IS_ZERO is Vulkan's own. The window origin must be upper left (the viewport flip assumes it). */
static const void *vk_vp_next(void) {
    static VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, VK_TRUE};
    static int seen = -1;
    int m = nvn_front_clip_modes();
    if (m != seen) {
        seen = m;
        printf("nvn_vk: window origin mode %d, depth mode %d (depth clip control %s)\n", m & 0xff, m >> 8, D.has_dcc ? "yes" : "no"); fflush(stdout);
        if ((m & 0xff) != 1) nvn_vk_missing("a window origin other than upper left (nvnDeviceSetWindowOriginMode)");
        if ((m >> 8) == 0 && !D.has_dcc) nvn_vk_missing("NVN depth mode NEAR_IS_MINUS_W on a GPU without VK_EXT_depth_clip_control");
    }
    return (m >> 8) == 0 && D.has_dcc ? &dcc : NULL;
}
static void vkd_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("nvn_vk: FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); ExitProcess(6);
}
#define DBG_ASSERT(c, ...) do { if (D.debug && !(c)) vkd_fail(__VA_ARGS__); } while (0)
/* Session 11: the pipeline-library mode. SMO_VK_GPL wins (0 = whole pipelines, the path before session 9; 1 = a library per
 * stage pair, a measurement mode; 2 = a library per stage + preparation + barrier); without it VK_GPL_DEFAULT. */
#ifndef VK_GPL_DEFAULT
#define VK_GPL_DEFAULT 2                           /* session 11: on by default where the driver has the extension (validation run s11v3 and the first-run table of V1_vulkan.md "Session 11"); -DVK_GPL_DEFAULT=0 or SMO_VK_GPL=0 for the old path */
#endif
static int vk_gpl_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SMO_VK_GPL"); m = e && e[0] ? atoi(e) : VK_GPL_DEFAULT; } return m; }

/* ---------------------------------------------------------------- device features */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext) {
    void **tail;
    vkGetPhysicalDeviceFeatures2 = vk_sym("vkGetPhysicalDeviceFeatures2");
    vkGetPhysicalDeviceFormatProperties = vk_sym("vkGetPhysicalDeviceFormatProperties");
    vkGetPhysicalDeviceMemoryProperties = vk_sym("vkGetPhysicalDeviceMemoryProperties");
    vkGetDeviceProcAddr = vk_sym("vkGetDeviceProcAddr");
    D.debug = getenv("SMO_VK_DEBUG") && atoi(getenv("SMO_VK_DEBUG"));
    D.f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; D.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    D.f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; D.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    D.f2.pNext = &D.f11; D.f11.pNext = &D.f12; D.f12.pNext = &D.f13; tail = &D.f13.pNext;
    D.exts[D.next++] = "VK_KHR_swapchain";
#define EXT(flag, name, st, stype) if (has_name(dext, ndext, name)) { D.flag = 1; D.exts[D.next++] = name; D.st.sType = stype; *tail = &D.st; tail = &D.st.pNext; }
    EXT(has_vids, "VK_EXT_vertex_input_dynamic_state", vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT)
    EXT(has_cbc, "VK_EXT_custom_border_color", cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT)
    EXT(has_pv, "VK_EXT_provoking_vertex", pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT)
    EXT(has_iu8, "VK_EXT_index_type_uint8", iu8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT)
    EXT(has_plr, "VK_EXT_primitive_topology_list_restart", plr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT)
    EXT(has_vad, "VK_EXT_vertex_attribute_divisor", vad, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
    EXT(has_dcc, "VK_EXT_depth_clip_control", dcc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT)
    if (!(getenv("SMO_VK_EDS3") && !atoi(getenv("SMO_VK_EDS3")))) { EXT(has_eds3, "VK_EXT_extended_dynamic_state3", eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) }
    /* session 9: graphics pipeline libraries (nvn_vk_m4.c "graphics pipeline libraries"): the stage pair is compiled at loading
     * without the pass formats, a draw only LINKS. SMO_VK_GPL=1 turns it on (needs extended dynamic state 3 as well). */
    D.ext_gpl = has_name(dext, ndext, "VK_EXT_graphics_pipeline_library") && has_name(dext, ndext, "VK_KHR_pipeline_library"); D.ext_so = has_name(dext, ndext, "VK_EXT_shader_object");
    if (D.ext_gpl && vk_gpl_mode()) { D.exts[D.next++] = "VK_KHR_pipeline_library"; EXT(has_gpl, "VK_EXT_graphics_pipeline_library", gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT) }
#undef EXT
    if (has_name(dext, ndext, "VK_KHR_push_descriptor") && !(getenv("SMO_VK_PUSH") && !atoi(getenv("SMO_VK_PUSH")))) { D.has_push = 1; D.exts[D.next++] = "VK_KHR_push_descriptor"; }
    vkGetPhysicalDeviceFeatures2(V.gpu, &D.f2);                    /* every supported feature of these structures is enabled */
    D.has_vids = D.has_vids && D.vids.vertexInputDynamicState; D.has_cbc = D.has_cbc && D.cbc.customBorderColors && D.cbc.customBorderColorWithoutFormat;
    D.has_pv = D.has_pv && D.pv.provokingVertexLast; D.has_iu8 = D.has_iu8 && D.iu8.indexTypeUint8; D.has_plr = D.has_plr && D.plr.primitiveTopologyListRestart;
    D.has_dcc = D.has_dcc && D.dcc.depthClipControl;
    /* session 4: blend / mask / polygon / multisample state out of the pipeline key, only when the GPU has ALL of these (else all stay in the key) */
    D.has_eds3 = D.has_eds3 && D.eds3.extendedDynamicState3ColorBlendEnable && D.eds3.extendedDynamicState3ColorBlendEquation && D.eds3.extendedDynamicState3ColorWriteMask && D.eds3.extendedDynamicState3PolygonMode &&
                 D.eds3.extendedDynamicState3AlphaToCoverageEnable && D.eds3.extendedDynamicState3SampleMask && (D.eds3.extendedDynamicState3DepthClampEnable || !D.f2.features.depthClamp);
    printf("nvn_vk: extended dynamic state 3 (blend enable / equation / write mask, polygon mode, alpha to coverage, sample mask, depth clamp dynamic = not in the pipeline key): %s\n", D.has_eds3 ? "yes" : "no");
    D.has_gpl = D.has_gpl && D.gpl.graphicsPipelineLibrary && D.has_eds3 && D.has_vids;
    printf("nvn_vk: driver support for pipelines without a hold: VK_EXT_graphics_pipeline_library %s, VK_EXT_shader_object %s, extended dynamic state 3 (all seven states) %s, vertex input dynamic state %s | pipeline libraries in use: %s\n",
           D.ext_gpl ? "yes" : "NO", D.ext_so ? "yes" : "NO", D.has_eds3 ? "yes" : "NO", D.has_vids ? "yes" : "NO", D.has_gpl ? "YES" : "no");
    /* session 11: ONE line that says which path draws and why */
    {   const char *e = getenv("SMO_VK_GPL");
        int m = vk_gpl_mode();
        printf("nvn_vk_path: %s: %s (SMO_VK_GPL %s; default %d; SMO_VK_GPL=0 is the switch back to whole pipelines)\n",
               D.has_gpl && m >= 2 ? "STAGE LIBRARIES + preparation + loading barrier (a draw links, nothing is compiled in a fight)" : D.has_gpl ? "PAIR LIBRARIES (SMO_VK_GPL=1, a measurement mode)" : "WHOLE PIPELINES (made by the workers from the list of earlier runs; a first use can hold a frame)",
               !m ? (e ? "turned off by SMO_VK_GPL=0" : "off by default in this build") : D.has_gpl ? (e ? "asked for by SMO_VK_GPL" : "the default: the driver has VK_EXT_graphics_pipeline_library, extended dynamic state 3 and dynamic vertex input") :
               !D.ext_gpl ? "the driver has NO VK_EXT_graphics_pipeline_library: fallen back" : "the driver lacks extended dynamic state 3 or dynamic vertex input: fallen back", e ? e : "not set", VK_GPL_DEFAULT);
    }
    if (!D.f13.dynamicRendering || !D.f13.synchronization2) vkd_fail("the device lacks dynamicRendering / synchronization2");
    if (!D.has_vids) nvn_vk_missing("a GPU without VK_EXT_vertex_input_dynamic_state (the vertex layout is not yet part of the pipeline key)");
    if (!D.f2.features.textureCompressionBC) nvn_vk_missing("a GPU without BC texture compression");
    di->enabledExtensionCount = D.next; di->ppEnabledExtensionNames = D.exts; di->pNext = &D.f2;
    printf("nvn_vk: features: vertex input dynamic %d, custom border colour %d, provoking vertex last %d, 8-bit indices %d, list restart %d, ASTC %d, depth clamp %d, wide lines %d, robust buffers %d\n",
           D.has_vids, D.has_cbc, D.has_pv, D.has_iu8, D.has_plr, (int)D.f2.features.textureCompressionASTC_LDR, (int)D.f2.features.depthClamp, (int)D.f2.features.wideLines, (int)D.f2.features.robustBufferAccess);
}

/* ---------------------------------------------------------------- memory: 64 MB blocks per memory type and class, first fit */
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
typedef struct { VkDeviceMemory mem; uint64_t size; void *map; uint32_t type; int cls, nfree, used; struct { uint64_t off, size; } fr[512]; } VBlock;
static VBlock *g_blk[2048];
static int g_nblk;
static LONG64 g_mem_total, g_mem_leak;
static VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls) {
    VMem m = {-1, 0, 0, NULL, NULL};
    uint32_t type = 32;
    for (uint32_t i = 0; i < D.mp.memoryTypeCount && type == 32; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == 32) vkd_fail("no memory type for flags 0x%x (bits 0x%x)", (unsigned)want, (unsigned)rq.memoryTypeBits);
    uint64_t al = rq.alignment < 256 ? 256 : rq.alignment;
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < g_nblk; b++) {
            VBlock *k = g_blk[b];
            if (!k || k->type != type || k->cls != cls) continue;
            for (int i = 0; i < k->nfree; i++) {
                uint64_t start = (k->fr[i].off + al - 1) / al * al, end = k->fr[i].off + k->fr[i].size;
                if (start + rq.size > end) continue;
                uint64_t head = start - k->fr[i].off, tail = end - (start + rq.size);
                if (head && tail) { if (k->nfree == 512) continue; memmove(&k->fr[i + 2], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree++; k->fr[i].size = head; k->fr[i + 1].off = start + rq.size; k->fr[i + 1].size = tail; }
                else if (head) k->fr[i].size = head;
                else if (tail) { k->fr[i].off = start + rq.size; k->fr[i].size = tail; }
                else { memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; }
                m.block = b; m.off = start; m.size = rq.size; m.mem = k->mem; m.map = k->map ? (uint8_t *)k->map + start : NULL; k->used++;
                return m;
            }
        }
        if (pass) break;
        int b = 0;
        while (b < g_nblk && g_blk[b]) b++;
        if (b == 2048) vkd_fail("out of memory blocks");
        VBlock *k = calloc(1, sizeof *k);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        k->size = rq.size > (16u << 20) ? rq.size : (64u << 20); k->type = type; k->cls = rq.size > (16u << 20) ? 100 + b : cls;      /* large: a block of its own */
        ai.allocationSize = k->size; ai.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(V.device, &ai, NULL, &k->mem);
        if (r != VK_SUCCESS) vkd_fail("vkAllocateMemory of %llu MB (type %u): %s; %lld MB allocated so far", (unsigned long long)(k->size >> 20), type, vk_result_name(r), (long long)(g_mem_total >> 20));
        if (D.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(V.device, k->mem, 0, VK_WHOLE_SIZE, 0, &k->map));
        k->nfree = 1; k->fr[0].off = 0; k->fr[0].size = k->size;
        g_blk[b] = k; if (b == g_nblk) g_nblk++;
        g_mem_total += (LONG64)k->size;
        cls = k->cls;
    }
    vkd_fail("allocation of %llu bytes failed in a fresh block", (unsigned long long)rq.size);
    return m;
}
static void vmem_free(VMem m) {
    if (m.block < 0) return;
    VBlock *k = g_blk[m.block];
    if (--k->used == 0 && k->cls >= 100) { vkFreeMemory(V.device, k->mem, NULL); g_mem_total -= (LONG64)k->size; free(k); g_blk[m.block] = NULL; return; }
    int i = 0;
    while (i < k->nfree && k->fr[i].off < m.off) i++;
    if (i > 0 && k->fr[i - 1].off + k->fr[i - 1].size == m.off) { k->fr[i - 1].size += m.size; if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i - 1].size += k->fr[i].size; memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; } return; }
    if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i].off = m.off; k->fr[i].size += m.size; return; }
    if (k->nfree == 512) { g_mem_leak += (LONG64)m.size; return; }
    memmove(&k->fr[i + 1], &k->fr[i], sizeof k->fr[0] * (size_t)(k->nfree - i)); k->nfree++;
    k->fr[i].off = m.off; k->fr[i].size = m.size;
}
static VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkBuffer b;
    VkMemoryRequirements rq;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(V.device, &bi, NULL, &b));
    vkGetBufferMemoryRequirements(V.device, b, &rq);
    *mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    VKC(vkBindBufferMemory(V.device, b, mem->mem, mem->off));
    return b;
}

/* ---------------------------------------------------------------- command buffers in flight */
#define NSLOT 8
typedef struct { VkCommandBuffer cmd; VkFence fence; VkDescriptorPool pool[16]; int npool, ipool, pending; uint64_t serial; LONG64 ts_frame; uint64_t rlap0, rpos0, rlap1, rpos1; } Slot;      /* rlap/rpos: the stream ring (lap, position) when it was begun and submitted */
typedef struct VTex VTex;
static struct {
    Slot slot[NSLOT]; int cur, recording; uint64_t serial, done;      /* serial: number of the last submission; done: every submission up to here has finished */
    VkCommandBuffer cmd;
    /* stream ring */
    VkBuffer ring; VMem ring_mem; uint64_t ring_pos, ring_size, ring_lap; uint32_t gen;
    struct { uint64_t addr, size, off; uint32_t gen, batch, wpos; } acache[8192];      /* wpos: host writes logged up to here were checked (ring_put) */
    VkBuffer zero; VMem zero_mem;
    /* pass */
    int in_pass, pass_w, pass_h;
    int pass_rs; float pass_sx, pass_sy;           /* track O: the pass draws into scaled images (pass_w, pass_h are the images'; sx, sy = image / game) */
    /* NVN state as left by the commands executed so far */
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_tex[6][32], c2[6][112], nvn_img[6][8];      /* nvn_img: nvnCommandBufferBindImage (NVN image handle = texture pool index) */
    uint32_t tex_int[6], tex_sint[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds; uint32_t chanmask, sample_mask;
    int sref[2], svmask[2], swmask[2];
    float alpha_ref; int alpha_func;
    uint8_t blend[8][8], blend_on, logic, poly[4], ms[24];
    float viewport[4], drange[2], blendc[4], point_size, line_width, polyoff[3];
    int scissor[4], have_viewport, have_scissor, rast_discard, depth_clamp, restart, render_off;
    int ncol; TargetRef col[8], depth; int have_targets;
    int64_t row_stride, image_stride;
    int dyn_dirty, vi_dirty, dyn3_dirty, dyn3_samples;      /* dyn3: the extended-dynamic-state-3 group (blend, masks, polygon mode, multisample) */
    void *pipe_bound;
    LONG64 frame_draws, frame_skipped, frame_clears, pipelines, modules, uploads, upload_bytes, images, images_live, views, ring_wraps, feedback, flushes, sets;
    LONG64 sets_reused, tset_collisions, dummy_kind;
    LONG64 dispatches, cb_bytes, cb_syncs, cb_draws, cb_made, cb_waits, cb_wait_ticks, cb_guest_words, cb_conflict_words, readbacks;
} R;

static void pass_shot(void);
/* SMO debug aid SMO_VK_FRAMELOG=<first>-<last>: one line per frame of that range: draws, draws left out (and how many of
 * them because rendering was switched off), conditional render switches (and how many switched rendering off), render
 * passes, and a hash of the conditional results in order, to find frames that differ from their neighbours. */
static LONG64 g_fr_cond, g_fr_condoff, g_fr_offskip, g_fr_passes, g_fr_texbar, g_fr_wraps, g_fr_ring0;
static uint64_t g_fr_condhash = 1469598103934665603ull;
/* SMO debug aid SMO_VK_PASSSHOT=<frame> or <first>-<last>[/<step>]: the frames whose render passes pass_shot saves and
 * whose draws are logged (a range finds a defect that shows in some frames only; its files carry the frame number). */
static long long g_ps_lo = -2, g_ps_hi, g_ps_step = 1;
static void passshot_parse(void) {
    const char *e = getenv("SMO_VK_PASSSHOT"), *d;
    g_ps_lo = -1;
    if (!e) return;
    g_ps_lo = g_ps_hi = atoll(e);
    if ((d = strchr(e, '-')) != NULL) { g_ps_hi = atoll(d + 1); if ((d = strchr(d, '/')) != NULL && atoll(d + 1) > 0) g_ps_step = atoll(d + 1); }
}
static int passshot_frame(void) {
    if (g_ps_lo == -2) passshot_parse();
    long long f = nvn_front_frame_next();
    return g_ps_lo >= 0 && f >= g_ps_lo && f <= g_ps_hi && (f - g_ps_lo) % g_ps_step == 0;
}
static int passshot_range(void) { if (g_ps_lo == -2) passshot_parse(); return g_ps_hi > g_ps_lo; }
/* SMO port: NVN counter type 1, SAMPLES_PASSED (occlusion). Between nvnCommandBufferResetCounter and ReportCounter the
 * samples of every draw are counted. A Vulkan occlusion query begun inside a render pass must end in it, so the count is
 * made of segments: one query from the first draw of a pass while counting until the pass (or the counting) ends.
 * Segments are numbered on without end and use a ring of OCC_MAX queries. A report does not wait for the GPU: it is
 * queued with the segments recorded since the entry before it, and its {count, timestamp} (the sum since the last reset,
 * as nvn_gl.c's counter_stop / acc) is written into guest memory once the submission holding them has finished. Finished
 * submissions are looked for at each batch and while the render thread is idle; the GPU is waited for where a result is
 * needed: a conditional render that compares it, the game's SyncWait or Finish, and the end of a present (the game reads
 * the counts of one frame in the next: agl::cull::OcclusionQuery::getZPassCount). SMO_VK_OCC_SYNC=1 waits at each report
 * as before; SMO_VK_OCC_PRESENT=0 does not wait at the present. */
#define OCC_MAX 65536
#define OCC_Q 4096
typedef struct { uint64_t lo, hi, dst, serial; LONG64 frame; int kind, log; } OccEntry;     /* segments [lo, hi); kind 0 report (to dst), 1 reset (the segments are dropped), 2 fold (added up, nothing written) */
static VkQueryPool g_occ_pool;
static int g_occ_on, g_occ_open;                   /* counting; a segment is open */
static uint64_t g_occ_head, g_occ_tail, g_occ_mark, g_occ_seg_serial;   /* next segment; oldest segment in use; first segment of no entry yet; submission of the newest segment */
static OccEntry g_occ_q[OCC_Q];
static int g_occ_qh, g_occ_qn;                     /* oldest entry, entries queued */
static uint64_t g_occ_total;                       /* samples of the count the resolved entries belong to */
static int g_occ_sync = -1, g_occ_present = -1;
static LONG64 g_occ_reports, g_occ_waits, g_occ_wait_ticks, g_occ_folds, g_occ_cond, g_occ_cond_waits, g_occ_present_waits, g_occ_late;
static void occ_end(void) { if (g_occ_open) { vkCmdEndQuery(R.cmd, g_occ_pool, (uint32_t)((g_occ_head - 1) % OCC_MAX)); g_occ_open = 0; } }
static void pass_end(void) { if (R.in_pass) { occ_end(); vkCmdEndRendering(R.cmd); R.in_pass = 0; pass_shot(); } }
static LONG64 g_tm[12];                          /* ticks: 0 wait for a submission, 1 copy-back, 2 dispatch, 3 batch replay, 4 present, 5 pipelines, 6 modules, 7 copy-back wait at batch start, 8 draw preparation */
static LONG64 tk(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
/* SMO port: report timestamps, the clock nvn_gl.c's now_ns uses (performance counter in nanoseconds) */
static uint64_t vk_now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}
/* Session 5, SMO_VK_GPUTIME=1: two timestamp queries per command buffer (written at its first and after its last command),
 * read when the buffer has finished; "nvn_vk_gpu:" line every 300 frames. The span of a buffer includes the time the GPU
 * gave to other processes while it ran. */
static int g_ts_on = -1;
static VkQueryPool g_ts_pool;
static double g_ts_ns;
static LONG64 g_ts_n;
/* Session 9: the same per FRAME (the frame a buffer was recorded for), with the time between two presents and whether the frame
 * was a battle frame (compute dispatches and more than 120 draws): median / p99 of the battle frames at exit ("nvn_vk_gpu_fight:"). */
#define FRSTAT_N 40000
static float g_fr_gpu[FRSTAT_N], g_fr_wall[FRSTAT_N];
static uint8_t g_fr_fight[FRSTAT_N];
static LONG64 g_cur_frame, g_battle_frames;
void nvn_tl(int ev, int arg);                     /* nvn_gl.c: SMO_FRAMETL (13/14: a wait for the GPU begins / ends) */
static void slot_wait(Slot *s) {
    if (!s->pending) return;
    LONG64 t0 = tk();
    nvn_tl(13, (int)(s - R.slot));
    VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, 20000000000ull);
    nvn_tl(14, (int)(s - R.slot));
    g_tm[0] += tk() - t0;
    if (r != VK_SUCCESS) vkd_fail("a submitted command buffer did not finish within 20 s (%s): the GPU is stuck or the device is lost", vk_result_name(r));
    VKC(vkResetFences(V.device, 1, &s->fence));
    if (g_ts_on > 0) {                             /* SMO_VK_GPUTIME=1: the GPU's own clock at the first and the last command of this buffer */
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(V.device, g_ts_pool, (uint32_t)(s - R.slot) * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) { double ns = (double)(ts[1] - ts[0]) * (double)V.props.limits.timestampPeriod; g_ts_ns += ns; g_ts_n++; if (s->ts_frame >= 0 && s->ts_frame < FRSTAT_N) g_fr_gpu[s->ts_frame] += (float)(ns / 1e6); }
    }
    s->pending = 0;
    if (s->serial > R.done) R.done = s->serial;
}
static void garbage_collect(void);
static uint32_t g_tset_gen = 1;                   /* texture descriptor sets live as long as the command buffer whose pools they come from */
static int g_tset_used, g_tset_list_used;
static void cmd_begin(void) {
    if (R.recording) return;
    g_tset_gen++; g_tset_used = 0; g_tset_list_used = 0;
    R.cur = (R.cur + 1) % NSLOT;
    Slot *s = &R.slot[R.cur];
    slot_wait(s);
    for (int i = 0; i < s->npool; i++) VKC(vkResetDescriptorPool(V.device, s->pool[i], 0));
    s->ipool = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(s->cmd, 0));
    VKC(vkBeginCommandBuffer(s->cmd, &bi));
    s->rlap0 = R.ring_lap; s->rpos0 = R.ring_pos;
    R.cmd = s->cmd; R.recording = 1; R.in_pass = 0; R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1;
    if (g_ts_on < 0) {
        g_ts_on = getenv("SMO_VK_GPUTIME") && atoi(getenv("SMO_VK_GPUTIME")) && V.props.limits.timestampComputeAndGraphics;
        if (g_ts_on) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * 2; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_ts_pool)); }
    }
    s->ts_frame = g_cur_frame;
    if (g_ts_on > 0) { vkCmdResetQueryPool(s->cmd, g_ts_pool, (uint32_t)R.cur * 2, 2); vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2); }
    garbage_collect();
}
/* Ends the command buffer being recorded and gives it to the queue. Returns its number (the last one when nothing was recorded). */
static uint64_t cmd_submit(VkSemaphore wait, VkSemaphore signal) {
    if (!R.recording) { if (!wait && !signal) return R.serial; cmd_begin(); }
    pass_end();
    Slot *s = &R.slot[R.cur];
    if (g_ts_on > 0) vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2 + 1);
    VKC(vkEndCommandBuffer(s->cmd));
    VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait) { sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &wait; sb.pWaitDstStageMask = &stage; }
    if (signal) { sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &signal; }
    sb.commandBufferCount = 1; sb.pCommandBuffers = &s->cmd;
    nvn_tl(15, (int)R.frame_draws);               /* SMO_FRAMETL: a submission, with the draws of the frame so far */
    VKC(vkQueueSubmit(V.queue, 1, &sb, s->fence));
    s->pending = 1; s->serial = ++R.serial;
    s->rlap1 = R.ring_lap; s->rpos1 = R.ring_pos;
    R.recording = 0; R.cmd = NULL; R.flushes++;
    return R.serial;
}
static void cmd_wait_all(void) { cmd_submit(NULL, NULL); for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]); R.done = R.serial; }

/* ---------------------------------------------------------------- stream ring */
#define RING_ALIGN 256
static void cb_ring_wrap(void);
/* SMO port: at a wrap the ring used to wait for the GPU to finish everything (cmd_wait_all), about every 13 frames in
 * Cascade Kingdom (256 MB, about 20 MB streamed a frame): a frame of 20+ ms each time. What is written from position 0 on
 * was written in the lap before by submissions long finished, so now a submission still running is waited for only when
 * what it read from the lap before overlaps the bytes about to be written. The compute write masks stay where they are
 * until the copy-back reads them (cb_land, at the next batch at the latest), long before the ring comes round to them
 * again. SMO_VK_RING_DRAIN=1: wait for everything at a wrap, as before. */
static int g_ring_drain = -1;
static LONG64 g_ring_waits, g_ring_bytes;
/* The range caches (the ring's copies of guest ranges, the largest index of an index range) hold for one generation,
 * R.gen: a new batch starts one (the game writes its memory between batches), and so does a ring wrap. The host's own
 * writes into guest memory during a batch (UpdateUniformBuffer, reports and timestamps, occlusion results, buffer copies
 * and clears, compute results copied back) used to start one as well, about 60 times a frame in Cascade Kingdom (52 of
 * them UpdateUniformBuffer), and 9 of the 22 MB the ring copied each frame were ranges copied again within their batch.
 * Now such a write is logged with its range, and a cached range is checked against the writes logged since it was copied
 * or last checked: only a range a write overlaps is copied (or scanned) again. A full log starts a new generation.
 * Counted for the perf line every 300 frames (why: 0 batch, 1 ring wrap, 2 copy-back of compute results, 3 occlusion
 * results, 4 UpdateUniformBuffer, 5 report value or timestamp, 6 buffer copy or clear, 7 texture read into a buffer,
 * 8 write log full, 9 a logged write overlaps the index range of a draw: index_max keeps its cache by generation only). */
enum { GEN_BATCH, GEN_WRAP, GEN_CB, GEN_OCC, GEN_UBO, GEN_REPORT, GEN_BUF, GEN_TEXBUF, GEN_LOGFULL, GEN_INDEXW, GEN_N };
static LONG64 g_gen_why[GEN_N], g_wlog_why[GEN_N];
#define WLOG_N 1024
static struct { uint64_t lo, hi; } g_wlog[WLOG_N];
static uint32_t g_wlog_n;                          /* host writes logged in this generation */
#define GEN_BUMP(why) (R.gen++, g_wlog_n = 0, g_gen_why[why]++)
static void host_wrote(uint64_t addr, uint64_t size, int why) {
    if (!size) return;
    if (g_wlog_n == WLOG_N) { GEN_BUMP(GEN_LOGFULL); return; }
    g_wlog[g_wlog_n].lo = addr; g_wlog[g_wlog_n].hi = addr + size; g_wlog_n++;
    g_wlog_why[why]++;
}
static int wlog_overlap(uint32_t from, uint64_t lo, uint64_t hi) {      /* a write logged since `from` overlaps [lo, hi) */
    for (uint32_t w = from; w < g_wlog_n; w++) if (g_wlog[w].lo < hi && lo < g_wlog[w].hi) return 1;
    return 0;
}
static LONG64 g_rput_calls, g_rput_hits, g_rput_again, g_rput_again_bytes;      /* ring_put: calls, cache hits, copies of a range already copied in this batch */
static uint32_t g_batch_no;
static void ring_reuse(uint64_t lo, uint64_t hi) {   /* [lo, hi) of this lap is about to be written: whoever read it in the lap before has finished */
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending) continue;                 /* finished, or the command buffer being recorded (pending only once submitted) */
        uint64_t a, b;                             /* what the submission wrote in the lap before this one */
        if (s->rlap0 + 1 == R.ring_lap) { a = s->rpos0; b = s->rlap1 + 1 == R.ring_lap ? s->rpos1 : R.ring_size; }
        else continue;                             /* begun in this lap (behind the write position) or two laps ago (finished: slots are reused in turn) */
        if (a < hi && lo < b) { slot_wait(s); g_ring_waits++; }
    }
}
static uint64_t ring_raw(const void *p, uint64_t n) {
    R.ring_pos = (R.ring_pos + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1);
    if (n > R.ring_size / 4) vkd_fail("a range of %llu bytes does not fit the stream ring (%llu MB)", (unsigned long long)n, (unsigned long long)(R.ring_size >> 20));
    if (g_ring_drain < 0) g_ring_drain = getenv("SMO_VK_RING_DRAIN") && atoi(getenv("SMO_VK_RING_DRAIN"));
    if (R.ring_pos + n > R.ring_size) {
        if (g_ring_drain) { cmd_wait_all(); cb_ring_wrap(); }      /* everything that reads the ring has finished */
        R.ring_pos = 0; R.ring_lap++; GEN_BUMP(GEN_WRAP); R.ring_wraps++; g_fr_wraps++;
    }
    if (!g_ring_drain && R.ring_lap) ring_reuse(R.ring_pos, R.ring_pos + n);
    uint64_t off = R.ring_pos;
    if (p) memcpy((uint8_t *)R.ring_mem.map + off, p, (size_t)n);
    R.ring_pos += n; g_ring_bytes += (LONG64)n;
    return off;
}
static uint64_t ring_put(uint64_t addr, uint64_t size) {
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    g_rput_calls++;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (R.acache[i].gen == R.gen && R.acache[i].addr == addr && R.acache[i].size == size) {
            if (R.acache[i].wpos == g_wlog_n || !wlog_overlap(R.acache[i].wpos, addr, addr + size)) { R.acache[i].wpos = g_wlog_n; g_rput_hits++; return R.acache[i].off; }
            h = i; break;                          /* the host wrote into the range since it was copied: copied again, same entry */
        }
        if (R.acache[i].gen != R.gen) { h = i; break; }
    }
    h &= 8191;
    if (R.acache[h].addr == addr && R.acache[h].size == size && R.acache[h].batch == g_batch_no) { g_rput_again++; g_rput_again_bytes += (LONG64)size; }
    uint64_t off = ring_raw(P(addr), size);
    R.acache[h].addr = addr; R.acache[h].size = size; R.acache[h].off = off; R.acache[h].gen = R.gen; R.acache[h].batch = g_batch_no; R.acache[h].wpos = g_wlog_n;      /* after ring_raw: a wrap starts a generation */
    return off;
}
/* Constant buffer 2 of a stage (R.c2: the texture sizes the translated shaders read) in the ring: copied again only when
 * a texture binding changed it or a new generation began (it used to be copied at every draw: 2.2 MB a frame). */
static struct { uint64_t off; uint32_t gen; int valid; } g_c2r[6];
static uint64_t c2_ring(int s) {
    if (g_c2r[s].valid && g_c2r[s].gen == R.gen) return g_c2r[s].off;
    uint64_t off = ring_raw(R.c2[s], sizeof R.c2[s]);
    g_c2r[s].off = off; g_c2r[s].gen = R.gen; g_c2r[s].valid = 1;
    return off;
}

/* ---------------------------------------------------------------- formats */
static VkFormat comp_fmt(int comps, int i) {
    static const int b8[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}, b16[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
                     b32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const struct { uint8_t w, o; } m[12] = {{8, 0}, {8, 1}, {8, 4}, {8, 5}, {16, 6}, {16, 0}, {16, 1}, {16, 4}, {16, 5}, {32, 2}, {32, 0}, {32, 1}};      /* offsets: UNORM 0 SNORM 1 USCALED 2 SSCALED 3 UINT 4 SINT 5 SFLOAT 6; 32-bit: UINT 0 SINT 1 SFLOAT 2 */
    return (VkFormat)((m[i].w == 8 ? b8 : m[i].w == 16 ? b16 : b32)[comps - 1] + m[i].o);
}
static int fmt_supported(VkFormat f, VkFlags need) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(V.gpu, f, &p); return (p.optimalTilingFeatures & need) == need; }
/* NVN texture format -> Vulkan. *expand = 1 + component kind when a 3-component format is stored with 4; *decode = 1 when the texels are decoded to RGBA8 on the CPU. */
static VkFormat tex_vkfmt(int nf, int *expand, int *decode) {
    static int d24 = -1, d24s8, astc;
    if (d24 < 0) { d24 = fmt_supported(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); d24s8 = fmt_supported(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); astc = D.f2.features.textureCompressionASTC_LDR != 0; }
    *expand = 0; *decode = 0;
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (comps == 3) { *expand = 1 + i; comps = 4; }
        return comp_fmt(comps, i);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        if (astc) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * ((nf - 0x79) % 14) + (nf >= 0x87));
        *decode = 1;
        return nf >= 0x87 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    switch (nf) {
    case 0x31: return VK_FORMAT_S8_UINT; case 0x32: return VK_FORMAT_D16_UNORM; case 0x33: return d24 ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT; case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT; case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB; case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 0x3A: case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK; case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK; case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK; case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK; case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK; case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK; case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK; case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK; case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM; case 0x9B: case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}
static VkFlags fmt_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkFormat vertex_vkfmt(int nf) {
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) return comp_fmt(nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12);
    if (nf >= 0x51 && nf <= 0x68) {
        int comps = (nf - 0x51) / 6 + 1, t = (nf - 0x51) % 6;
        if (t >= 4) return VK_FORMAT_UNDEFINED;                       /* 32-bit scaled: Vulkan has none */
        return (VkFormat)(comp_fmt(comps, t < 2 ? 0 : 5) + 2 + (t & 1));
    }
    switch (nf) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

/* ---------------------------------------------------------------- images */
typedef struct VView { EView e; int vtype, attach; VkImageView view; struct VView *next; } VView;
static LONG64 g_storage_tex;                       /* SMO: textures whose image has storage use (vt_get2) */
struct VTex {
    uint32_t magic; Tex *t; VkImage img; VMem mem; VkFormat fmt; VkImageLayout layout; VkFlags aspect;
    int w, h, d, levels, layers, samples, expand, decode, is3d, feedback, bpb, bw, bh;
    int rs; float sx, sy;                          /* track O: rs = 1 when the image is smaller than the game's texture (w, h are the image's; sx, sy = image / game) */
    int storage;                                   /* SMO: made with VK_IMAGE_USAGE_STORAGE_BIT (NVN_TEXTURE_FLAGS_IMAGE): it can be bound as an image */
    VView *views; VTex *next, *prev;
};
/* ---- Track O: internal resolution scale (graphics.ini resolution_scale; SMO_VK_SCALE wins; plan\phases\O1_options.md, session 7).
 * A texture of the screen class (2D, one layer, attachable, the game's size exactly 1920x1080 >> k, k = 0..6) gets an image
 * of round(size * scale); everything the game says in ITS pixels about such an image is converted where it is used:
 * viewport, scissor, clear rectangles, copy / blit / resolve / downsample regions, uploads and read-backs (through a
 * temporary image of the game's size), the crop of the present. The size table of the shaders (c2) and the regions the game
 * reads keep the game's sizes. Fragment stages are translated with MxIn.screen_scale (another hash: ssrc_hb). At 1.0 none
 * of this runs: every branch is behind RS_ON or VTex.rs / R.pass_rs, which are then 0. */
static float g_rs = -1.0f;
static LONG64 g_rs_images, g_rs_stages, g_rs_fcs, g_rs_tf, g_rs_up, g_rs_down, g_rs_blits;
static float rs_scale(void) {
    if (g_rs < 0) {
        const char *e = getenv("SMO_VK_SCALE");
        float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
        if (v < 0.25f) v = 0.25f;
        if (v > 1.0f) v = 1.0f;                    /* above 1.0 (supersampling) is not offered: not tested */
        g_rs = v;
    }
    return g_rs;
}
#define RS_ON (rs_scale() != 1.0f)
static int rs_screen_class(int w, int h) { for (int k = 0; k < 7; k++) if (w == 1920 >> k && h == 1080 >> k) return 1; return 0; }
/* A coordinate of the game's grid on a scaled image (nearest image texel edge, inside 0..max). */
static int rs_edge(int v, float s, int max) { int r = (int)floorf((float)v * s + 0.5f); return r < 0 ? 0 : r > max ? max : r; }
/* Session 9: the textures of the game's window (nvn_gl.c win_initialize keeps the list; also noted here at every present).
 * SMO_VK_UI_NATIVE=1 (resolution scale, experiment): a window texture is NOT scaled, so what the game draws straight into it
 * (its last full-screen pass and the 2D interface after it) is drawn at the game's 1920x1080. */
int nvn_front_tex_is_window(const Tex *t);
static Tex *g_wintex[8];
static LONG64 g_rs_mixed;
static int wintex_is(const Tex *t) { if (nvn_front_tex_is_window(t)) return 1; for (int i = 0; i < 8; i++) if (g_wintex[i] == t) return 1; return 0; }
static void wintex_note(Tex *t) { if (!t || wintex_is(t)) return; for (int i = 0; i < 8; i++) if (!g_wintex[i]) { g_wintex[i] = t; return; } }
static int rs_ui_native(void) { static int on = -1; if (on < 0) on = getenv("SMO_VK_UI_NATIVE") && atoi(getenv("SMO_VK_UI_NATIVE")); return on; }
static VTex *g_vtex_all;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap;
static VkFlags layout_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT; case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT; case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}
static void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    DBG_ASSERT(!R.in_pass, "debug: an image layout change inside a render pass");
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = layout_access(from); b.dstAccessMask = layout_access(to); b.oldLayout = from; b.newLayout = to;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void vt_layout(VTex *vt, VkImageLayout to) {
    if (vt->layout == to) return;
    pass_end();
    cmd_begin();
    image_barrier(vt->img, vt->aspect, vt->layout, to);
    vt->layout = to;
}
/* SMO port: nvnCommandBufferBarrier with ORDER_FRAGMENTS (2) / ORDER_FRAGMENTS_TILED (4): the draws after it read what
 * the draws before it rendered (a texture that is also a render target of the pass, or was one of the pass before
 * without a change of layout, e.g. GENERAL). The OpenGL path answers with glTextureBarrier; here the pass is ended and
 * every attachment / shader / copy write so far is made visible to every read after it (the next draw begins the pass
 * again, its targets loaded). Between passes the layout changes already order what one pass writes and the next reads;
 * the case left is a pass that samples its own target (GENERAL layout), where without a barrier the texture cache may
 * still hold what was there before. SMO_VK_NO_TEXBARRIER=1: as before (no barrier). */
static LONG64 g_texbar, g_texbar_split, g_texbar_skip;
/* SMO port: the vertices a draw really reads. nvnCommandBufferBindVertexBuffer names a range, and some of SMO's draws read
 * past it: the composite passes of its half-size buffers (programs #12026 and #23331 in Cascade Kingdom, and 15 more at
 * the title screen) draw 9 vertices from a range of 36 bytes, 3 vertices of 12; the other 6 follow in the game's memory,
 * which is what the console reads. The copy into the stream ring stopped at the range, so the GPU took those 6 vertices
 * from whatever the ring held after it: stray triangles in the half-size buffer, upscaled into a quarter or a half of
 * the screen for a frame (the flicker: single frames with a white or pale wedge, or half the screen white; in Cascade
 * Kingdom 2 to 9 frames in 361 while the camera moved, and 0 since). Each stream is now copied as far as the draw reads:
 * the highest vertex (plain draws: first + count; indexed draws: the largest index, found once per index range and batch)
 * and, for per-instance streams, the highest instance; only inside the game's heap, and by 16 MB at most (else left as
 * bound, counted). SMO_VK_VTXGROW=0: as before; SMO_VK_VTXCHECK=1 logs each kind of draw that reads past its range. */
static int g_vtxcheck = -1, g_vtxgrow = -1;
static LONG64 g_vtx_over, g_fr_vtxover, g_vtx_capped, g_imax_scans;
static uint64_t g_vneed[16];                       /* bytes of each vertex stream the next draw reads (0: not known) */
int smo_heap_section_offset(uint64_t addr, uint64_t size, HANDLE *section, uint64_t *offset);      /* smo_nn.c: inside the game's committed heap? */
static unsigned vattr_size(int nf) {               /* bytes of one vertex attribute of NVN format nf (as vertex_vkfmt maps it) */
    static const uint8_t w[12] = {8, 8, 8, 8, 16, 16, 16, 16, 16, 32, 32, 32};
    if (nf >= 0x01 && nf <= 0x30) return (unsigned)((nf - 1) / 12 + 1) * w[(nf - 1) % 12] / 8;
    if (nf >= 0x6D && nf <= 0x78) return 4u * w[nf - 0x6D] / 8;
    if (nf >= 0x51 && nf <= 0x68) { int t = (nf - 0x51) % 6; return (unsigned)((nf - 0x51) / 6 + 1) * (t < 2 ? 1u : t < 4 ? 2u : 4u); }
    return 4;
}
static void vtx_switches(void) {
    if (g_vtxgrow >= 0) return;
    g_vtxgrow = !(getenv("SMO_VK_VTXGROW") && !atoi(getenv("SMO_VK_VTXGROW")));
    g_vtxcheck = getenv("SMO_VK_VTXCHECK") && atoi(getenv("SMO_VK_VTXCHECK"));
}
/* The largest index of [addr, addr + count) of index type t (0 8-bit, 1 16-bit, 2 32-bit), restart indices left out;
 * remembered for the batch (R.gen: guest memory may change between batches). 16 bytes at a time (SSE2: the host code is
 * built at -O1, which does not vectorize): one index at a time it was 7% of the render thread in Cascade Kingdom, about
 * 1.2 ms a frame. Unsigned maxima: bytes directly, 16- and 32-bit indices with the top bit flipped compared as signed;
 * a restart index counts as 0. */
static uint32_t index_max(uint64_t addr, int count, unsigned t) {
    static struct { uint64_t addr; uint32_t gen, maxi; int n; uint8_t t, rs; } c[2048];
    unsigned h = (unsigned)(((addr >> 1) ^ (uint64_t)count * 0x9E3779B1u) * 2654435761u) >> 21;
    if (c[h].addr == addr && c[h].n == count && c[h].t == (uint8_t)t && c[h].gen == R.gen && c[h].rs == (uint8_t)R.restart) return c[h].maxi;
    const uint8_t *src = P(addr);
    uint32_t mx = 0;
    int k = 0, per = 16 >> t;                      /* indices in 16 bytes */
    __m128i ones = _mm_set1_epi8(-1), rs = R.restart ? ones : _mm_setzero_si128();
    if (t == 0) {
        __m128i m = _mm_setzero_si128();
        for (; k + 16 <= count; k += 16) { __m128i v = _mm_loadu_si128((const __m128i *)(src + k)); m = _mm_max_epu8(m, _mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi8(v, ones), rs), v)); }
        uint8_t b[16]; _mm_storeu_si128((__m128i *)b, m);
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    } else if (t == 1) {
        __m128i flip = _mm_set1_epi16((short)0x8000), m = flip;
        for (; k + 8 <= count; k += 8) { __m128i v = _mm_loadu_si128((const __m128i *)(src + 2 * (size_t)k)); m = _mm_max_epi16(m, _mm_xor_si128(_mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi16(v, ones), rs), v), flip)); }
        uint16_t b[8]; _mm_storeu_si128((__m128i *)b, _mm_xor_si128(m, flip));
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    } else {
        __m128i flip = _mm_set1_epi32((int)0x80000000u), m = flip;
        for (; k + 4 <= count; k += 4) {
            __m128i v = _mm_loadu_si128((const __m128i *)(src + 4 * (size_t)k));
            v = _mm_xor_si128(_mm_andnot_si128(_mm_and_si128(_mm_cmpeq_epi32(v, ones), rs), v), flip);
            __m128i gt = _mm_cmpgt_epi32(v, m);
            m = _mm_or_si128(_mm_and_si128(gt, v), _mm_andnot_si128(gt, m));
        }
        uint32_t b[4]; _mm_storeu_si128((__m128i *)b, _mm_xor_si128(m, flip));
        for (int i = 0; i < per; i++) if (b[i] > mx) mx = b[i];
    }
    if (t == 0) { for (; k < count; k++) { uint32_t v = src[k]; if (v > mx && !(R.restart && v == 0xFFu)) mx = v; } }
    else if (t == 1) { const uint16_t *p16 = (const uint16_t *)src; for (; k < count; k++) { uint32_t v = p16[k]; if (v > mx && !(R.restart && v == 0xFFFFu)) mx = v; } }
    else { const uint32_t *p32 = (const uint32_t *)src; for (; k < count; k++) { uint32_t v = p32[k]; if (v > mx && !(R.restart && v == 0xFFFFFFFFu)) mx = v; } }
    c[h].addr = addr; c[h].n = count; c[h].t = (uint8_t)t; c[h].gen = R.gen; c[h].rs = (uint8_t)R.restart; c[h].maxi = mx;
    g_imax_scans++;
    return mx;
}
/* maxv: the highest vertex the draw reads; maxi: the highest instance. Fills g_vneed for the streams that read past
 * their range (only those: the others keep the game's range and its copy). */
static void vtx_need(uint64_t maxv, uint64_t maxi, int indexed) {
    static struct { int vs, fs, st; uint64_t size; } seen[256];
    static int nseen;
    memset(g_vneed, 0, sizeof g_vneed);
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        uint32_t a = R.vattr[i];
        if (!(a & 0xff)) continue;
        int st = (int)((a >> 8) & 31);
        if (st >= 16 || !R.vbuf[st].addr) continue;
        uint64_t last = R.vstream[st].divisor ? maxi / R.vstream[st].divisor : maxv;
        uint64_t need = last * R.vstream[st].stride + (a >> 14) + vattr_size((int)(a & 0xff));
        if (need <= R.vbuf[st].size || need <= g_vneed[st]) continue;
        if (need > R.vbuf[st].size + (16u << 20)) { g_vtx_capped++; continue; }
        { HANDLE hs; uint64_t ho; if (!smo_heap_section_offset(R.vbuf[st].addr, need, &hs, &ho)) { g_vtx_capped++; continue; } }      /* the bytes past the range must be the game's memory (smo_nn.c) */
        g_vneed[st] = need;
    }
    for (int st = 0; st < 16; st++) {
        if (!g_vneed[st]) continue;
        g_vtx_over++; g_fr_vtxover++;
        if (!g_vtxcheck) break;
        int vs = R.prog[0] ? R.prog[0]->serial : -1, fs = R.prog[1] ? R.prog[1]->serial : -1, k;
        for (k = 0; k < nseen && !(seen[k].vs == vs && seen[k].fs == fs && seen[k].st == st && seen[k].size == R.vbuf[st].size); k++) {}
        if (k == nseen && nseen < 256) {
            seen[nseen].vs = vs; seen[nseen].fs = fs; seen[nseen].st = st; seen[nseen].size = R.vbuf[st].size; nseen++;
            printf("nvn_vk_vtx: frame %lld: %s draw of programs #%d/#%d reads stream %d (stride %u%s) to byte %llu, the game bound %llu%s\n",
                   nvn_front_frame_next(), indexed ? "indexed" : "plain", vs, fs, st, R.vstream[st].stride, R.vstream[st].divisor ? ", per instance" : "",
                   (unsigned long long)g_vneed[st], (unsigned long long)R.vbuf[st].size, g_vtxgrow ? " (copied that far)" : "");
        }
        break;
    }
    if (!g_vtxgrow) memset(g_vneed, 0, sizeof g_vneed);
}
static void tex_barrier(void) {
    static int off = -1;
    if (off < 0) off = getenv("SMO_VK_NO_TEXBARRIER") && atoi(getenv("SMO_VK_NO_TEXBARRIER"));
    if (off) return;
    if (R.in_pass) {                               /* SMO issues one at almost every pass (about 60 a frame): only a pass that samples its own target needs it */
        int fb = R.depth.tex && R.depth.tex->vk && ((VTex *)R.depth.tex->vk)->feedback;
        for (int i = 0; i < R.ncol && !fb; i++) fb = R.col[i].tex && R.col[i].tex->vk && ((VTex *)R.col[i].tex->vk)->feedback;
        if (!fb) { g_texbar_skip++; return; }      /* the layout change after the pass carries the barrier for what reads it later */
        pass_end(); g_texbar_split++;
    }
    cmd_begin();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    g_texbar++; g_fr_texbar++;
}
static void vt_check(VTex *vt) { if (vt->magic != 0x56544558) vkd_fail("an image is used after it was destroyed (texture record %p)", (void *)vt); }
static void vt_destroy(VTex *vt) {
    for (VView *v = vt->views, *n; v; v = n) { n = v->next; vkDestroyImageView(V.device, v->view, NULL); free(v); }
    vkDestroyImage(V.device, vt->img, NULL);
    vmem_free(vt->mem);
    if (vt->prev) vt->prev->next = vt->next; else g_vtex_all = vt->next;
    if (vt->next) vt->next->prev = vt->prev;
    vt->magic = 0xDEADDEAD; R.images_live--;
    free(vt);
}
static void garbage_collect(void) {
    int n = 0;
    LONG64 t0 = g_ngarbage ? tk() : 0;
    for (int i = 0; i < g_ngarbage; i++) if (g_garbage[i].serial <= R.done) vt_destroy(g_garbage[i].vt); else g_garbage[n++] = g_garbage[i];
    if (t0 && g_ngarbage - n >= 50) {              /* session 8: what destroying many images at once costs (the end of a screen) */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double ms = (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart;
        static int nl;
        if (ms > 10.0 && nl++ < 40) printf("nvn_vk_tex: frame %lld: %d images destroyed in %.1f ms\n", (long long)nvn_front_frame_next(), g_ngarbage - n, ms);
    }
    g_ngarbage = n;
}
static void vk_drop(int kind, void *host) {
    if (kind != K_TEX) return;
    Tex *t = host;
    VTex *vt = t->vk;
    if (!vt) return;
    t->vk = NULL;
    for (int i = 0; i < 8; i++) if (R.col[i].tex == t) { pass_end(); R.col[i].tex = NULL; }
    if (R.depth.tex == t) { pass_end(); R.depth.tex = NULL; }
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;       /* the command buffer being recorded may still name it */
}
static int lvl_(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Track O. A temporary image of w x h (one level, one layer, the format of `like`) for an upload into, or a read-back from,
 * an image that is smaller than the game's texture. The command buffer is being recorded; the image goes when it has finished. */
static VTex *rs_temp(VTex *like, int w, int h) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = like->fmt; vt->aspect = like->aspect; vt->w = w; vt->h = h; vt->d = 1; vt->levels = 1; vt->layers = 1; vt->samples = 1;
    vt->bpb = like->bpb; vt->bw = like->bw; vt->bh = like->bh;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;
    return vt;
}
static VkFilter rs_filter(VTex *vt) { return vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
/* Texels of the game's grid (w x h at `off` of the stream ring, for the rectangle x, y of one level) into a scaled image:
 * through a temporary image of the game's size and a filtered blit. The command buffer is being recorded, no pass is open. */
static void rs_upload(VTex *vt, int level, int layer, uint64_t off, uint32_t rowlen, uint32_t rows, int x, int y, int w, int h) {
    int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
    VkBufferImageCopy c;
    VkImageBlit b;
    if (w <= 0 || h <= 0) return;
    if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) { nvn_vk_missing("resolution scale: an upload into a scaled texture whose format cannot be blitted"); return; }
    VTex *tmp = rs_temp(vt, w, h);
    memset(&c, 0, sizeof c); memset(&b, 0, sizeof b);
    c.bufferOffset = off; c.bufferRowLength = rowlen; c.bufferImageHeight = rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(R.cmd, R.ring, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.mipLevel = (uint32_t)level; b.dstSubresource.baseArrayLayer = (uint32_t)layer; b.dstSubresource.layerCount = 1;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = rs_edge(x, vt->sx, lw); b.dstOffsets[0].y = rs_edge(y, vt->sy, lh); b.dstOffsets[1].x = rs_edge(x + w, vt->sx, lw); b.dstOffsets[1].y = rs_edge(y + h, vt->sy, lh); b.dstOffsets[1].z = 1;
    if (b.dstOffsets[1].x <= b.dstOffsets[0].x || b.dstOffsets[1].y <= b.dstOffsets[0].y) return;
    vkCmdBlitImage(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
    g_rs_up++;
}
static VTex *g_put_vt;
static uint32_t g_fmt_hist[256];                   /* images made per NVN format (printed with the totals) */
static void vt_put(Tex *t, int level, int layer, int w, int h, int d, const void *data, size_t size, int rgba8, int64_t row_stride) {
    VTex *vt = g_put_vt;
    uint8_t *tmp = NULL;
    (void)t;
    if (level >= vt->levels) return;
    if (vt->expand && !rgba8) {                    /* 3 components -> 4, the fourth is one */
        static const uint8_t csize[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
        static const uint32_t one[12] = {255, 127, 1, 1, 0x3C00, 65535, 32767, 1, 1, 0x3F800000, 1, 1};
        int cs = csize[vt->expand - 1];
        size_t n = (size_t)w * h * d, stride = row_stride ? (size_t)row_stride : (size_t)w * 3 * cs;
        tmp = malloc(n * 4 * cs);
        for (size_t y = 0; y < (size_t)h * d; y++)
            for (size_t x = 0; x < (size_t)w; x++) { uint8_t *o = tmp + (y * w + x) * 4 * cs; memcpy(o, (const uint8_t *)data + y * stride + x * 3 * cs, 3 * (size_t)cs); memcpy(o + 3 * cs, &one[vt->expand - 1], (size_t)cs); }
        data = tmp; size = n * 4 * cs; row_stride = 0;
    }
    uint64_t off = ring_raw(data, size);           /* may submit what was recorded so far */
    free(tmp);
    pass_end(); cmd_begin();
    if (vt->rs) {                                  /* track O: the texture's own memory holds the game's size */
        if (!vt->is3d && (uint32_t)layer < (uint32_t)vt->layers) rs_upload(vt, level, layer, off, row_stride && vt->bpb ? (uint32_t)(row_stride / vt->bpb) : 0, 0, 0, 0, w, h);
        R.upload_bytes += (LONG64)size;
        return;
    }
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    if (row_stride && vt->bpb) c.bufferRowLength = (uint32_t)(row_stride / vt->bpb);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = (uint32_t)(vt->is3d ? d : 1);
    if (!vt->is3d && (uint32_t)layer >= (uint32_t)vt->layers) return;
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    R.upload_bytes += (LONG64)size;
}
/* The image of a texture: made at first use, and holding what the guest memory holds (unless the GPU wrote it). */
/* Session 8: textures made and read AHEAD of their first draw (tex_ahead, below), and what is still made at a draw. */
static LONG64 g_ta_n, g_ta_bytes, g_ta_tk, g_ta_max_tk, g_late_n, g_late_tk, g_late_fight_n, g_late_fight_tk, g_up_fr_tk, g_up_fr_bytes;
static int g_up_fr_n, g_ta_inside;
static volatile LONG g_in_fight;                   /* nvn_vk_m4.c (included further down) */
static VTex *vt_get2(Tex *t, int sync) {
    VTex *vt = t->vk;
    LONG64 tu0 = 0, ub0 = R.upload_bytes;
    if (!vt) {
        tu0 = tk();
        TexBuilder *b = &t->b;
        Fmt f;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        vt = calloc(1, sizeof *vt);
        vt->magic = 0x56544558; vt->t = t;
        vt->fmt = tex_vkfmt(b->format, &vt->expand, &vt->decode);
        if (!vt->fmt || !nvn_front_fmt(b->format, &f)) { static char msg[80]; snprintf(msg, sizeof msg, "texture format 0x%x", b->format); nvn_vk_missing(msg); vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; nvn_front_fmt(0x25, &f); }
        vt->bpb = vt->decode ? 4 : vt->expand ? f.bpb / 3 * 4 : f.bpb; vt->bw = vt->decode ? 1 : f.bw; vt->bh = vt->decode ? 1 : f.bh;
        vt->aspect = fmt_aspect(vt->fmt);
        vt->w = b->width > 0 ? b->width : 1; vt->h = b->height > 0 ? b->height : 1; vt->d = b->depth > 0 ? b->depth : 1;
        vt->levels = nvn_front_tex_levels(t); vt->layers = nvn_front_tex_layers(t); vt->samples = b->samples > 1 ? b->samples : 1; vt->is3d = b->target == 2;
        if (b->target == 0 || b->target == 3) { ci.imageType = VK_IMAGE_TYPE_1D; vt->h = 1; if (b->target == 3) vt->layers = b->height > 0 && b->depth <= 1 ? b->height : vt->layers; }
        else ci.imageType = vt->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        if (RS_ON && (b->target == 1 || b->target == 5) && vt->layers == 1 && !vt->decode && rs_screen_class(vt->w, vt->h) && !(rs_ui_native() && wintex_is(t)) &&
            (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (f.kind != 4 && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)))) {      /* track O: a screen-class target */
            int gw = vt->w, gh = vt->h, lv = 1;
            vt->w = (int)((float)gw * rs_scale() + 0.5f); vt->h = (int)((float)gh * rs_scale() + 0.5f);
            if (vt->w < 1) vt->w = 1; if (vt->h < 1) vt->h = 1;
            vt->rs = 1; vt->sx = (float)vt->w / (float)gw; vt->sy = (float)vt->h / (float)gh;
            while ((vt->w >> lv) || (vt->h >> lv)) lv++;
            if (vt->levels > lv) vt->levels = lv;
            g_rs_images++;
        }
        ci.format = vt->fmt; ci.extent.width = (uint32_t)vt->w; ci.extent.height = (uint32_t)vt->h; ci.extent.depth = (uint32_t)(vt->is3d ? vt->d : 1);
        ci.mipLevels = (uint32_t)vt->levels; ci.arrayLayers = (uint32_t)(vt->is3d ? 1 : vt->layers); ci.samples = (VkSampleCountFlagBits)vt->samples; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        int can_color = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && f.kind != 4 && !vt->decode && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        if (can_color) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if ((b->flags & 4) && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand && vt->samples == 1 && !vt->rs && vt->bw == 1 && vt->bh == 1 &&
            (fmt_supported(vt->fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB)) {      /* SMO: NVN_TEXTURE_FLAGS_IMAGE (0x4; the game sets it on most textures): compute programs may bind it as an image; not on block-compressed formats, which no storage view can have */
            ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT; vt->storage = 1; g_storage_tex++;
            if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ci.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;      /* bound through a view of another format (sRGB, r32ui on r32f) */
        }
        if (ci.imageType == VK_IMAGE_TYPE_2D && vt->w == vt->h && vt->layers >= 6 && vt->samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (vt->is3d && (ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;      /* SMO: its slices can be render targets */
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(V.device, &ci, NULL, &vt->img);
        if (r != VK_SUCCESS) vkd_fail("vkCreateImage (NVN format 0x%x -> %d, target %d, %dx%dx%d, %d levels, %d layers, %d samples): %s", b->format, (int)vt->fmt, b->target, vt->w, vt->h, vt->d, vt->levels, vt->layers, vt->samples, vk_result_name(r));
        VkMemoryRequirements rq;
        vkGetImageMemoryRequirements(V.device, vt->img, &rq);
        vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
        vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
        t->vk = vt;
        R.images++; R.images_live++; nvn_front_count(5, 1);
        g_fmt_hist[b->format & 255]++;
        if (can_color || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {      /* an image nothing was written to reads as zero (the OpenGL driver's behaviour, which the comparison saw) */
            VkImageSubresourceRange sr = {vt->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (vt->samples == 1) {
                if (can_color) { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
                else { VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
            }
        }
    }
    vt_check(vt);
    if (sync && nvn_front_tex_changed(t)) {
        if (!tu0) tu0 = tk();
        g_put_vt = vt;
        cmd_begin();
        if (!nvn_front_tex_read(t, vt->decode, vt_put)) { static int n; if (n++ < 20) printf("nvn_vk: texture %dx%d format 0x%x: its memory is not readable; not uploaded\n", t->b.width, t->b.height, t->b.format); }
        R.uploads++;
    }
    if (tu0 && !g_ta_inside) {                     /* made or read again at its use (a draw, a copy, a target): the frame pays */
        LONG64 d = tk() - tu0;
        g_up_fr_tk += d; g_up_fr_n++; g_up_fr_bytes += R.upload_bytes - ub0;
        g_late_n++; g_late_tk += d;
        if (g_in_fight) { g_late_fight_n++; g_late_fight_tk += d; }
    }
    return vt;
}
static VTex *vt_get(Tex *t) { return vt_get2(t, 1); }
/* Session 8, render thread, after every present. A texture used to get its image, and its texels read out of the game's
 * memory (hash, de-swizzle, copy into the stream ring), at the first draw that sampled it: the first frame of a fight paid
 * 37-50 ms for the stage's and the fighters' textures (runs s7fn2 / s7gn2, frames 1804-1823). Here every block-compressed
 * texture the game has made (nvnTextureInitialize) and not yet drawn with is given its image two frames after the render
 * thread first saw it, under a time budget per frame (4 ms outside a fight, 1 ms in one; one texture is always taken, so a
 * very large one can exceed it): the loading screen absorbs the work. Only compressed formats: they cannot be render
 * targets, so nothing the GPU is about to write is touched; if the game fills the memory later, the draw sees the change
 * (nvn_front_tex_changed) and reads it again, as before. SMO_VK_TEXAHEAD=0 = as before (everything at the first draw). */
static int fight_recent(void);                     /* nvn_vk_m4.c */
static void present_progress(float frac);          /* session 11: the held loading screen presented again with the progress bar (defined above vk_present) */
static LONG64 g_ovl_presents, g_ovl_frames;        /* extra presents of a held loading screen; normal frames drawn with the thin bar */
static int holdlog_on(void);
static void tex_ahead(LONG64 frame) {
    static int on = -1;
    static Tex *done_head;
    static LONG64 fq;
    if (on < 0) { const char *e = getenv("SMO_VK_TEXAHEAD"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; on = !(e && !atoi(e)); }
    if (!on) return;
    Tex *head = nvn_front_tex_first();
    if (head == done_head) return;
    int fr = fight_recent();
    LONG64 t0 = tk(), budget = fq * (fr ? 1 : 4) / 1000;
    int pending = 0, taken = 0;
    Tex *oldest = NULL;                            /* the oldest texture still pending: the next walk ends after it */
    g_ta_inside = 1;
    for (Tex *t = head; t && t != done_head; t = t->all_next) {
        Fmt f;
        if (t->ahead_seen < 0) continue;
        if (t->vk || t->dead) { t->ahead_seen = -1; continue; }
        if (!t->ahead_seen) { t->ahead_seen = frame + 1; pending++; oldest = t; continue; }
        if (frame + 1 - t->ahead_seen < 2) { pending++; oldest = t; continue; }
        if (t->gpu_written || (!t->mem && !t->b.packaged) || t->b.samples > 1 || t->b.width <= 0 || t->b.height <= 0 || !nvn_front_fmt(t->b.format, &f) || f.kind != 4) { t->ahead_seen = -1; continue; }
        if (taken && tk() - t0 > budget) { pending++; oldest = t; continue; }
        if (fr) {
            /* in and around a fight (a frame of the last 600 had fighters) only what is certainly cheap: at most 512 KB, no
             * decoding on the CPU, no wrap of the stream ring (a wrap waits for the GPU). Measured without this rule (runs s8b
             * frame 3549, s8d 4297, s8f 3840: the result screen's textures arriving while the fight ends, in frames without
             * a dispatch): 15-22 ms after one present and a frame of 34-35 ms. The others wait for a frame far from a fight,
             * or are made at their use as before. SMO port: SMO dispatches compute in every frame, so for it every frame is
             * "in a fight": a texture over 512 KB or decoded on the CPU is made at its use (it stayed pending for ever, and the
             * walk above, which ends after the oldest pending texture, went over every texture made since the start in every
             * frame: 24% of the render thread after two minutes in Cascade Kingdom). */
            VTex probe;
            if (t->data_size > (512u << 10)) { t->ahead_seen = -1; continue; }
            if (R.ring_pos + 2 * (uint64_t)t->data_size + (1u << 20) > R.ring_size) { pending++; oldest = t; continue; }
            tex_vkfmt(t->b.format, &probe.expand, &probe.decode);
            if (probe.decode) { t->ahead_seen = -1; continue; }
        }
        LONG64 b0 = R.upload_bytes;
        vt_get(t);
        t->ahead_seen = -1; taken++;
        g_ta_n++; g_ta_bytes += R.upload_bytes - b0;
    }
    g_ta_inside = 0;
    LONG64 d = tk() - t0;
    g_ta_tk += d; if (d > g_ta_max_tk) g_ta_max_tk = d;
    if (d * 1000 > 6 * fq && holdlog_on()) { static int nl; if (nl++ < 100) printf("nvn_vk_tex: frame %lld (%s): %d textures made ahead after the present in %.1f ms\n", (long long)frame, fr ? "in or near a fight" : "far from a fight", taken, (double)d * 1000.0 / (double)fq); }
    done_head = pending ? oldest->all_next : head;     /* everything older than the oldest pending texture is done (before: only when nothing was
                                                    * pending, and the game makes textures every frame, so every frame walked all of them) */
}
/* tex_view_obj's rules: the effective view of `t` through `v`. */
static void eview_of(Tex *t, VTex *vt, const View *v, EView *e) {
    EView base;
    memset(&base, 0, sizeof base);
    base.target = t->b.target; base.format = t->b.format; base.nlvl = vt->levels; base.nlay = vt->is3d ? vt->d : nvn_front_tex_layers(t); base.dsmode = t->b.dsmode;   /* SMO: a 3D texture's view layers are its depth slices (a render target view picks one) */
    memcpy(base.swz, t->b.swz, 4);
    *e = base;
    if (!v || !(v->set & 0x80)) return;
    if (v->set & 1) { e->lvl0 = v->lvl0; e->nlvl = v->nlvl; }
    if (v->set & 2) { e->lay0 = v->lay0; e->nlay = v->nlay; }
    if ((v->set & 4) && v->format) e->format = v->format;
    if (v->set & 8) memcpy(e->swz, v->swz, 4);
    if (v->set & 16) e->dsmode = v->dsmode;
    if (v->set & 32) e->target = v->target;
    if (e->lvl0 < 0 || e->lvl0 >= base.nlvl) e->lvl0 = 0;
    if (e->nlvl <= 0 || e->lvl0 + e->nlvl > base.nlvl) e->nlvl = base.nlvl - e->lvl0;
    if (e->lay0 < 0 || e->lay0 >= base.nlay) e->lay0 = 0;
    if (e->nlay <= 0 || e->lay0 + e->nlay > base.nlay) e->nlay = base.nlay - e->lay0;
    if (t->b.target == 2) e->target = base.target;      /* SMO: a 3D texture keeps the view's slice (lay0); before, every slice became slice 0 (the colour-grading LUT is drawn as 8 attachments = 8 slices: blue frames) */
    else if (t->b.samples > 1) { e->target = base.target; e->lay0 = 0; e->nlay = base.nlay; }
    if ((e->target == 1 || e->target == 7 || e->target == 0) && e->nlay > 1) e->nlay = 1;
    if (e->target == 8 && e->nlay != 6) e->target = e->nlay == 1 ? 1 : 4;
    if (e->target == 9 && e->nlay % 6) e->target = 4;
}
enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1DA, DIM_2DA, DIM_CUBEA };
static const VkImageViewType k_vtype[7] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
static int target_dim(int target) { static const int m[10] = {DIM_1D, DIM_2D, DIM_3D, DIM_1DA, DIM_2DA, DIM_2D, DIM_2DA, DIM_2D, DIM_CUBE, DIM_CUBEA}; return target >= 0 && target < 10 ? m[target] : DIM_2D; }
/* dim < 0: an attachment view (one level, one layer, both aspects, no swizzle). */
static VkImageView vt_view(VTex *vt, const EView *e, int dim, int attach) {
    for (VView *n = vt->views; n; n = n->next) if (n->vtype == dim && n->attach == attach && !memcmp(&n->e, e, sizeof *e)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    int ex, dec;
    n->e = *e; n->vtype = dim; n->attach = attach;
    ci.image = vt->img; ci.format = vt->fmt;
    if (e->format != vt->t->b.format && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand) { VkFormat f = tex_vkfmt(e->format, &ex, &dec); if (f && !ex && !dec && fmt_aspect(f) == VK_IMAGE_ASPECT_COLOR_BIT) ci.format = f; }
    if (attach) {
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.subresourceRange.aspectMask = vt->aspect;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1; ci.subresourceRange.baseArrayLayer = (uint32_t)e->lay0; ci.subresourceRange.layerCount = 1;
        if (vt->is3d) {   /* SMO: one depth slice of a 3D texture, through a 2D view (image made 2D_ARRAY_COMPATIBLE); the slice is the view's first layer */
            int dl = vt->d >> e->lvl0; if (dl < 1) dl = 1;
            if (e->lay0 >= dl) ci.subresourceRange.baseArrayLayer = (uint32_t)(dl - 1);
            if (e->nlay > 1) { static LONG64 n; if (n++ < 5) printf("nvn_vk: a 3D texture bound as render target with %d slices: only slice %d is drawn (layered rendering into a 3D texture is not implemented)\n", e->nlay, e->lay0); }
        }
    } else {
        int lay = e->nlay;
        if (dim == DIM_1D || dim == DIM_2D || dim == DIM_3D) lay = 1; else if (dim == DIM_CUBE) lay = 6; else if (dim == DIM_CUBEA) lay -= lay % 6;
        ci.viewType = k_vtype[dim];
        ci.subresourceRange.aspectMask = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (e->dsmode == 1 || vt->aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = (uint32_t)e->nlvl; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = (uint32_t)(vt->is3d ? 1 : lay);
        ci.components.r = sw[e->swz[0] < 6 ? e->swz[0] : 0]; ci.components.g = sw[e->swz[1] < 6 ? e->swz[1] : 0]; ci.components.b = sw[e->swz[2] < 6 ? e->swz[2] : 0]; ci.components.a = sw[e->swz[3] < 6 ? e->swz[3] : 0];
    }
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (type %d, format %d of image format %d, levels %d+%d, layers %d+%d of %d, attach %d): %s", (int)ci.viewType, (int)ci.format, (int)vt->fmt, e->lvl0, e->nlvl, e->lay0, e->nlay, vt->layers, attach, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++;
    return n->view;
}

/* ---------------------------------------------------------------- samplers (shared by value, never deleted before shutdown) */
static struct { SamplerParams p; VkSampler s; } g_smp[2048];
static int g_nsmp;
static VkSampler g_smp_default;
static VkSampler sampler_get(const SamplerParams *p) {
    static const VkSamplerAddressMode wrap[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int i = 0; i < g_nsmp; i++) if (!memcmp(&g_smp[i].p, p, sizeof *p)) return g_smp[i].s;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    int minf = p->min < 6 ? p->min : 1;          /* nearest, linear, nearest-mip-nearest, linear-mip-nearest, nearest-mip-linear, linear-mip-linear */
    ci.minFilter = (minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.magFilter = p->mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap[p->ws & 7]; ci.addressModeV = wrap[p->wt & 7]; ci.addressModeW = wrap[p->wr & 7];
    ci.mipLodBias = p->bias; ci.minLod = p->lodmin < 0 ? 0 : p->lodmin; ci.maxLod = p->lodmax;
    if (minf < 2) { ci.minLod = 0; ci.maxLod = 0.25f; }      /* no mip maps: level 0 only (the Vulkan specification's own mapping of GL_NEAREST / GL_LINEAR) */
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    float aniso = p->aniso;
    {   /* track O: anisotropic_filtering in graphics.ini = at least that on samplers that filter linearly between mip levels (not shadow-compare samplers) */
        static LONG64 raised;
        float want = (float)gfx_options()->aniso;
        if (want > aniso && minf >= 4 && p->cmode != 1) { aniso = want; raised++; if (raised == 1 || raised % 100 == 0) printf("nvn_vk: anisotropic_filtering=%d: %lld samplers raised so far (this one from %.0f)\n", gfx_options()->aniso, (long long)raised, p->aniso); }
    }
    if (aniso > 1.0f && D.f2.features.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = aniso > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : aniso; }
    if (p->cmode == 1) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cfunc ? (p->cfunc - 1) & 7 : 7); }
    {   /* the OpenGL path can only use four border colours (bindless handles: sampler_gl); the same rounding here */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]}, mx = b[0] > b[1] ? b[0] : b[1];
        int rgb, al;
        if (b[2] > mx) mx = b[2];
        rgb = mx >= 0.5f; al = b[3] >= 0.5f;
        if (p->border_kind) { int32_t bi[4]; memcpy(bi, p->border, 16); rgb = bi[0] || bi[1] || bi[2]; al = bi[3] != 0; }
        if (!rgb) ci.borderColor = p->border_kind ? (al ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK) : (al ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        else if (al) ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else {
            int uses = (p->ws & 7) == 5 || (p->wt & 7) == 5 || (p->wr & 7) == 5;
            if (uses && !D.has_cbc) nvn_vk_missing("a sampler border colour (1, 1, 1, 0) on a GPU without VK_EXT_custom_border_color");
            if (D.has_cbc) {
                ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                if (p->border_kind) { cb.customBorderColor.int32[0] = cb.customBorderColor.int32[1] = cb.customBorderColor.int32[2] = 1; }
                else { cb.customBorderColor.float32[0] = cb.customBorderColor.float32[1] = cb.customBorderColor.float32[2] = 1.0f; }
                cb.pNext = ci.pNext; ci.pNext = &cb;
            }
        }
    }
    if (p->reduction == 1 || p->reduction == 2) {
        if (!D.f12.samplerFilterMinmax) nvn_vk_missing("a min / max reduction sampler on a GPU without samplerFilterMinmax");
        else { red.reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX; red.pNext = ci.pNext; ci.pNext = &red; }
    }
    if (p->lod_snap != 0.0f) nvn_vk_missing("a sampler with LOD snap");
    VkSampler s;
    VKC(vkCreateSampler(V.device, &ci, NULL, &s));
    if (g_nsmp == 2048) vkd_fail("more than 2048 distinct samplers");
    g_smp[g_nsmp].p = *p; g_smp[g_nsmp++].s = s;
    return s;
}

#include "nvn_vk_m4.c"                             /* session 4: stages, pipelines, workers (M4) */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's stages and pipelines made at the draw); kept only until someone deletes the block */
/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h; int stage, code_len, ok; uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp;
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next;
} VStage;
/* M4 (design section 5): what a stage needs to become a module again without the translator and glslang, in
 * ..\cache\vk\stage\<stage>_<hash of machine code, constants, variant, translator version>.st */
#define MX_VK_VERSION 8u                           /* part of the stage hash: change it when mx_translate's Vulkan text changes */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp; VSmp smp[64]; } StageRec;
static CRITICAL_SECTION g_sh_lock;                 /* the stage and pipeline tables while the start-up workers run */
static VkPipelineCache g_pcache;
static LONG64 g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
static VStage *g_vstage[4096];
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static uint64_t g_spv_unused;
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
    }
    free(words); fclose(fp);
    return mod;
}
static void stage_rec_path(char *path, size_t n, int s, uint64_t h) { snprintf(path, n, "../cache/vk/stage/%d_%016llx.st", s, (unsigned long long)h); }
/* A stage from its record on disk (the caller holds g_sh_lock when workers run). NULL when there is none. */
static VStage *vstage_from_rec(int s, uint64_t h) {
    char path[128];
    StageRec r;
    stage_rec_path(path, sizeof path, s, h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t got = fread(&r, 1, sizeof r, fp);
    fclose(fp);
    if (got != sizeof r || r.magic != 0x52545356 + MX_VK_VERSION || r.stage != s || r.nsmp < 0 || r.nsmp > 64) return NULL;
    VkShaderModule mod = module_from_spv(r.spv);
    if (!mod) return NULL;
    VStage *v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = r.code_len; memcpy(v->key, r.key, 16); v->fo = r.fo; v->id = ++g_vstage_ids;
    v->cbufs = r.cbufs; v->ssbos = r.ssbos; v->ssbo_w = r.ssbo_w; v->images = r.images; v->dep = r.dep; v->tex_units = r.tex_units; v->in_attr = r.in_attr; v->out_attr = r.out_attr;
    v->uses_alpha = r.uses_alpha; v->nsmp = r.nsmp; memcpy(v->smp, r.smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    InterlockedIncrement64(&R.modules);
    return v;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash) {
    {   /* SMO port, debug aid SMO_VK_GLSLDUMP=<folder>: every freshly translated stage as <what>.glsl (cached stages are
         * not translated again: move build/cache/vk/stage aside to dump everything) */
        const char *dir = getenv("SMO_VK_GLSLDUMP");
        if (dir) {
            static LONG n;
            char path[300]; snprintf(path, sizeof path, "%s/%s.glsl", dir, what); for (char *q = path + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '#') *q = '_';
            FILE *f = fopen(path, "wb");
            if (f) { fputs(glsl, f); fclose(f); }
            if (InterlockedIncrement(&n) <= 2) printf("nvn_vk: GLSL dump %s: %s\n", path, f ? "written" : "NOT written");
        }
    }
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    int from_dll = 0;
    VkShaderModule mod = NULL;
    LONG64 t_mod = tk();
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n > 20 && n % 4 == 0) { words = malloc((size_t)n); count = (size_t)n / 4; if (fread(words, 1, (size_t)n, fp) != (size_t)n || words[0] != 0x07230203) { free(words); words = NULL; } }
        fclose(fp);
    }
    if (!words) {
        if (!g_glsl_to_spirv) {
            HMODULE m = LoadLibraryA("smo_glslang.dll");
            g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "smo_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "smo_spirv_free") : NULL;
            if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("smo_glslang.dll cannot be loaded (build_glslang.bat makes it)");
        }
        if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
        from_dll = 1;
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/spv", NULL);
        fp = fopen(path, "wb");
        if (fp) { fwrite(words, 4, count, fp); fclose(fp); }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    if (from_dll) g_spirv_free(words); else free(words);
    InterlockedIncrement64(&R.modules);
    g_tm[6] += tk() - t_mod;
    return mod;
}
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo) {
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    if (!st->code) return NULL;
    uint32_t dep_a = v && s == 0 ? v->dep : 0;
    if (v && ((key[0] ^ v->key[0]) & dep_a) == 0 && ((key[1] ^ v->key[1]) & dep_a) == 0 && ((key[2] ^ v->key[2]) & v->tex_units) == 0 && ((key[3] ^ v->key[3]) & v->tex_units) == 0 && v->fo == fo) return v->ok ? v : NULL;
    const uint8_t *cdata = st->ctl[3] && st->ctl[4] ? st->code - (s == 5 ? 0 : 0x30) + st->ctl[4] : NULL;       /* a compute stage has no program header */
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
    if (s == 5) { h = fnv(h, st->local, sizeof st->local); h = fnv(h, &st->smem_bytes, 4); h = fnv(h, &st->lmem_bytes, 4); }
    if (cdata) h = fnv(h ^ st->ctl[3], cdata, st->ctl[3]);
    h = fnv(h, key, 16); h = fnv(h, &fo, 4);
    { uint32_t ver = MX_VK_VERSION; h = fnv(h, &ver, 4); }
    for (v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s && v->code_len == st->code_len && v->fo == fo && !memcmp(v->key, key, 16)) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v->ok ? v : NULL; }
    {   LONG64 t_mod = tk();
        v = vstage_from_rec(s, h);                 /* met in an earlier run: no translator, no glslang */
        g_tm[6] += tk() - t_mod;
        if (v) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v; }
    }
    MxIn in;
    MxOut out;
    char what[64];
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = s; in.code = st->code; in.code_len = st->code_len; in.cdata = cdata; in.cdata_size = cdata ? st->ctl[3] : 0;
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3]; in.force_out = fo;
    in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
    if (s == 5) {                                  /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
        in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
        memcpy(in.local_size, st->local, sizeof in.local_size); in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    }
    v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = st->code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    st->vk = v;
    snprintf(what, sizeof what, "program #%d %s stage", p->serial, s == 5 ? "compute" : s ? "fragment" : "vertex");
    if (!mx_translate(&in, &out)) { printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout); return NULL; }
    uint64_t spv_hash = 0;
    v->mod = module_from_glsl(s, out.glsl, what, &spv_hash);
    memcpy(v->local, st->local, sizeof v->local);
    v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr; v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->nsmp = out.nsmp;
    if (s == 0) v->dep = out.int_dep; else v->tex_units = out.tex_units | out.int_dep;
    v->tex_units = out.tex_units;
    for (int i = 0; i < out.nsmp && i < 64; i++) {
        const char *ty = out.smp[i].type;
        int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
        if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
        v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
    }
    free(out.glsl);
    v->ok = v->mod != NULL;
    if (v->ok) {
        char path[128];
        StageRec r;
        memset(&r, 0, sizeof r);
        r.magic = 0x52545356 + MX_VK_VERSION; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash;
        r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
        r.uses_alpha = v->uses_alpha; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/stage", NULL);
        stage_rec_path(path, sizeof path, s, v->h);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
    }
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines (made at the draw that first needs them: session 2; workers are M4) */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; } PKey;
typedef struct Pipe { PKey k; VkPipeline p; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; struct Pipe *next; } Pipe;
static Pipe *g_pipes[1024];
static void pkey_targets(PKey *k);
static VkBlendFactor blend_factor(unsigned f) {
    static const uint8_t m[9] = {VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                 VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
                 case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                 case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned e) { static const uint8_t m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX}; return (VkBlendOp)m[e < 6 ? e : 1]; }
static void layout_bindings(VStage *v, int s, VkDescriptorSetLayoutBinding *b0, int *n0, VkDescriptorSetLayoutBinding *b1, int *n1) {
    VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    for (unsigned n = 1; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[(*n1)++] = b; }
}
/* Makes the pipeline of a key (any thread: the device calls are thread-safe, the pipeline cache is synchronised by the driver). */
static Pipe *pipe_make(const PKey *k, VStage *vs, VStage *fs, double *took) {
    Pipe *p = calloc(1, sizeof *p);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    p->k = *k; p->vs = vs; p->fs = fs;
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &p->nb[0], b1, &p->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &p->nb[0], b1, &p->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)p->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && p->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; p->push0 = 1; }      /* 32: the smallest maxPushDescriptors a device may have */
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &p->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = p->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &p->layout));

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? fs->mod : NULL; st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1; vp.pNext = vk_vp_next();
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof *dyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    *took = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    InterlockedIncrement64(&R.pipelines);
    return p;
}
/* ..\cache\vk\pipes.bin: every pipeline key this PC ever needed, with its stages named by their hashes (design section 5:
 * "pipes.txt"). Read at start by the workers; a key missing at a draw is made there and appended (once per key per PC). */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    CreateDirectoryA("../cache/vk", NULL);
    FILE *fp = fopen("../cache/vk/pipes.bin", "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, k, sizeof *k);
    for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p;
    double ms = 0;
    LONG64 t0 = tk();
    Pipe *p = pipe_make(k, vs, fs, &ms);           /* never met on this PC: made here, the frame is late once (counted) */
    g_tm[5] += tk() - t0;
    g_pipe_miss++;
    if (D.debug) printf("nvn_vk: pipeline miss %lld: stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    piperec_append(k, vs, fs, 0);
    return p;
}

#endif
/* ---------------------------------------------------------------- passes */
static int target_level(const TargetRef *r) { return (r->view.set & 0x81) == 0x81 ? r->view.lvl0 : 0; }
static VkImageView target_view(const TargetRef *r, VTex *vt) {
    EView e;
    eview_of(r->tex, vt, &r->view, &e);
    memset(e.swz, 0, 4); e.nlvl = 1; e.nlay = 1; e.format = r->tex->b.format; e.target = 1; e.dsmode = 0;
    if ((r->view.set & 0x84) == 0x84 && r->view.format) e.format = r->view.format;
    return vt_view(vt, &e, -1, 1);
}
static int pass_begin(void) {
    VkRenderingAttachmentInfo ca[8], da;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    int w = 0, h = 0;
    if (R.in_pass) return 1;
    cmd_begin();
    R.pass_rs = 0;
    memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da);
    for (int i = 0; i < R.ncol; i++) {
        Tex *t = R.col[i].tex;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!t || t->dead) continue;
        VTex *vt = vt_get(t);
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("a depth format bound as colour target"); continue; }
        if (vt->is3d && ((t->b.format == 0x3f && vt->d <= 8) || getenv("SMO_VK_3DRT_ALL"))) { static int n; if (n++ < 40) printf("nvn_vk_3drt: pass %lld: 3D texture %dx%dx%d fmt 0x%x as colour %d, view set 0x%x layers %d+%d level %d\n", (long long)nvn_front_frame_next(), vt->w, vt->h, vt->d, t->b.format, i, R.col[i].view.set, R.col[i].view.lay0, R.col[i].view.nlay, target_level(&R.col[i])); }
        t->gpu_written = 1;
        vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca[i].imageView = target_view(&R.col[i], vt); ca[i].imageLayout = vt->layout;
        if (!w) { w = lvl_(vt->w, target_level(&R.col[i])); h = lvl_(vt->h, target_level(&R.col[i])); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
    }
    if (R.depth.tex && !R.depth.tex->dead) {
        VTex *vt = vt_get(R.depth.tex);
        R.depth.tex->gpu_written = 1;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) nvn_vk_missing("a colour format bound as depth target");
        else if (RS_ON && rs_ui_native() && vt->rs && !R.pass_rs && w && (lvl_(vt->w, target_level(&R.depth)) < w || lvl_(vt->h, target_level(&R.depth)) < h)) g_rs_mixed++;      /* session 9 (SMO_VK_UI_NATIVE): an unscaled colour target with a scaled depth target: the pass runs WITHOUT depth (counted, reported at exit) */
        else if (w && (lvl_(vt->w, target_level(&R.depth)) < w || lvl_(vt->h, target_level(&R.depth)) < h)) {
            /* SMO port: a depth target SMALLER than the colour targets. NVN allows targets of different sizes (each surface
             * drops what falls outside it); Vulkan needs every attachment to cover the render area, and NVIDIA then drew
             * nothing at all. SMO binds a 2x2 dummy depth texture to full-screen passes with depth test and write off
             * (title screen, boot105 pass 7: Mario's layer stayed empty and the projected shadow became black speckles).
             * The pass runs without depth; the dropped surface could only have held its own few texels. */
            static LONG64 n; if (n++ < 5) printf("nvn_vk: depth target %dx%d smaller than the colour targets %dx%d: pass without depth (%lld so far)\n", lvl_(vt->w, target_level(&R.depth)), lvl_(vt->h, target_level(&R.depth)), w, h, (long long)n);
        }
        else {
            vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.imageView = target_view(&R.depth, vt); da.imageLayout = vt->layout;
            if (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
            if (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
            if (!w) { w = lvl_(vt->w, target_level(&R.depth)); h = lvl_(vt->h, target_level(&R.depth)); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
        }
    }
    if (!w) return 0;
    {   /* session 9, SMO_VK_UILOG=<n>: every pass begun in the n-th battle frame ("nvn_vk_ui:" lines): its targets, whether the
         * colour target is a texture of the window, and the draws of the frame so far (the next line's number minus this one = its draws) */
        static int uil = -2, np;
        if (uil == -2) uil = getenv("SMO_VK_UILOG") ? atoi(getenv("SMO_VK_UILOG")) : -1;
        if (uil > 0 && g_battle_frames == uil) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL, *dt = R.depth.tex && !R.depth.tex->dead ? R.depth.tex : NULL;
            printf("nvn_vk_ui: pass %d: %d colour, first %dx%d format 0x%x level %d%s%s | depth %dx%d format 0x%x%s | image size of the pass %dx%d | %lld draws so far, %lld dispatches so far\n", np++, R.ncol,
                   c0 ? c0->b.width : 0, c0 ? c0->b.height : 0, c0 ? (int)c0->b.format : 0, c0 ? target_level(&R.col[0]) : 0, c0 && wintex_is(c0) ? " WINDOW TEXTURE" : "", c0 && c0->vk && ((VTex *)c0->vk)->rs ? " (scaled)" : "",
                   dt ? dt->b.width : 0, dt ? dt->b.height : 0, dt ? (int)dt->b.format : 0, dt && dt->vk && ((VTex *)dt->vk)->rs ? " (scaled)" : "", w, h, (long long)R.frame_draws, (long long)R.dispatches);
        }
    }
    {   /* SMO_VK_RTLOG=1 (track O, resolution scale survey): every distinct render-target size a pass is begun with, with
         * the colour / depth formats and how many passes used it (printed at exit: "nvn_vk_rt:" lines) */
        static int on = -1;
        if (on < 0) on = getenv("SMO_VK_RTLOG") && atoi(getenv("SMO_VK_RTLOG"));
        if (on) {
            int cf = R.ncol && R.col[0].tex ? (int)R.col[0].tex->b.format : 0, df = R.depth.tex ? (int)R.depth.tex->b.format : 0, lv = R.ncol && R.col[0].tex ? target_level(&R.col[0]) : R.depth.tex ? target_level(&R.depth) : 0, i;
            for (i = 0; i < g_rtlog_n; i++) if (g_rtlog[i].w == w && g_rtlog[i].h == h && g_rtlog[i].ncol == R.ncol && g_rtlog[i].cf == cf && g_rtlog[i].df == df && g_rtlog[i].lv == lv) break;
            if (i == g_rtlog_n && g_rtlog_n < 256) { g_rtlog[i].w = w; g_rtlog[i].h = h; g_rtlog[i].ncol = R.ncol; g_rtlog[i].cf = cf; g_rtlog[i].df = df; g_rtlog[i].lv = lv; g_rtlog[i].first = nvn_front_frame_next(); g_rtlog_n++; }
            if (i < 256) { g_rtlog[i].n++; if (g_in_fight) g_rtlog[i].fight++; }
        }
    }
    cmd_begin();                                   /* vt_get may have submitted (ring wrap) */
    ri.renderArea.extent.width = (uint32_t)w; ri.renderArea.extent.height = (uint32_t)h; ri.layerCount = 1; ri.colorAttachmentCount = (uint32_t)R.ncol; ri.pColorAttachments = ca;
    vkCmdBeginRendering(R.cmd, &ri);
    g_fr_passes++;
    R.in_pass = 1; R.pass_w = w; R.pass_h = h;
    return 1;
}

/* ---------------------------------------------------------------- draws */
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    Slot *s = &R.slot[R.cur];
    VkDescriptorSet set;
    for (;;) {
        if (s->ipool < s->npool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s->pool[s->ipool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            VkResult r = vkAllocateDescriptorSets(V.device, &ai, &set);
            if (r == VK_SUCCESS) { R.sets++; return set; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) vk_fatal("vkAllocateDescriptorSets", r);
            s->ipool++;
            continue;
        }
        if (s->npool == 16) vkd_fail("more than 16 descriptor pools in one command buffer");
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 8192; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &s->pool[s->npool++]));
    }
}
static struct { uint64_t key; VkDescriptorSet set; uint32_t gen; VkDescriptorSetLayout dl; int n, off; } g_tset[4096];      /* texture sets of the command buffer being recorded */
static struct { VTex *d2, *d1, *d3; VkImageView v[7]; } g_dummy;
static struct { VkImageView view; VkSampler smp; VkImageLayout layout; } g_dtex[200], g_tset_list[32768];      /* g_tset_list: the lists of the cached sets, for the full comparison */
static int g_ndtex;
/* The image view and sampler of one declared sampler of stage s. May end the pass (upload, layout change). */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride);
/* SMO, 1.4.1: a sampler whose handle the game keeps in one of its uniform buffers (mx_translate.c bindless_unit: unit
 * 32 + 8 * hardware buffer + 64-bit word; hardware buffer n >= 3 is the stage's uniform buffer slot n - 3), read at the draw.
 * 0 when the buffer is not bound or too short (the shader of 1.4.1's primitive drawer then draws untextured itself). */
static uint64_t uniform_handle(int s, unsigned u) {
    unsigned cb = (u - 32) / 8, off = (u - 32) % 8 * 8;
    if (u < 32 || cb < 3 || cb >= 19) return 0;
    const Range *b = &R.ubo[s][cb - 3];
    if (!b->addr || b->size < off + 8) return 0;
    uint64_t h;
    memcpy(&h, (const void *)(uintptr_t)(b->addr + off), 8);
    return h;
}
static void resolve_sampler(int s, VStage *v, int i) {
    unsigned u = v->smp[i].unit;
    Tex *t = NULL;
    View view;
    SamplerParams sp;
    uint64_t handle = u < 32 ? R.nvn_tex[s][u] : uniform_handle(s, u);
    int r = u < 32 || handle ? nvn_front_handle(R.tpool, R.spool, handle, &t, &view, &sp) : 0;
    if (u >= 32) {      /* the first draw of each program that takes a texture this way: what it got (says where the game uses it) */
        static int said[64], nsaid;
        int serial = R.prog[s] ? R.prog[s]->serial : -1, k;
        for (k = 0; k < nsaid && said[k] != serial; k++) {}
        if (k == nsaid && nsaid < 64) {
            said[nsaid++] = serial;
            printf("nvn_vk: program #%d, %s stage: texture handle %llx from uniform buffer %u + %u (first draw, frame %lld) -> %s",
                   serial, s ? "fragment" : "vertex", (unsigned long long)handle, (u - 32) / 8 - 3, (u - 32) % 8 * 8,
                   (long long)nvn_front_frame_next(), (r & 1) ? "" : "no texture (an untextured draw)\n");
            if (r & 1) printf("texture format 0x%x, %d x %d\n", t->b.format, t->b.width, t->b.height);
        }
    }
    {   /* SMO port, with SMO_VK_PASSSHOT: what each draw of that frame samples (pass_shot's frame) */
        static int want = -2;
        want = passshot_frame() ? 1 : -1;
        if (want >= 0) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL;
            printf("nvn_vk_bind: stage %d unit %u handle %llx -> %s tex %p fmt 0x%x %dx%d mem %p gpu_written %d | target tex %p fmt 0x%x mem %p\n",
                   s, u, (unsigned long long)handle, (r & 1) ? "live" : "NO TEXTURE", (void *)((r & 1) ? t : NULL),
                   (r & 1) ? (unsigned)t->b.format : 0u, (r & 1) ? t->b.width : 0, (r & 1) ? t->b.height : 0, (void *)((r & 1) ? t->mem : NULL),
                   (r & 1) ? t->gpu_written : -1, (void *)c0, c0 ? (unsigned)c0->b.format : 0u, (void *)(c0 ? c0->mem : NULL));
            if (!(r & 1) && handle) printf("nvn_vk_bind:   why: %s\n", nvn_front_handle_why(R.tpool, handle));
            if ((r & 1) && t->b.target == 2 && t->b.format == 0x3f && t->b.width * t->b.height * t->b.depth <= 4096) {      /* SMO debug aid: a small R11G11B10F volume (colour-grading LUT): its contents */
                static uint32_t px[4096];
                int rr[6] = {0, 0, 0, t->b.width, t->b.height, t->b.depth}, n = t->b.width * t->b.height * t->b.depth;
                if (read_region(t, NULL, rr, px, 0, 0)) {
                    double m[3] = {0, 0, 0};
                    float first[4][3];
                    for (int k = 0; k < n; k++) {
                        uint32_t w = px[k], bits[3] = {w & 0x7ff, (w >> 11) & 0x7ff, w >> 22}, mb[3] = {6, 6, 5};
                        for (int ch = 0; ch < 3; ch++) {        /* unsigned float: 5-bit exponent, 6/6/5-bit mantissa */
                            uint32_t ex = bits[ch] >> mb[ch], man = bits[ch] & ((1u << mb[ch]) - 1);
                            float f = ex == 0 ? ldexpf((float)man, -14 - (int)mb[ch]) : ex == 31 ? 65504.0f : ldexpf(1.0f + (float)man / (float)(1u << mb[ch]), (int)ex - 15);
                            m[ch] += f; if (k < 4) first[k][ch] = f;
                        }
                    }
                    printf("nvn_vk_lut: %dx%dx%d R11G11B10F mean %.3f %.3f %.3f | texel 0: %.3f %.3f %.3f  1: %.3f %.3f %.3f  2: %.3f %.3f %.3f  3: %.3f %.3f %.3f | last raw %08x\n",
                           t->b.width, t->b.height, t->b.depth, m[0] / n, m[1] / n, m[2] / n, first[0][0], first[0][1], first[0][2], first[1][0], first[1][1], first[1][2],
                           first[2][0], first[2][1], first[2][2], first[3][0], first[3][1], first[3][2], px[n - 1]);
                }
            }
        }
    }
    g_dtex[g_ndtex].view = g_dummy.v[v->smp[i].dim]; g_dtex[g_ndtex].smp = g_smp_default; g_dtex[g_ndtex].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (r & 1) {
        VTex *vt = vt_get(t);
        EView e;
        int attached = R.depth.tex == t, dim = v->smp[i].dim;
        for (int k = 0; k < R.ncol; k++) attached |= R.col[k].tex == t;
        eview_of(t, vt, &view, &e);
        int have = target_dim(e.target);
        int ok = have == dim || (dim == DIM_2D && (have == DIM_2DA || have == DIM_CUBE || have == DIM_CUBEA)) || (dim == DIM_2DA && (have == DIM_2D || have == DIM_CUBE || have == DIM_CUBEA)) ||
                 (dim == DIM_CUBE && have == DIM_CUBEA) || (dim == DIM_CUBEA && have == DIM_CUBE) || (dim == DIM_1D && have == DIM_1DA) || (dim == DIM_1DA && have == DIM_1D) ||
                 ((dim == DIM_CUBE || dim == DIM_CUBEA) && (have == DIM_2DA) && e.nlay >= 6 && vt->w == vt->h);
        {   static int want2 = -2;
            want2 = passshot_frame() ? 1 : -1;
            if (want2 >= 0)
                printf("nvn_vk_bind:   shader wants dim %d, view gives dim %d (view target %d, %d layers, levels %d+%d), samples %d, attached %d -> %s\n",
                       dim, have, e.target, e.nlay, e.lvl0, e.nlvl, vt->samples, attached, ok && vt->samples == 1 ? "BOUND" : "DUMMY (black)");
        }
        if (ok && vt->samples == 1) {
            if (attached && have_targets_live()) { if (!vt->feedback) { vt->feedback = 1; R.feedback++; pass_end(); } vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL); }
            else vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_dtex[g_ndtex].view = vt_view(vt, &e, dim, 0); g_dtex[g_ndtex].layout = vt->layout;
        } else { static int n; R.dummy_kind++;
            if (n < 3) printf("nvn_vk: (detail of the next line) frame %lld, program #%d, texture format 0x%x, %d x %d x %d, %d levels, draws so far with a dummy for a view-kind mismatch: %lld\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.format, t->b.width, t->b.height, t->b.depth, nvn_front_tex_levels(t), (long long)R.dummy_kind);
            if (n++ < 20) printf("nvn_vk: texture unit %u of the %s stage: the shader declares view kind %d, the texture is kind %d (target %d, %d samples): a dummy is bound\n", u, s ? "fragment" : "vertex", dim, have, e.target, vt->samples); }
        if (r & 2) g_dtex[g_ndtex].smp = sampler_get(&sp);
    }
    g_ndtex++;
}
static int have_targets_live(void);
/* Track O: a rectangle of the game's pixels (scissor, clear) on the scaled images of the pass: every image pixel the
 * rectangle touches (floor of the first edge, ceiling of the last). */
static void rs_rect(int *x, int *y, int *w, int *h) {
    int x0 = (int)floorf((float)*x * R.pass_sx), y0 = (int)floorf((float)*y * R.pass_sy);
    int x1 = (int)ceilf((float)(*x + *w) * R.pass_sx - 0.001f), y1 = (int)ceilf((float)(*y + *h) * R.pass_sy - 0.001f);
    *x = x0; *y = y0; *w = x1 > x0 ? x1 - x0 : 0; *h = y1 > y0 ? y1 - y0 : 0;
}
static void set_dynamic_state(void) {
    VkViewport vp;
    VkRect2D sc;
    uint64_t d = R.ds;
    float x = R.have_viewport ? R.viewport[0] : 0, y = R.have_viewport ? R.viewport[1] : 0, w = R.have_viewport ? R.viewport[2] : (float)R.pass_w, h = R.have_viewport ? R.viewport[3] : (float)R.pass_h;
    if (R.pass_rs && R.have_viewport) { x *= R.pass_sx; w *= R.pass_sx; y *= R.pass_sy; h *= R.pass_sy; }      /* track O: the game's rectangle on the scaled image */
    vp.x = x; vp.y = y + h; vp.width = w; vp.height = -h; vp.minDepth = R.drange[0]; vp.maxDepth = R.drange[1];      /* NVN upper-left origin: clip y = +1 is row y (design, section 3) */
    if (vp.minDepth < 0) vp.minDepth = 0; if (vp.maxDepth > 1) vp.maxDepth = 1;
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
    if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
    if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
    if (sx > R.pass_w) sx = R.pass_w; if (sy > R.pass_h) sy = R.pass_h;
    if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
    sc.offset.x = sx; sc.offset.y = sy; sc.extent.width = (uint32_t)(sw > 0 ? sw : 0); sc.extent.height = (uint32_t)(sh > 0 ? sh : 0);
    vkCmdSetScissor(R.cmd, 0, 1, &sc);
    if (R.pass_rs && D.f2.features.wideLines) { float lw = R.line_width * R.pass_sx; vkCmdSetLineWidth(R.cmd, lw < V.props.limits.lineWidthRange[0] ? V.props.limits.lineWidthRange[0] : lw); } else
    vkCmdSetLineWidth(R.cmd, D.f2.features.wideLines ? R.line_width : 1.0f);
    vkCmdSetDepthBias(R.cmd, R.polyoff[1], D.f2.features.depthBiasClamp ? R.polyoff[2] : 0.0f, R.polyoff[0]);
    vkCmdSetBlendConstants(R.cmd, R.blendc);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.svmask[0]); vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.svmask[1]);
    vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.swmask[0]); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.swmask[1]);
    vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.sref[0]); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.sref[1]);
    { static int nocull = -1; if (nocull < 0) nocull = getenv("SMO_VK_DIAG_NOCULL") != NULL;      /* SMO port: diagnosis only */
      vkCmdSetCullMode(R.cmd, nocull ? 0 : (R.poly[0] & 3)); }
    vkCmdSetFrontFace(R.cmd, R.poly[1] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    { static int nodepth = -1; if (nodepth < 0) nodepth = getenv("SMO_VK_DIAG_NODEPTH") != NULL;      /* SMO port: diagnosis only */
      vkCmdSetDepthTestEnable(R.cmd, nodepth ? 0 : (d & 1)); vkCmdSetDepthWriteEnable(R.cmd, (d >> 1) & 1); }
    { unsigned f = (unsigned)(d >> 4) & 15; vkCmdSetDepthCompareOp(R.cmd, (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 7)); }
    vkCmdSetStencilTestEnable(R.cmd, (d >> 2) & 1);
    {   static const uint8_t op[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
#define SOP(sh) ((VkStencilOp)op[((d >> (sh)) & 15) < 9 ? ((d >> (sh)) & 15) : 1])
#define SFN(sh) ((VkCompareOp)((((d >> (sh)) & 15) >= 1 && ((d >> (sh)) & 15) <= 8) ? ((d >> (sh)) & 15) - 1 : 7))
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_BIT, SOP(16), SOP(24), SOP(20), SFN(8));       /* (fail, pass, depth fail, compare); NVN order: stencil fail, depth fail, depth pass */
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_BACK_BIT, SOP(28), SOP(36), SOP(32), SFN(12));
#undef SOP
#undef SFN
    }
    vkCmdSetDepthBiasEnable(R.cmd, (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1);          /* offset enable bits: point, line, fill; the polygon mode picks one */
    vkCmdSetRasterizerDiscardEnable(R.cmd, R.rast_discard);
}
static int have_targets_live(void) { return R.in_pass || R.have_targets; }
/* Session 4: the state that left the pipeline key (VK_EXT_extended_dynamic_state3). Set after a pipeline is bound, whenever
 * one of the values changed, the pass changed, or a pipeline of our own (static state) was bound in between. */
static void set_dynamic_state3(int samples) {
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
    for (int i = 0; i < 8; i++) {
        const uint8_t *b = R.blend[i];
        en[i] = R.blend_on >> i & 1; wm[i] = (R.chanmask >> (i * 4)) & 15;
        eq[i].srcColorBlendFactor = blend_factor(b[0]); eq[i].dstColorBlendFactor = blend_factor(b[1]); eq[i].colorBlendOp = blend_op(b[4]);
        eq[i].srcAlphaBlendFactor = blend_factor(b[2]); eq[i].dstAlphaBlendFactor = blend_factor(b[3]); eq[i].alphaBlendOp = blend_op(b[5]);
    }
    vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en);
    vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq);
    vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
    vkCmdSetPolygonModeEXT(R.cmd, R.poly[2] == 0 ? VK_POLYGON_MODE_POINT : R.poly[2] == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetAlphaToCoverageEnableEXT(R.cmd, R.ms[1] != 0);
    { VkSampleMask m = R.sample_mask; vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(samples ? samples : 1), &m); }
    if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, R.depth_clamp != 0);
    R.dyn3_samples = samples;
}

/* ---------------------------------------------------------------- compute (M3; design section 3, "DispatchCompute" and "Storage buffers written by compute")
 * A storage buffer a compute stage may write gets a Vulkan buffer of its own (CB: host-visible memory, filled from guest
 * memory at its first dispatch of the batch). Later commands of the SAME batch whose range lies inside it bind that buffer
 * (the fighter's vertex buffers: no round trip). At the end of the batch the command buffer is submitted and the CBs become
 * "pending"; when that submission has finished the ranges a dispatch REALLY wrote (mx_wm, one word per dispatch in the
 * stream ring) are copied back to guest memory (cb_land): at the latest before the next batch reads guest memory, earlier
 * when the render thread is idle or a sync is waited for. The OpenGL path copies back before the next draw; what guest
 * memory holds once the batch is done is the same. Anything that reads guest memory through a range that only partly lies
 * in a CB forces the copy-back at once (cb_sync: submit, wait; counted). */
typedef struct { uint64_t addr, size, cap, busy; VkBuffer buf; VMem mem; int state, dirty; uint8_t *snap; uint64_t snap_cap; } CB;     /* state: 0 free, 1 live in this batch, 2 pending (batch submitted); snap: guest memory as it was when the CB was filled */
static int g_cb_merge = -1;                        /* SMO_VK_CBMERGE=0: the whole range is copied back as before session 5 */
typedef struct { uint64_t wm_off; uint32_t written; int16_t cb[16]; } CDisp;
static CB g_cb[256];
static CDisp g_cdisp[2048];
static int g_ncdisp, g_cb_live, g_cb_pending, g_cb_cached = -1;
static uint64_t g_cb_serial;                       /* the submission the pending CBs wait for */
static void cb_masks(void) {                       /* every recorded dispatch has run: which buffers did each one write? */
    for (int i = 0; i < g_ncdisp; i++) {
        uint32_t wm;
        memcpy(&wm, (uint8_t *)R.ring_mem.map + g_cdisp[i].wm_off, 4);
        wm &= g_cdisp[i].written;
        for (int k = 0; k < 16; k++) if ((wm >> k & 1) && g_cdisp[i].cb[k] >= 0) g_cb[g_cdisp[i].cb[k]].dirty = 1;
    }
    g_ncdisp = 0;
}
static uint32_t g_cb_epoch;
/* midbatch: the command being prepared may still name a freed CB's buffer (it is recorded into the next submission), so
 * that buffer is not given out again before that submission has finished. */
static void cb_land(int midbatch) {                /* the GPU has finished everything that wrote a CB */
    LONG64 t0 = tk();
    cb_masks();
    for (int i = 0; i < 256; i++) {
        CB *c = &g_cb[i];
        if (!c->state) continue;
        if (c->dirty) host_wrote(c->addr, c->size, GEN_CB);      /* guest memory changes: ranges the ring copied from it are copied again */
        if (c->dirty && !c->snap) { memcpy(P(c->addr), c->mem.map, (size_t)c->size); R.cb_bytes += (LONG64)c->size; }
        else if (c->dirty) {
            /* Session 5: the copy-back is DEFERRED (up to the next batch), so the guest may have written into the range since
             * the CB was filled. Only what compute changed goes back: a word the guest changed meanwhile (guest != snapshot)
             * is kept and counted; when compute changed that word too it is a real conflict (counted apart; the guest's later
             * write stays, as on the OpenGL path where the copy-back comes before it). */
            uint64_t *g = P(c->addr);
            const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)c->mem.map;
            size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
            LONG64 gw = 0, cf = 0;
            for (size_t k = 0; k < nw; k++) {
                uint64_t sv = s[k], mv = m[k];
                if (mv == sv) { if (g[k] != sv) gw++; continue; }
                if (g[k] == sv) g[k] = mv; else { gw++; cf++; }
            }
            if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
            R.cb_bytes += (LONG64)c->size; R.cb_guest_words += gw; R.cb_conflict_words += cf;
            if (gw) { static int n; if (n++ < 10) printf("nvn_vk: copy-back of %llx+%llu: the guest wrote %lld words of the range while it was pending (%lld of them also written by compute); merged\n", (unsigned long long)c->addr, (unsigned long long)c->size, (long long)gw, (long long)cf); }
        }
        c->state = c->dirty = 0;
        c->busy = midbatch ? R.serial + 1 : 0;
    }
    g_cb_live = g_cb_pending = 0;
    nvn_front_ccmp_landed();                       /* SMO_COMPUTE_CMP: what the dispatches wrote is in guest memory now */
    g_cb_epoch++;
    g_tm[1] += tk() - t0;
}
static void cb_ring_wrap(void) { if (g_cb_pending) cb_land(0); else cb_masks(); }      /* after cmd_wait_all: the mask words in the ring are about to be reused */
static void cb_poll(void) {                        /* pending CBs whose submission has finished are copied back; never waits */
    if (!g_cb_pending) return;
    if (R.done < g_cb_serial) for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && vkGetFenceStatus(V.device, R.slot[i].fence) == VK_SUCCESS) slot_wait(&R.slot[i]);
    if (R.done >= g_cb_serial) cb_land(0);
}
static void cb_sync(void) {                        /* guest memory must hold what compute wrote, now */
    int mid = g_cb_live != 0;
    if (!g_cb_live && !g_cb_pending) return;
    if (g_cb_live) { g_cb_serial = cmd_submit(NULL, NULL); R.cb_syncs++; }
    if (R.done < g_cb_serial) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && R.slot[i].serial <= g_cb_serial) slot_wait(&R.slot[i]);
        if (R.done < g_cb_serial) R.done = g_cb_serial;
        QueryPerformanceCounter(&t1);
        R.cb_waits++; R.cb_wait_ticks += t1.QuadPart - t0.QuadPart;
    }
    cb_land(mid);
}
static void occ_pool(void) {
    if (g_occ_pool) return;
    if (!D.f12.hostQueryReset) nvn_vk_missing("occlusion counters on a GPU without hostQueryReset");
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = OCC_MAX;
    VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_occ_pool));
    vkResetQueryPool(V.device, g_occ_pool, 0, OCC_MAX);
    g_occ_sync = getenv("SMO_VK_OCC_SYNC") && atoi(getenv("SMO_VK_OCC_SYNC"));
    g_occ_present = !getenv("SMO_VK_OCC_PRESENT") || atoi(getenv("SMO_VK_OCC_PRESENT"));
}
/* SMO_VK_GS_DEBUG=1 or 3 (gs_dbg_mode), draws with a geometry stage. (a) The pipeline statistics of 16 of them (every 25th),
 * printed once the GPU has run them (gsq_poll at every present): primitives in, vertex and geometry invocations, primitives
 * the geometry stage put out, primitives into and out of clipping, fragment invocations. (b) gs_dbg_read: the colour target
 * of every 20th of the first 400, read just before and just after the draw: how many texels the draw brightened and by how
 * much (R11G11B10F targets only). */
static VkQueryPool g_gsq_pool;
static int g_gsq_n, g_gsq_read, g_gsq_open;
static void gsq_begin(void) {
    if (g_gsq_n >= 16 || g_gs_draws % 25 != 1 || (gs_dbg_mode() != 1 && gs_dbg_mode() != 3) || !D.f2.features.pipelineStatisticsQuery) return;
    if (!g_gsq_pool) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = 16; qi.pipelineStatistics = 0xFE;      /* everything but input vertices */
        VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_gsq_pool));
        vkResetQueryPool(V.device, g_gsq_pool, 0, 16);
    }
    vkCmdBeginQuery(R.cmd, g_gsq_pool, (uint32_t)g_gsq_n, 0); g_gsq_open = 1;
}
static void gsq_end(void) { if (g_gsq_open) { vkCmdEndQuery(R.cmd, g_gsq_pool, (uint32_t)g_gsq_n++); g_gsq_open = 0; } }
static void gsq_poll(void) {
    while (g_gsq_pool && g_gsq_read < g_gsq_n) {
        uint64_t r[8];
        if (vkGetQueryPoolResults(V.device, g_gsq_pool, (uint32_t)g_gsq_read, 1, sizeof r, r, sizeof r, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !r[7]) return;
        printf("nvn_vk_gs: statistics of draw %d: primitives in %llu, vertex invocations %llu, geometry invocations %llu, geometry primitives out %llu, clipping in %llu, clipping out %llu, fragment invocations %llu\n",
               1 + 25 * g_gsq_read, (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6]);
        g_gsq_read++;
    }
}
static uint32_t *g_gsr_before;
static Tex *g_gsr_t;
static float r11g11b10_sum(uint32_t v) {          /* the three channels of an R11G11B10F texel, added */
    uint32_t c[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22}, mb[3] = {6, 6, 5};
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) { uint32_t e = c[i] >> mb[i], m = c[i] & ((1u << mb[i]) - 1); sum += e ? ldexpf(1.0f + (float)m / (float)(1u << mb[i]), (int)e - 15) : ldexpf((float)m, -14 - (int)mb[i]); }
    return sum;
}
static void gs_dbg_read(int after) {
    Tex *t = R.col[0].tex;
    if (!t || t->b.format != 0x3f) return;
    int w = t->b.width, h = t->b.height, rr[6] = {0, 0, 0, w, h, 1};
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px || !read_region(t, NULL, rr, px, 0, 0)) { free(px); return; }
    if (!after) { free(g_gsr_before); g_gsr_before = px; g_gsr_t = t; return; }
    if (g_gsr_before && g_gsr_t == t) {
        int n = 0, dbl = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
        double sum = 0.0, base = 0.0, mx = 0.0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t k = (size_t)y * w + x;
                if (px[k] == g_gsr_before[k]) continue;
                float b0 = r11g11b10_sum(g_gsr_before[k]), d = r11g11b10_sum(px[k]) - b0;
                if (d <= 0.001f) continue;
                n++; sum += d; base += b0; if (d > mx) mx = d; if (d > b0) dbl++;
                if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y;
            }
        printf("nvn_vk_gs: draw %lld (frame %lld): target %dx%d: %d texels brightened, by %.3f on average (max %.3f) over %.3f before; %d more than doubled; box %d,%d .. %d,%d\n",
               (long long)g_gs_draws, (long long)nvn_front_frame_next(), w, h, n, n ? sum / n : 0.0, mx, n ? base / n : 0.0, dbl, x0, y0, x1, y1);
    }
    free(px); free(g_gsr_before); g_gsr_before = NULL;
}
static void occ_draw(void) {                       /* after draw_prepare, inside a pass */
    if (!g_occ_on || g_occ_open || !R.in_pass) return;
    if (g_occ_head - g_occ_tail >= OCC_MAX) {      /* full within one batch (occ_fold makes room between batches) */
        nvn_vk_missing("more than 65536 occlusion count segments waiting for the GPU within one batch");
        return;
    }
    vkCmdBeginQuery(R.cmd, g_occ_pool, (uint32_t)(g_occ_head % OCC_MAX), D.f2.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);   /* NVN counts samples */
    g_occ_head++;
    g_occ_seg_serial = R.serial + 1;               /* the number the command buffer being recorded will get */
    g_occ_open = 1;
}
/* Has submission `serial` finished? With wait it is waited for (submitted first while it is still being recorded). */
static int occ_done(uint64_t serial, int wait) {
    if (serial <= R.done) return 1;
    LONG64 t0 = wait ? tk() : 0;
    if (wait && serial > R.serial) cmd_submit(NULL, NULL);
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (s->pending && s->serial <= serial && (wait || vkGetFenceStatus(V.device, s->fence) == VK_SUCCESS)) slot_wait(s);
    }
    if (wait) { if (R.done < serial) R.done = serial; g_occ_waits++; g_occ_wait_ticks += tk() - t0; }
    return serial <= R.done;
}
/* Segments [lo, hi) have run: their samples are added to *sum (none when sum is NULL) and their queries are free again. */
static void occ_take(uint64_t lo, uint64_t hi, uint64_t *sum) {
    static uint64_t res[OCC_MAX];
    while (lo < hi) {
        uint32_t first = (uint32_t)(lo % OCC_MAX);
        uint32_t n = hi - lo < (uint64_t)(OCC_MAX - first) ? (uint32_t)(hi - lo) : OCC_MAX - first;
        if (sum) {
            VKC(vkGetQueryPoolResults(V.device, g_occ_pool, first, n, (size_t)n * 8, res, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            for (uint32_t i = 0; i < n; i++) *sum += res[i];
        }
        vkResetQueryPool(V.device, g_occ_pool, first, n);
        lo += n;
    }
}
/* The queued entries in their order: the first `need` of them are waited for where the GPU has not finished them yet,
 * the ones after those as long as their submission has finished. */
static void occ_resolve(int need) {
    while (g_occ_qn) {
        OccEntry *e = &g_occ_q[g_occ_qh];
        if (!occ_done(e->serial, need > 0)) break;
        uint64_t sum = 0;
        occ_take(e->lo, e->hi, e->kind == 1 ? NULL : &sum);
        g_occ_tail = e->hi;
        if (e->kind == 1) g_occ_total = 0;
        else g_occ_total += sum;
        if (e->kind == 0 && e->dst) {
            uint64_t d[2] = {g_occ_total, vk_now_ns()};
            memcpy(P(e->dst), d, 16);
            host_wrote(e->dst, 16, GEN_OCC);      /* ranges the ring copied from there are copied again */
            if (nvn_front_frame_next() > e->frame) g_occ_late++;
            if (e->log) printf("nvn_vk_occ: frame %lld report %llu samples to %llx (segments %llu..%llu), written in frame %lld\n", (long long)e->frame, (unsigned long long)g_occ_total,
                               (unsigned long long)e->dst, (unsigned long long)e->lo, (unsigned long long)e->hi, nvn_front_frame_next());      /* SMO debug aid (SMO_VK_PASSSHOT) */
        }
        g_occ_qh = (g_occ_qh + 1) % OCC_Q; g_occ_qn--; need--;
    }
}
static void occ_push(int kind, uint64_t dst) {     /* queues an entry for the segments since the last one */
    occ_end();
    if (g_occ_qn == OCC_Q) occ_resolve(OCC_Q / 2);
    OccEntry *e = &g_occ_q[(g_occ_qh + g_occ_qn) % OCC_Q];
    e->kind = kind; e->dst = dst; e->lo = g_occ_mark; e->hi = g_occ_head;
    e->serial = e->hi > e->lo ? g_occ_seg_serial : 0;
    e->frame = nvn_front_frame_next(); e->log = 0;
    g_occ_mark = g_occ_head;
    g_occ_qn++;
}
/* A count that stays open over many passes (SMO's Top-Hat Tower stage: about 136 segments a frame): at the end of a
 * batch, when half of the ring is in use, its segments so far are added up into the count (a fold entry) and the GPU is
 * waited for. Segments of a count that was reset without a report are dropped by the reset's entry. */
static void occ_fold(void) {
    if (!g_occ_pool || g_occ_head - g_occ_tail < OCC_MAX / 2) return;
    occ_resolve(0);
    if (g_occ_head - g_occ_tail < OCC_MAX / 2) return;
    if (g_occ_head > g_occ_mark) { occ_push(2, 0); g_occ_folds++; }
    occ_resolve(g_occ_qn);
}
static void occ_reset(void) {
    occ_pool();
    occ_end();
    if (!g_occ_qn && g_occ_head == g_occ_mark) g_occ_total = 0;      /* nothing queued: the count starts again at once */
    else occ_push(1, 0);
    g_occ_on = 1;
}
static void occ_report(uint64_t dst) {
    occ_pool();
    occ_push(0, dst);
    g_occ_reports++;
    if (passshot_frame()) g_occ_q[(g_occ_qh + g_occ_qn - 1) % OCC_Q].log = 1;
    if (g_occ_sync) occ_resolve(g_occ_qn);
}
static void cb_batch_end(void) {
    if (!g_cb_live) return;
    g_cb_serial = cmd_submit(NULL, NULL);
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 1) g_cb[i].state = 2;
    g_cb_pending = g_cb_live; g_cb_live = 0;
}
/* The CBs of this batch (state 1), in the order they were made: the first g_cb_live entries (every change of a CB out of
 * state 1 sets g_cb_live to 0). Every range a draw reads is looked up here while compute results are live, about 10,000
 * a frame: walking the 256 slots for the dozen CBs of a frame cost 0.5-0.7 ms a frame once they stayed live longer. */
static int16_t g_cb_live_list[256];
static CB *cb_inside(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_cb_live; k++) { CB *c = &g_cb[g_cb_live_list[k]]; if (c->state == 1 && addr >= c->addr && addr + size <= c->addr + c->size) return c; }
    return NULL;
}
static int cb_overlap(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_cb_live; k++) { const CB *c = &g_cb[g_cb_live_list[k]]; if (c->state == 1 && addr < c->addr + c->size && c->addr < addr + size) return 1; }
    return 0;
}
static CB *cb_new(uint64_t addr, uint64_t size) {
    CB *c = NULL, *empty = NULL, *victim = NULL;
    for (int i = 0; i < 256 && !c; i++) {
        CB *k = &g_cb[i];
        if (k->state || k->busy > R.done) continue;
        if (!k->buf) { if (!empty) empty = k; }
        else if (k->cap >= size && k->cap <= size * 2 + 4096) c = k;
        else if (!victim) victim = k;
    }
    if (!c) {
        c = empty ? empty : victim;
        if (!c) vkd_fail("more than 256 storage buffers written by compute in one batch");
        if (c->buf) { vkDestroyBuffer(V.device, c->buf, NULL); vmem_free(c->mem); c->buf = NULL; }      /* free and not busy: nothing in flight names it */
        if (g_cb_cached < 0) {                     /* read back by the CPU: cached host memory when the device has it */
            g_cb_cached = 0;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((D.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) g_cb_cached = 1;
        }
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements rq;
        c->cap = (size + 4095) & ~(uint64_t)4095;
        bi.size = c->cap; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device, &bi, NULL, &c->buf));
        vkGetBufferMemoryRequirements(V.device, c->buf, &rq);
        VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (g_cb_cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        int found = 0;
        for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) found = 1;
        if (!found) want &= ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        c->mem = vmem_alloc(rq, want, 2);
        VKC(vkBindBufferMemory(V.device, c->buf, c->mem.mem, c->mem.off));
        R.cb_made++;
    }
    c->addr = addr; c->size = size; c->state = 1; c->dirty = 0;
    if (g_cb_merge < 0) g_cb_merge = !(getenv("SMO_VK_CBMERGE") && !atoi(getenv("SMO_VK_CBMERGE")));
    if (g_cb_merge) {                              /* the snapshot the copy-back is compared with (cb_land) */
        if (c->snap_cap < size) { free(c->snap); c->snap_cap = (size + 4095) & ~(uint64_t)4095; c->snap = malloc((size_t)c->snap_cap); }
        memcpy(c->snap, P(addr), (size_t)size);
        memcpy(c->mem.map, c->snap, (size_t)size);
    } else
    memcpy(c->mem.map, P(addr), (size_t)size);
    g_cb_live_list[g_cb_live++] = (int16_t)(c - g_cb);
    return c;
}
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
/* Where a command reads the guest range from: a CB of this batch that contains it, else the stream ring. align: the offset
 * alignment the use needs (storage descriptors), 1 = none. Returns 1 when a CB is bound. */
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b) {
    b->zero = 0; b->size = size;
    if (g_cb_live) {
        CB *c = cb_inside(addr, size);
        if (c && (addr - c->addr) % align == 0) { b->buf = c->buf; b->off = addr - c->addr; return 1; }
        if (cb_overlap(addr, size)) cb_sync();
    }
    b->off = ring_put(addr, size); b->buf = R.ring;      /* in this order: a ring wrap submits */
    return 0;
}
static void compute_pipe_make(VStage *v);
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz);
static void compute_dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    Prog *p = R.prog[5];
    static int nlog;
    R.dispatches++;
    if (!p) { if (nlog++ < 10) printf("nvn_vk: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    if (!st->code) { nvn_vk_missing("nvnCommandBufferDispatchCompute of a program that has no machine code (assembly text only)"); return; }
    uint32_t key[4] = {0, 0, R.tex_int[5], R.tex_sint[5]};
    VStage *v = vstage_get(p, 5, key, 0, 0);
    if (!v) { nvn_vk_missing("nvnCommandBufferDispatchCompute: the compute stage could not be translated or compiled (the log names the program)"); return; }
    if (v->nsmp > 32) { nvn_vk_missing("a compute program with more than 32 samplers"); return; }
    if (!gx || !gy || !gz) return;
    int cp_how = 0;
    if (v->cp_state != 2) { LONG64 t0 = tk(); cp_how = compute_pipe_ensure(v); if (cp_how) { hold_note(cp_how == 1 ? H_PIPE_DRAW : H_PIPE_WAIT, tk() - t0); g_tm[5] += tk() - t0; } }
    if (!v->cp_recorded) { v->cp_recorded = 1; if (!v->images) piperec_append(NULL, v, NULL, 1); }      /* SMO: a stage with images has no record to be made from at start */
    if (cp_how == 1) {
        g_pipe_miss++;
        printf("nvn_vk: compute program #%d: pipeline made at its first dispatch (%d bytes of machine code, work group %u x %u x %u, constant buffers 0x%x, storage 0x%x written 0x%x, %d samplers)\n", p->serial, st->code_len,
               st->local[0], st->local[1], st->local[2], v->cbufs, v->ssbos, v->ssbo_w, v->nsmp);
    }
    compute_dispatch_run(p, st, v, gx, gy, gz);
}
/* SMO: images of compute programs (nvnCommandBufferBindImage; 14 compute stages of the game: 13 of agl's post effects,
 * one of alComputeShader). The NVN image handle is the texture's index in the texture pool; the texture must have been made with
 * NVN_TEXTURE_FLAGS_IMAGE (its image then has VK_IMAGE_USAGE_STORAGE_BIT, vt_get2). A storage view has one level and the
 * identity swizzle; its format is the one the shader declares (typed accesses, atomics) or the texture view's own. */
static LONG64 g_img_disp, g_img_views;
static VkFormat gl_image_vkfmt(uint16_t gl) {      /* the image formats of mx_translate.c img_qualifier */
    static const struct { uint16_t gl; VkFormat vk; } m[] = {
        {0x8814, VK_FORMAT_R32G32B32A32_SFLOAT}, {0x881A, VK_FORMAT_R16G16B16A16_SFLOAT}, {0x8230, VK_FORMAT_R32G32_SFLOAT}, {0x822F, VK_FORMAT_R16G16_SFLOAT},
        {0x8C3A, VK_FORMAT_B10G11R11_UFLOAT_PACK32}, {0x822E, VK_FORMAT_R32_SFLOAT}, {0x822D, VK_FORMAT_R16_SFLOAT}, {0x805B, VK_FORMAT_R16G16B16A16_UNORM},
        {0x8059, VK_FORMAT_A2B10G10R10_UNORM_PACK32}, {0x8058, VK_FORMAT_R8G8B8A8_UNORM}, {0x822C, VK_FORMAT_R16G16_UNORM}, {0x822B, VK_FORMAT_R8G8_UNORM},
        {0x822A, VK_FORMAT_R16_UNORM}, {0x8229, VK_FORMAT_R8_UNORM}, {0x8F9B, VK_FORMAT_R16G16B16A16_SNORM}, {0x8F97, VK_FORMAT_R8G8B8A8_SNORM},
        {0x8F99, VK_FORMAT_R16G16_SNORM}, {0x8F95, VK_FORMAT_R8G8_SNORM}, {0x8F98, VK_FORMAT_R16_SNORM}, {0x8F94, VK_FORMAT_R8_SNORM},
        {0x8D70, VK_FORMAT_R32G32B32A32_UINT}, {0x8D76, VK_FORMAT_R16G16B16A16_UINT}, {0x906F, VK_FORMAT_A2B10G10R10_UINT_PACK32}, {0x8D7C, VK_FORMAT_R8G8B8A8_UINT},
        {0x823C, VK_FORMAT_R32G32_UINT}, {0x823A, VK_FORMAT_R16G16_UINT}, {0x8238, VK_FORMAT_R8G8_UINT}, {0x8236, VK_FORMAT_R32_UINT}, {0x8234, VK_FORMAT_R16_UINT},
        {0x8232, VK_FORMAT_R8_UINT}, {0x8D82, VK_FORMAT_R32G32B32A32_SINT}, {0x8D88, VK_FORMAT_R16G16B16A16_SINT}, {0x8D8E, VK_FORMAT_R8G8B8A8_SINT},
        {0x823B, VK_FORMAT_R32G32_SINT}, {0x8239, VK_FORMAT_R16G16_SINT}, {0x8237, VK_FORMAT_R8G8_SINT}, {0x8235, VK_FORMAT_R32_SINT}, {0x8233, VK_FORMAT_R16_SINT},
        {0x8231, VK_FORMAT_R8_SINT}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].gl == gl) return m[i].vk;
    return VK_FORMAT_UNDEFINED;
}
static VkImageView vt_storage_view(VTex *vt, const EView *e, int ty, VkFormat fmt) {      /* cached with the other views (attach 2) */
    static const VkImageViewType vtype[6] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D};
    EView key = *e;
    memset(key.swz, 0, sizeof key.swz); key.nlvl = 1; key.format = (int)fmt; key.dsmode = 0;
    if (ty != 2 && ty != 4) key.nlay = 1;
    for (VView *n = vt->views; n; n = n->next) if (n->attach == 2 && n->vtype == ty && !memcmp(&n->e, &key, sizeof key)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    n->e = key; n->vtype = ty; n->attach = 2;
    ci.image = vt->img; ci.format = fmt; ci.viewType = vtype[ty];
    ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = vt->is3d ? 0 : (uint32_t)e->lay0; ci.subresourceRange.layerCount = vt->is3d ? 1 : (uint32_t)key.nlay;
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (storage, type %d, format %d of image format %d, level %d, layers %d+%d): %s", (int)ci.viewType, (int)fmt, (int)vt->fmt, e->lvl0, e->lay0, key.nlay, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++; g_img_views++;
    return n->view;
}
/* The images a compute stage uses: a storage view of each, in the GENERAL layout. 0 (the dispatch is left out, said once per
 * reason) when one is not bound or cannot be bound. */
static int compute_images(Prog *p, VStage *v, VkImageView *iview, Tex **itex) {
    for (unsigned k = 0; k < 8; k++) {
        Tex *t = NULL;
        View view;
        SamplerParams sp;
        iview[k] = NULL; itex[k] = NULL;
        if (!(v->images >> k & 1)) continue;
        if (!(nvn_front_handle(R.tpool, NULL, R.nvn_img[5][k] & 0xfffff, &t, &view, &sp) & 1)) {
            static int n; if (n++ < 10) printf("nvn_vk: compute program #%d uses image %u but none is bound (handle %llx): dispatch left out\n", p->serial, k, (unsigned long long)R.nvn_img[5][k]);
            return 0;
        }
        VTex *vt = vt_get(t);
        int ty = v->img_type[k];
        EView e;
        eview_of(t, vt, &view, &e);
        VkFormat f = v->img_fmt[k] ? gl_image_vkfmt(v->img_fmt[k]) : VK_FORMAT_UNDEFINED;
        if (!f) { int ex, dec; f = tex_vkfmt(e.format, &ex, &dec); if (!f || ex || dec) f = vt->fmt; }
        if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_R8G8B8A8_UNORM; else if (f == VK_FORMAT_B8G8R8A8_SRGB) f = VK_FORMAT_B8G8R8A8_UNORM;      /* no sRGB storage */
        const char *why = !vt->storage ? "the texture was not made with NVN_TEXTURE_FLAGS_IMAGE" : ty == 1 ? "an image buffer (texel buffer) is not supported" :
                          !fmt_supported(f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "its format has no storage support on this GPU" :
                          (ty == 5 ? 2 : ty >= 3 ? 1 : 0) != (vt->is3d ? 2 : (t->b.target == 0 || t->b.target == 3) ? 0 : 1) ? "the shader's image type (1D, 2D or 3D) does not fit the texture" : NULL;
        if (why) {
            static int n; if (n++ < 10) printf("nvn_vk: compute program #%d, image %u (texture format 0x%x, %dx%dx%d, flags 0x%x, view format %d, type %d): %s: dispatch left out\n", p->serial, k, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.flags, (int)f, ty, why);
            nvn_vk_missing("a compute image that cannot be bound (the log says why)");
            return 0;
        }
        {   static int n; if (n++ < 20) printf("nvn_vk: compute program #%d binds image %u: texture format 0x%x %dx%dx%d flags 0x%x, level %d, layers %d+%d, view format %d, type %d, %s\n", p->serial, k, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.flags, e.lvl0, e.lay0, e.nlay, (int)f, ty, (v->images_w >> k & 1) ? "written" : "read only"); }
        vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL);
        iview[k] = vt_storage_view(vt, &e, ty, f); itex[k] = t;
    }
    return 1;
}
static void compute_pipe_make(VStage *v) {         /* any thread */
    {
        VkDescriptorSetLayoutBinding b0[40], b1[64];
        int n0 = 0, n1 = 0;
        for (unsigned n = 0; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        if (v->ssbo_w) { VkDescriptorSetLayoutBinding b = {38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }
        for (unsigned k = 0; k < 8; k++) if (v->images >> k & 1) { VkDescriptorSetLayoutBinding b = {32 + k, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }      /* SMO: MxIn.image_base 32 */
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &v->cdl[i]));
        }
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = v->cdl;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &v->cl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = v->mod; ci.stage.pName = "main"; ci.layout = v->cl;
        VkPipeline cp = NULL;
        VkResult r = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, &cp);
        if (r != VK_SUCCESS) vkd_fail("vkCreateComputePipelines (compute stage %u): %s", v->id, vk_result_name(r));
        v->cp = cp;
        InterlockedIncrement64(&R.pipelines);
    }
}
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz) {
    static BufRef bufs[40];
    int nbuf = 0;
    int16_t wcb[16];
    uint64_t al = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    pass_end();
    /* textures, then buffers (both may submit: upload, ring wrap, copy-back), then the mask word, then the commands */
    g_ndtex = 0;
    for (int i = 0; i < v->nsmp; i++) resolve_sampler(5, v, i);
    VkImageView iview[8];
    Tex *itex[8];
    if (v->images && !compute_images(p, v, iview, itex)) return;
    if (g_ncdisp == 2048 || g_cb_pending) cb_sync();
    uint64_t wm_off = 0;
    for (int attempt = 0; ; attempt++) {           /* a copy-back in here (cb_sync) frees the CBs chosen before it: everything is then chosen again */
        uint32_t epoch = g_cb_epoch;
        nbuf = 0;
        memset(wcb, 0xff, sizeof wcb);
        for (unsigned n = 0; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = c2_ring(5); b->size = sizeof R.c2[5]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[5][n - 3].addr && R.ubo[5][n - 3].size) buf_ref(R.ubo[5][n - 3].addr, R.ubo[5][n - 3].size, al, b);
            else b->zero = 1;                      /* c0 outside the storage descriptors, or nothing bound: zeros (OpenGL: nothing bound) */
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            Range r = R.ssbo[5][k];
            b->zero = !(r.addr && r.size); b->buf = R.ring;
            if (b->zero) continue;
            if (!(v->ssbo_w >> k & 1)) { buf_ref(r.addr, r.size, al, b); continue; }
            CB *c = g_cb_live ? cb_inside(r.addr, r.size) : NULL;
            if (c && (r.addr - c->addr) % al) c = NULL;
            if (!c) { if (g_cb_live && cb_overlap(r.addr, r.size)) cb_sync(); c = cb_new(r.addr, r.size); }
            b->buf = c->buf; b->off = r.addr - c->addr; b->size = r.size; wcb[k] = (int16_t)(c - g_cb);
        }
        if (v->ssbo_w) { static const uint32_t zero[4]; wm_off = ring_raw(zero, 16); }
        if (epoch == g_cb_epoch) break;
        if (attempt == 3) vkd_fail("compute program #%d: its buffers cannot be bound without a copy-back in between", p->serial);
    }
    nvn_front_ccmp_dispatch(R.ubo[5], R.ssbo[5], v->ssbo_w, st->code, st->code_len, gx, gy, gz);      /* SMO_COMPUTE_CMP */
    cmd_begin();
    static VkWriteDescriptorSet w[90];
    static VkDescriptorBufferInfo bi[40];
    static VkDescriptorImageInfo ii[72];
    VkDescriptorSet sets[2];
    int nw = 0, ib = 0;
    sets[0] = set_alloc(v->cdl[0]); sets[1] = set_alloc(v->cdl[1]);
    memset(w, 0, sizeof w);
    for (unsigned n = 0; n < 34; n++) {
        unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
        if (!bit) continue;
        BufRef *b = &bufs[ib];
        bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = n < 18 ? n : 20 + n - 18; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    if (v->ssbo_w) {
        bi[ib].buffer = R.ring; bi[ib].offset = wm_off; bi[ib].range = 16;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 38; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    for (int i = 0; i < v->nsmp; i++) {
        ii[i].sampler = g_dtex[i].smp; ii[i].imageView = g_dtex[i].view; ii[i].imageLayout = g_dtex[i].layout;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = v->smp[i].binding; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i]; nw++;
    }
    for (unsigned k = 0, j = 64; k < 8; k++) {     /* SMO: the images, in the GENERAL layout (compute_images) */
        if (!(v->images >> k & 1)) continue;
        ii[j].sampler = NULL; ii[j].imageView = iview[k]; ii[j].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = 32 + k; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[nw].pImageInfo = &ii[j]; nw++; j++;
    }
    if (nw) vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cp);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cl, 0, 2, sets, 0, NULL);
    {   unsigned k = mx_split_x(st->local, st->smem_bytes);      /* SMO: a work group over 1024 invocations runs as k groups along x (mx_translate.h) */
        if (!k) { nvn_vk_missing("a compute work group over 1024 invocations that uses shared memory"); return; }
        vkCmdDispatch(R.cmd, gx * k, gy, gz);
    }
    if (v->images_w) {                             /* SMO: what the dispatch wrote into images is read by later dispatches and draws (a layout change orders the rest) */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        for (unsigned k = 0; k < 8; k++) if ((v->images_w >> k & 1) && itex[k]) { itex[k]->gpu_written = 1; itex[k]->uploaded = 1; }
        g_img_disp++;
    }
    if (v->ssbo_w) {                               /* what the dispatch wrote is read by later dispatches and draws of this command buffer */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CDisp *d = &g_cdisp[g_ncdisp++];
        d->wm_off = wm_off; d->written = v->ssbo_w; memcpy(d->cb, wcb, sizeof wcb);
    }
}
/* Everything of a draw up to the draw call. topo: NVN primitive. Returns 0 when the draw cannot be done (counted as skipped). */
static int draw_prepare(unsigned prim) {
    {   /* SMO port, with SMO_VK_PASSSHOT: the state of each draw of that frame (every draw kind passes here) */
            static int want = -2;
            want = passshot_frame() ? 1 : -1;
            if (want >= 0)
                printf("nvn_vk_draw: prim %u programs vs #%d fs #%d gs #%d target %p | viewport %s %.0f %.0f %.0f %.0f | scissor %s %d %d %d %d | pass %dx%d in_pass %d | cull %u discard %d render_off %d | chanmask %08x blend_on %x blend0 %u %u %u %u %u %u | ds %llx\n",
                       prim, R.prog[0] ? R.prog[0]->serial : -1, R.prog[1] ? R.prog[1]->serial : -1, R.prog[2] ? R.prog[2]->serial : -1, (void *)R.col[0].tex, R.have_viewport ? "set" : "default", R.viewport[0], R.viewport[1], R.viewport[2], R.viewport[3],
                       R.have_scissor ? "set" : "default", R.scissor[0], R.scissor[1], R.scissor[2], R.scissor[3], R.pass_w, R.pass_h, R.in_pass,
                       (unsigned)(R.poly[0] & 3), R.rast_discard, R.render_off, R.chanmask, (unsigned)R.blend_on,
                       R.blend[0][0], R.blend[0][1], R.blend[0][2], R.blend[0][3], R.blend[0][4], R.blend[0][5], (unsigned long long)R.ds);
                if (want >= 0) printf("nvn_vk_draw:   depth target %p fmt 0x%x %dx%d drange %g..%g polyoff %g %g %g poly %u %u %u %u\n", (void *)R.depth.tex, R.depth.tex ? R.depth.tex->b.format : 0, R.depth.tex ? R.depth.tex->b.width : 0, R.depth.tex ? R.depth.tex->b.height : 0, R.drange[0], R.drange[1], R.polyoff[0], R.polyoff[1], R.polyoff[2], R.poly[0], R.poly[1], R.poly[2], R.poly[3]);
            if (want >= 0) {
                float ar; memcpy(&ar, &R.alpha_ref, 4);
                printf("nvn_vk_draw:   alpha func %u ref %g | vertex ubo0 (c3) addr %llx size %llx:", (unsigned)R.alpha_func, ar,
                       (unsigned long long)R.ubo[0][0].addr, (unsigned long long)R.ubo[0][0].size);
                if (R.ubo[0][0].addr && R.ubo[0][0].size >= 80) for (int k = 4; k < 20; k++) { float fv; memcpy(&fv, (const void *)(uintptr_t)(R.ubo[0][0].addr + 4u * k), 4); printf(" %g", fv); }
                printf("\n");
            }
        }
    static const int8_t topo[7] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    if (!R.have_targets || R.render_off) { if (R.render_off) g_fr_offskip++; return 0; }
    if (prim > 6 || topo[prim] < 0) { static char msg[64]; snprintf(msg, sizeof msg, "a draw with NVN primitive type %u", prim); nvn_vk_missing(msg); return 0; }
    if (!R.prog[0]) return 0;
    if (R.prog[3] || R.prog[4]) { nvn_vk_missing("a draw with a tessellation stage"); return 0; }      /* the game has no tessellation stage (tests/check_all_shaders.py) */
    if (R.prog[2] && !D.f2.features.geometryShader) { nvn_vk_missing("a draw with a geometry stage on a GPU without geometry shaders"); return 0; }
    if (R.prog[2] && gs_dbg_mode() == 2) return 0;      /* SMO_VK_GS_DEBUG=2: left out */
    uint32_t ints = 0, sints = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        int nf = (int)(R.vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { ints |= 1u << i; if (k == 3 || k == 8 || k == 11) sints |= 1u << i; }
        if (nf == 0x3E) ints |= 1u << i;
        if (nf == 0x6A) { ints |= 1u << i; sints |= 1u << i; }
    }
    uint32_t key0[4] = {ints, sints, R.tex_int[0], R.tex_sint[0]}, key1[4] = {0, 0, R.tex_int[1], R.tex_sint[1]};
    int depth_only = !R.prog[1];
    /* fragment inputs the vertex code never writes are declared by a vertex variant of their own, left at (0, 0, 0, 1). Session 4:
     * the variant the program used last is asked for first (session 3 asked for the plain one at every draw of such a pair,
     * which hashed the machine code each time). */
    VStage *cur = R.prog[0]->st[0].vk;
    VStage *fs = depth_only ? NULL : vstage_get(R.prog[1], 1, key1, 0, 0), *vs, *gs = NULL;
    if (R.prog[2]) {                               /* SMO: a geometry stage (the lightning effects), translated for the draw's primitive class; it writes the fragment inputs, the vertex stage its inputs */
        uint32_t keyg[4] = {0, 0, R.tex_int[2], R.tex_sint[2]};
        VStage *gcur = R.prog[2]->st[2].vk;
        gs = vstage_get(R.prog[2], 2, keyg, gcur ? gcur->fo : 0, tclass[prim]);
        if (gs) { uint32_t need = fs ? fs->in_attr & ~(gs->out_attr & ~gs->fo) : 0; if (need != gs->fo) gs = vstage_get(R.prog[2], 2, keyg, need, tclass[prim]); }
    }
    vs = vstage_get(R.prog[0], 0, key0, cur ? cur->fo : 0, 0);
    if (vs) { uint32_t want = R.prog[2] ? (gs ? gs->in_attr : 0) : fs ? fs->in_attr : 0, need = want & ~(vs->out_attr & ~vs->fo); if (need != vs->fo) vs = vstage_get(R.prog[0], 0, key0, need, 0); }
    if (g_skip_flag) { g_skip_flag = 0; if (!vs || (!depth_only && !fs) || (R.prog[2] && !gs)) return 0; }      /* shader_wait = skip: the draw is left out (counted in vstage_get) */
    if (!vs || (!depth_only && !fs) || (R.prog[2] && !gs)) { nvn_vk_missing("a draw whose vertex, fragment or geometry stage could not be translated or compiled (the log names the stage)"); return 0; }
    VStage *sv[3] = {vs, fs, gs};                  /* by NVN stage: the bindings of stage s are 40 * s + ... (mx_translate.h) */

    /* 1. textures (may upload, change layouts, end the pass) */
    g_ndtex = 0;
    for (int s = 0; s < 3; s++) if (sv[s]) for (int i = 0; i < sv[s]->nsmp; i++) resolve_sampler(s, sv[s], i);
    /* 2. buffers: every range the draw reads, into the ring (may wrap the ring = submit) */
    static BufRef bufs[120];
    int nbuf = 0;
    uint64_t sal = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    for (int s = 0; s < 3; s++) {
        VStage *v = sv[s];
        if (!v) continue;
        Stage *st = &R.prog[s]->st[s];
        for (unsigned n = 1; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = c2_ring(s); b->size = sizeof R.c2[s]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[s][n - 3].addr && R.ubo[s][n - 3].size) buf_ref(R.ubo[s][n - 3].addr, R.ubo[s][n - 3].size, sal, b);
            else b->zero = 1;
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = !(R.ssbo[s][k].addr && R.ssbo[s][k].size); b->buf = R.ring;
            if (!b->zero) buf_ref(R.ssbo[s][k].addr, R.ssbo[s][k].size, sal, b);
        }
    }
    /* vertex streams: only the ones an attribute names (a stale binding of another mesh is not read, and is not copied) */
    uint64_t voff[16];
    VkBuffer vbufs[16];
    uint32_t streams = 0;
    int from_cb = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) if (R.vattr[i] & 0xff) streams |= 1u << ((R.vattr[i] >> 8) & 31);
    for (int i = 0; i < 16; i++) {
        BufRef b;
        voff[i] = ~0ull; vbufs[i] = R.ring;
        if (!(streams >> i & 1) || !R.vbuf[i].addr || !R.vbuf[i].size) continue;
        from_cb |= buf_ref(R.vbuf[i].addr, g_vtxgrow && g_vneed[i] > R.vbuf[i].size ? g_vneed[i] : R.vbuf[i].size, 1, &b);
        voff[i] = b.off; vbufs[i] = b.buf;
    }
    R.cb_draws += from_cb;
    {   /* SMO debug aid, with SMO_VK_PASSSHOT: each attribute of the draw (format, stream, offset, stride) and the first two
         * vertices of attribute 0 as floats, read from guest memory */
        static int want = -2;
        want = passshot_frame() ? 1 : -1;
        if (want >= 0) {
            for (int i = 0; i < 4 && i < R.nattr; i++) {
                uint32_t a = R.vattr[i]; int st = (int)((a >> 8) & 31);
                if (!(a & 0xff)) continue;
                printf("nvn_vk_vtx:   attr %d fmt 0x%x stream %d offset %u stride %u addr %llx size %llx", i, a & 0xff, st, a >> 14, st < 32 ? R.vstream[st].stride : 0, (unsigned long long)(st < 16 ? R.vbuf[st].addr : 0), (unsigned long long)(st < 16 ? R.vbuf[st].size : 0));
                if (i == 0 && st < 16 && R.vbuf[st].addr && (a & 0xff) >= 0x1f && (a & 0xff) <= 0x24 + 12) {
                    const float *fp = (const float *)P(R.vbuf[st].addr + (a >> 14));
                    uint32_t sd = R.vstream[st].stride ? R.vstream[st].stride / 4 : 4;
                    printf(" | v0 %g %g %g  v1 %g %g %g", fp[0], fp[1], fp[2], fp[sd], fp[sd + 1], fp[sd + 2]);
                }
                printf("\n");
            }
        }
    }
    /* 3. pass and pipeline */
    if (!pass_begin()) return 0;
    PKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.fs = fs ? fs->id : 0; k.topo_class = tclass[prim]; k.logic = R.logic; k.sshade = fs ? (uint8_t)R.prog[1]->sample_shading : 0;
    if (!D.has_eds3) {                             /* without extended dynamic state 3 this state is part of the key */
        k.chanmask = R.chanmask; k.sample_mask = R.sample_mask; k.poly = R.poly[2] < 3 ? R.poly[2] : 2;
        k.a2c = R.ms[1] != 0; k.dclamp = R.depth_clamp && D.f2.features.depthClamp; k.blend_on = R.blend_on;
        memcpy(k.blend, R.blend, sizeof k.blend);
    }
    pkey_targets(&k);
    Pipe *p = gs ? gs_pipe_get(&k, vs, gs, fs) : pipe_get(&k, vs, fs);
    if (!p) return 0;                              /* shader_wait = skip: the pipeline is being made by a worker, the draw is left out (counted in pipe_get) */
    if (gs) g_gs_draws++;
    if (!R.pipe_bound) R.dyn3_dirty = 1;          /* a new command buffer, or a pipeline of our own with static state was bound */
    if (R.pipe_bound != p) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p); R.pipe_bound = p; }
    if (R.dyn_dirty) { set_dynamic_state(); R.dyn_dirty = 0; }
    if (D.has_eds3 && (R.dyn3_dirty || R.dyn3_samples != k.samples)) { set_dynamic_state3(k.samples); R.dyn3_dirty = 0; }
    if (gs && g_gs_dbg_fs) { vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); R.dyn_dirty = 1; }      /* SMO_VK_GS_DEBUG=3 (gs_pipe_get) */
    vkCmdSetPrimitiveTopology(R.cmd, (VkPrimitiveTopology)topo[prim]);
    vkCmdSetPrimitiveRestartEnable(R.cmd, R.restart && (D.has_plr || prim == 3 || prim == 5 || prim == 6));
    /* 4. vertex input */
    {
        VkVertexInputBindingDescription2EXT bd[17];
        VkVertexInputAttributeDescription2EXT ad[16];
        VkBuffer vb[17];
        VkDeviceSize vo[17];
        uint32_t nb = 0, na = 0, bmap[32], need_const = 0;
        memset(bmap, 0xff, sizeof bmap);
        for (int i = 0; i < 16; i++) {
            uint32_t a = i < R.nattr ? R.vattr[i] : 0;
            int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
            VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
            if (nf && !f) { static char msg[64]; snprintf(msg, sizeof msg, "vertex attribute format 0x%x", nf); nvn_vk_missing(msg); }
            memset(&ad[na], 0, sizeof ad[na]);
            ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
            if (f && stream < 16 && voff[stream] != ~0ull) {
                if (bmap[stream] == 0xffffffffu) {
                    bmap[stream] = nb;
                    memset(&bd[nb], 0, sizeof bd[nb]);
                    bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = R.vstream[stream].stride;
                    bd[nb].inputRate = R.vstream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = R.vstream[stream].divisor ? R.vstream[stream].divisor : 1;
                    vb[nb] = vbufs[stream]; vo[nb] = voff[stream]; nb++;
                }
                ad[na].binding = bmap[stream]; ad[na].format = f; ad[na].offset = a >> 14; na++;
            } else if (vs->dep >> i & 1 || 1) {       /* not supplied: (0, 0, 0, 1), as OpenGL's current attribute value; integer inputs take the integer copy */
                ad[na].binding = 16; ad[na].format = (ints >> i & 1) ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = (ints >> i & 1) ? 16 : 0;
                need_const = 1; na++;
            }
        }
        if (need_const) {
            memset(&bd[nb], 0, sizeof bd[nb]);
            bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
            for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
            vb[nb] = R.zero; vo[nb] = 65536; nb++;
        }
        vkCmdSetVertexInputEXT(R.cmd, nb, bd, na, ad);
        if (nb) vkCmdBindVertexBuffers(R.cmd, 0, nb, vb, vo);
    }
    /* 5. descriptors */
    {
        static VkWriteDescriptorSet w[320];
        static VkDescriptorBufferInfo bi[120];
        static VkDescriptorImageInfo ii[200];
        /* M4: buffers change with every draw: pushed into the command buffer (no set, no pool) when the device can; the
         * textures of a draw repeat: one set per distinct list (layout, views, samplers, image layouts) per command buffer */
        VkDescriptorSet sets[2] = {NULL, NULL};
        int nw = 0, ib = 0, it = 0;
        if (!p->push0 && p->nb[0]) sets[0] = set_alloc(p->dl[0]);
        memset(w, 0, sizeof w[0] * (size_t)(p->nb[0] + p->nb[1]));
        for (int s = 0; s < 3; s++) {
            VStage *v = sv[s];
            if (!v) continue;
            for (unsigned n = 1; n < 34; n++) {
                unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
                if (!bit) continue;
                BufRef *b = &bufs[ib];
                bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
                w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 40u * (unsigned)s + (n < 18 ? n : 20 + n - 18); w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
            }
        }
        if (p->push0) { if (nw) vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, (uint32_t)nw, w); }
        else if (sets[0]) { vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL); vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &sets[0], 0, NULL); }
        if (p->nb[1]) {
            uint64_t key = fnv(0xcbf29ce484222325ULL, &p->dl[1], sizeof p->dl[1]);
            unsigned slot;
            if (p->dl[1] == g_udl[1]) for (int s = 0; s < 3; s++) { VStage *v = sv[s]; if (v) for (int i = 0; i < v->nsmp; i++) key = fnv(key, &v->smp[i].binding, sizeof v->smp[i].binding); }      /* session 9: one layout for every pipeline: the binding numbers tell the lists apart */
            for (int i = 0; i < g_ndtex; i++) { key = fnv(key, &g_dtex[i].view, sizeof g_dtex[i].view); key = fnv(key, &g_dtex[i].smp, sizeof g_dtex[i].smp); key = fnv(key, &g_dtex[i].layout, sizeof g_dtex[i].layout); }
            if (!key) key = 1;
            for (slot = (unsigned)key & 4095; g_tset[slot].gen == g_tset_gen && g_tset[slot].key != key; slot = (slot + 1) & 4095) {}
            /* session 4: a hash hit is confirmed by comparing the whole list (layout, views, samplers, image layouts); a
             * different list with the same hash gets a set of its own and is counted */
            int hit = g_tset[slot].gen == g_tset_gen && g_tset[slot].key == key;
            if (hit && (g_tset[slot].dl != p->dl[1] || g_tset[slot].n != g_ndtex || memcmp(&g_tset_list[g_tset[slot].off], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex))) { hit = 0; R.tset_collisions++; slot = 4096; }
            if (hit) { sets[1] = g_tset[slot].set; R.sets_reused++; }
            else {
                int n1 = 0;
                sets[1] = set_alloc(p->dl[1]);
                for (int s = 0; s < 3; s++) {
                    VStage *v = sv[s];
                    if (!v) continue;
                    for (int i = 0; i < v->nsmp; i++) {
                        ii[it].sampler = g_dtex[it].smp; ii[it].imageView = g_dtex[it].view; ii[it].imageLayout = g_dtex[it].layout;
                        memset(&w[n1], 0, sizeof w[n1]);
                        w[n1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n1].dstSet = sets[1]; w[n1].dstBinding = v->smp[i].binding; w[n1].descriptorCount = 1;
                        w[n1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n1].pImageInfo = &ii[it]; n1++; it++;
                    }
                }
                vkUpdateDescriptorSets(V.device, (uint32_t)n1, w, 0, NULL);
                if (slot < 4096 && g_tset_used < 3000 && g_tset_list_used + g_ndtex <= (int)(sizeof g_tset_list / sizeof g_tset_list[0])) {
                    g_tset[slot].key = key; g_tset[slot].set = sets[1]; g_tset[slot].gen = g_tset_gen; g_tset[slot].dl = p->dl[1]; g_tset[slot].n = g_ndtex; g_tset[slot].off = g_tset_list_used;
                    memcpy(&g_tset_list[g_tset_list_used], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex);
                    g_tset_list_used += g_ndtex; g_tset_used++;
                }
            }
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &sets[1], 0, NULL);
        }
    }
    {   uint32_t pc[4] = {(uint32_t)R.alpha_func, 0, 0, 0};
        memcpy(&pc[1], &R.alpha_ref, 4); memcpy(&pc[2], &R.point_size, 4);
        if (RS_ON) {                               /* track O: mx_fcs = game pixels per image pixel of this pass (fragment stages translated with screen_scale); points shrink with the image */
            float fcs = R.pass_rs ? 1.0f / R.pass_sx : 1.0f, ps = R.pass_rs ? R.point_size * R.pass_sx : R.point_size;
            memcpy(&pc[3], &fcs, 4); memcpy(&pc[2], &ps, 4);
        }
        vkCmdPushConstants(R.cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    }
    return 1;
}

/* The pass formats of a target set as the pipeline key holds them (also used by the look-ahead of nvn_vk_m4.c on targets
 * that are not bound yet). Returns 0 when one of the images does not exist yet. */
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth) {
    int all = 1;
    k->ncolor = (uint8_t)ncol; k->samples = 1;
    for (int i = 0; i < ncol; i++) {
        if (!col[i].tex) continue;
        VTex *vt = col[i].tex->vk;
        int ex, dec;
        if (!vt) { all = 0; continue; }
        VkFormat f = vt->fmt;
        if ((col[i].view.set & 0x84) == 0x84 && col[i].view.format) { VkFormat f2 = tex_vkfmt(col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; }
        k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples;
    }
    if (depth->tex) { VTex *vt = depth->tex->vk; if (!vt) all = 0; else { k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; } }
    return all;
}
static void pkey_targets(PKey *k) { pkey_targets_of(k, R.ncol, R.col, &R.depth); }
#if 0   /* session 3's form */
static void pkey_targets(PKey *k) {
    k->ncolor = (uint8_t)R.ncol; k->samples = 1;
    for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) { VTex *vt = R.col[i].tex->vk; int ex, dec; VkFormat f = vt->fmt; if ((R.col[i].view.set & 0x84) == 0x84 && R.col[i].view.format) { VkFormat f2 = tex_vkfmt(R.col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; } k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples; }
    if (R.depth.tex && R.depth.tex->vk) { VTex *vt = R.depth.tex->vk; k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; }
}
#endif
/* ClearDepthStencil with a stencil mask that is not all bits: Vulkan's clears have no mask, so one triangle over the
 * rectangle replaces the masked bits (no colour, no depth). The pass is active. */
static void stencil_clear_masked(uint32_t mask, uint32_t value, int sx, int sy, int sw, int sh) {
    static VStage *vs;
    PKey k;
    if (!vs) {
        vs = calloc(1, sizeof *vs);
        vs->id = ++g_vstage_ids; vs->ok = 1; vs->stage = -1;
        vs->mod = module_from_glsl(0, "#version 460\nvoid main() { gl_Position = vec4(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0); }\n", "internal stencil clear", &g_spv_unused);
        if (!vs->mod) vkd_fail("the internal stencil-clear vertex shader does not compile");
        vs->next = g_vstage[0]; g_vstage[0] = vs;
    }
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.sample_mask = 0xffffffffu; k.topo_class = 2; k.poly = 2; k.logic = 3;
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, NULL);
    VkViewport vp = {0, 0, (float)R.pass_w, (float)R.pass_h, 0, 1};
    VkRect2D sc = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
    float zero4[4] = {0, 0, 0, 0};
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    if (D.has_eds3) {                              /* session 4: what the key held (no colour written, no blending, fill, every sample) is dynamic state now */
        VkBool32 en[8] = {0};
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8] = {0};
        VkSampleMask all = 0xffffffffu;
        uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
        for (int i = 0; i < 8; i++) { eq[i].srcColorBlendFactor = eq[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; eq[i].dstColorBlendFactor = eq[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; eq[i].colorBlendOp = eq[i].alphaBlendOp = VK_BLEND_OP_ADD; }
        vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en); vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq); vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
        vkCmdSetPolygonModeEXT(R.cmd, VK_POLYGON_MODE_FILL); vkCmdSetAlphaToCoverageEnableEXT(R.cmd, VK_FALSE); vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(k.samples ? k.samples : 1), &all);
        if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, VK_FALSE);
        R.dyn3_dirty = 1;
    }
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc); vkCmdSetLineWidth(R.cmd, 1.0f); vkCmdSetDepthBias(R.cmd, 0, 0, 0); vkCmdSetBlendConstants(R.cmd, zero4);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, mask); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, value);
    vkCmdSetCullMode(R.cmd, VK_CULL_MODE_NONE); vkCmdSetFrontFace(R.cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE); vkCmdSetPrimitiveTopology(R.cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE); vkCmdSetDepthCompareOp(R.cmd, VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(R.cmd, VK_TRUE);
    vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBiasEnable(R.cmd, VK_FALSE); vkCmdSetPrimitiveRestartEnable(R.cmd, VK_FALSE); vkCmdSetRasterizerDiscardEnable(R.cmd, VK_FALSE);
    vkCmdSetVertexInputEXT(R.cmd, 0, NULL, 0, NULL);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    R.pipe_bound = NULL; R.dyn_dirty = 1;
}

/* ---------------------------------------------------------------- copies */
/* CopyBufferToTexture and WriteTexels: texels from memory into a region of one level. The image is not synchronised
 * with the texture's own memory first (the OpenGL path does not either). */
static void write_region(Tex *t, const View *vp, const int *r, const void *data, int64_t row_stride, int64_t image_stride) {
    View v;
    memset(&v, 0, sizeof v);
    if (vp) v = *vp;
    VTex *vt = vt_get2(t, 0);
    if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->decode || vt->expand) { nvn_vk_missing("CopyBufferToTexture / WriteTexels into a depth, ASTC-decoded or 3-component texture"); return; }
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0; else if (t->b.target != 2) layer += r[2];
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    uint64_t off = ring_raw(data, size);
    pass_end(); cmd_begin();
    if (vt->rs) { rs_upload(vt, level, layer, off, (uint32_t)rowlen, (uint32_t)rows, r[0], r[1], r[3], r[4]); t->gpu_written = 1; t->uploaded = 1; return; }      /* track O */
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off; c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    t->gpu_written = 1; t->uploaded = 1;
}
static void copy_buf_tex(const Cmd *m) {
    Tex *t = P(m->a[1]);
    View v;
    int r[6];
    if (!t || t->dead || !m->a[0]) return;
    memcpy(&v, BLOB(m), sizeof v); memcpy(r, BLOB(m) + 24, 24);
    write_region(t, &v, r, P(m->a[0]), R.row_stride, R.image_stride);
}
static void vk_write_texels(void *arg) {
    WriteJob *j = arg;
    if (V.alive && j->tex && !j->tex->dead) write_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->data, j->row_stride, 0);
}
/* Session 5. CopyTextureToBuffer, ReadTexels, the movie maker's present: a region of one level to host memory, as the
 * OpenGL path's glGetTextureSubImage (tight rows, or the given row / image strides). Synchronous: the command buffer is
 * submitted and waited for (these calls are rare: album, replays, screenshots). Returns the bytes written, 0 when it stopped. */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride) {
    VTex *vt = vt_get(t);
    if (vt->decode || vt->expand || vt->samples > 1) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of an ASTC-decoded, 3-component or multisample texture"); return 0; }
    if (vt->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of a combined depth-stencil texture"); return 0; }
    int level = vp && (vp->set & 0x81) == 0x81 ? vp->lvl0 : 0, layer = vp && (vp->set & 0x82) == 0x82 ? vp->lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    uint64_t rowlen = row_stride && vt->bw == 1 ? (uint64_t)(row_stride / vt->bpb) : 0, rows = image_stride && row_stride && vt->bw == 1 ? (uint64_t)(image_stride / row_stride) : 0;
    uint64_t bw = ((uint64_t)(rowlen ? rowlen : (uint64_t)r[3]) + vt->bw - 1) / vt->bw, bh = ((uint64_t)(rows ? rows : (uint64_t)r[4]) + vt->bh - 1) / vt->bh, size = bw * bh * vt->bpb * (uint64_t)depth;
    if (!size) return 0;
    VMem mem;
    VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem);
    VkBufferImageCopy c;
    pass_end(); cmd_begin();
    VkImageLayout was = vt->layout;
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.bufferRowLength = (uint32_t)rowlen; c.bufferImageHeight = (uint32_t)rows;
    c.imageSubresource.aspectMask = vt->aspect; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)(layer + r[2]); c.imageSubresource.layerCount = vt->is3d ? 1 : (uint32_t)depth;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    if (vt->rs) {                                  /* track O: the game reads ITS rectangle: the image's part of it is blitted up to a temporary image of that size */
        int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
        VkImageBlit b;
        if (r[3] <= 0 || r[4] <= 0 || !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            nvn_vk_missing("resolution scale: ReadTexels / CopyTextureToBuffer of a scaled texture whose format cannot be blitted");
            if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
            vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
            return 0;
        }
        VTex *tmp = rs_temp(vt, r[3], r[4]);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = vt->aspect; b.srcSubresource.mipLevel = (uint32_t)level; b.srcSubresource.baseArrayLayer = (uint32_t)(layer + r[2]); b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = vt->aspect; b.dstSubresource.layerCount = 1;
        b.srcOffsets[0].x = rs_edge(r[0], vt->sx, lw); b.srcOffsets[0].y = rs_edge(r[1], vt->sy, lh); b.srcOffsets[1].x = rs_edge(r[0] + r[3], vt->sx, lw); b.srcOffsets[1].y = rs_edge(r[1] + r[4], vt->sy, lh); b.srcOffsets[1].z = 1;
        if (b.srcOffsets[1].x <= b.srcOffsets[0].x) b.srcOffsets[1].x = b.srcOffsets[0].x + 1; if (b.srcOffsets[1].y <= b.srcOffsets[0].y) b.srcOffsets[1].y = b.srcOffsets[0].y + 1;
        if (b.srcOffsets[1].x > lw) { b.srcOffsets[0].x = lw - 1; b.srcOffsets[1].x = lw; } if (b.srcOffsets[1].y > lh) { b.srcOffsets[0].y = lh - 1; b.srcOffsets[1].y = lh; }
        b.dstOffsets[1].x = r[3]; b.dstOffsets[1].y = r[4]; b.dstOffsets[1].z = 1;
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageSubresource.mipLevel = 0; c.imageSubresource.baseArrayLayer = 0; c.imageSubresource.layerCount = 1; c.imageOffset.x = c.imageOffset.y = c.imageOffset.z = 0;
        vkCmdCopyImageToBuffer(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        g_rs_down++;
    } else
    vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
    cmd_wait_all();
    memcpy(out, mem.map, (size_t)size);
    vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
    R.readbacks++;
    return (size_t)size;
}
/* SMO port: nvnTextureInvalidateTexels: what the GPU rendered or copied into the texture becomes readable in its pool
 * memory, in the texture's own layout (the OpenGL path's job_read_texels with to_pool does the same): each layer of the
 * level (the view's first level, else 0) is read back as packed rows and written into the pool memory with
 * tex_swizzle_level (a linear texture: row by row at its stride); the texture's hash is then taken again (no upload).
 * In SMO agl's SMAA lookup textures (agl::TextureTileConverter) call it. Not for a texture the GPU never wrote (its
 * memory is already right), one kept decoded on the CPU side, or a multisample one (said in the log). */
static void vk_invalidate_texels(ReadJob *j) {
    Tex *t = j->tex;
    static int nlog;
    if (!t->mem || !t->gpu_written || t->b.packaged) return;
    const TexFormat *tf = tex_format(t->b.format);
    if ((tf && tf->sw_decode) || t->b.samples > 1) {
        if (nlog++ < 5) printf("nvn_vk: InvalidateTexels on a texture of format 0x%x%s: its pool memory is not rewritten\n", t->b.format, t->b.samples > 1 ? " (multisample)" : " kept decoded");
        return;
    }
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0;
    TexLayout L;
    tex_layout(&L, t->b.target, t->b.format, t->b.width, t->b.height, t->b.depth, t->b.levels, t->b.flags & ~0x50, 0,
               t->b.has_layout && t->b.layout[0] <= 4 ? t->b.layout[0] : -1);
    if (level >= L.levels) return;
    int is3d = t->b.target == 2, layers = is3d ? 1 : L.layers;
    uint8_t *lin = malloc(L.lvl_linear_size[level] ? L.lvl_linear_size[level] : 1);
    if (!lin) return;
    cb_sync();
    for (int layer = 0; layer < layers; layer++) {
        int r[6] = {0, 0, layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1};
        View v;
        memset(&v, 0, sizeof v);
        v.set = 0x81; v.lvl0 = level; v.nlvl = 1;
        if (!read_region(t, &v, r, lin, 0, 0)) break;
        if (t->b.flags & 0x10) {                                       /* linear: rows at the texture's stride */
            size_t row = L.lvl_linear_size[level] / (size_t)(L.lvl_h[level] > 0 ? L.lvl_h[level] : 1);
            int64_t stride = t->b.stride ? t->b.stride : (int64_t)row;
            if (t->mem + stride * L.lvl_h[level] > t->limit) break;
            for (int y = 0; y < L.lvl_h[level]; y++) memcpy(t->mem + stride * y, lin + row * (size_t)y, row);
        } else {
            uint8_t *dst = (uint8_t *)(uintptr_t)tex_level_ptr(&L, t->mem, level, layer);
            if (t->limit && dst + L.lvl_size[level] > t->limit) break;
            tex_swizzle_level(&L, level, lin, dst);
        }
    }
    free(lin);
    nvn_front_tex_rehash(t);
}
static void vk_read_texels(void *arg) {
    ReadJob *j = arg;
    if (!V.alive || !j->tex || j->tex->dead) return;
    if (j->to_pool) { vk_invalidate_texels(j); return; }
    cb_sync();
    read_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->out, j->row_stride, j->image_stride);
}
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);      /* nnalbum.c, as nvn_gl.c job_present_movie */
static void vk_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!V.alive || !t || t->dead || t->b.width <= 0 || t->b.height <= 0) return;
    VTex *vt = vt_get(t);
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, r[6] = {0, 0, 0, w, h, 1}, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("nvnQueuePresentTexture to the movie maker's window with a window texture that is not 8-bit RGBA"); return; }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    if (read_region(t, NULL, r, px, 0, 0)) {
        if (bgr) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
        album_movie_push_frame(px, w, h, w * 4, 0);
    }
    free(px);
}
static LONG64 g_blit_same;                         /* SMO: copies within one texture (blit_tex) */
static void blit_tex(Tex *s, const View *sv, const int *sr, Tex *d, const View *dv, const int *dr, int linear) {
    VTex *vs = vt_get(s), *vd = vt_get(d);
    VkImageBlit b;
    d->gpu_written = 1;
    /* SMO: a copy within one texture (another level or layer of it, e.g. a mip chain made level by level) goes through the
     * GENERAL layout, with a barrier so that it reads what the copy before it wrote; the same level and layer is refused */
    int same = vs == vd;
    VkImageLayout sl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pass_end(); cmd_begin();
    if (same) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        vt_layout(vs, VK_IMAGE_LAYOUT_GENERAL);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        g_blit_same++;
    } else { vt_layout(vs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); }
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = vs->aspect; b.srcSubresource.mipLevel = sv && (sv->set & 0x81) == 0x81 ? (uint32_t)sv->lvl0 : 0; b.srcSubresource.baseArrayLayer = sv && (sv->set & 0x82) == 0x82 && !vs->is3d ? (uint32_t)sv->lay0 : 0; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = vd->aspect; b.dstSubresource.mipLevel = dv && (dv->set & 0x81) == 0x81 ? (uint32_t)dv->lvl0 : 0; b.dstSubresource.baseArrayLayer = dv && (dv->set & 0x82) == 0x82 && !vd->is3d ? (uint32_t)dv->lay0 : 0; b.dstSubresource.layerCount = 1;
    b.srcOffsets[0].x = sr[0]; b.srcOffsets[0].y = sr[1]; b.srcOffsets[1].x = sr[0] + sr[3]; b.srcOffsets[1].y = sr[1] + sr[4]; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = dr[0]; b.dstOffsets[0].y = dr[1]; b.dstOffsets[1].x = dr[0] + dr[3]; b.dstOffsets[1].y = dr[1] + dr[4]; b.dstOffsets[1].z = 1;
    /* SMO port: a 3D side copies the z range of its region (z, depth), starting at the view's first layer (a 3D texture's
     * view layers are its slices). Before, every 3D copy took z 0..1: SMO's 8x8x8 colour-grading LUT got one slice and
     * every 3D frame came out blue (boot86, nvn_vk_lut). Vulkan filters a 3D blit in all three axes, as the scaled copy needs. */
    if (vs->is3d) { int z0 = (sv && (sv->set & 0x82) == 0x82 ? sv->lay0 : 0) + sr[2]; b.srcOffsets[0].z = z0; b.srcOffsets[1].z = z0 + (sr[5] > 0 ? sr[5] : 1); }
    if (vd->is3d) { int z0 = (dv && (dv->set & 0x82) == 0x82 ? dv->lay0 : 0) + dr[2]; b.dstOffsets[0].z = z0; b.dstOffsets[1].z = z0 + (dr[5] > 0 ? dr[5] : 1); }
    if (vs->is3d != vd->is3d && (b.srcOffsets[1].z - b.srcOffsets[0].z != 1 || b.dstOffsets[1].z - b.dstOffsets[0].z != 1)) { nvn_vk_missing("CopyTextureToTexture of several slices between a 3D and a 2D texture"); return; }
    if (same && b.srcSubresource.mipLevel == b.dstSubresource.mipLevel && (vs->is3d || b.srcSubresource.baseArrayLayer == b.dstSubresource.baseArrayLayer)) { nvn_vk_missing("CopyTextureToTexture within one level and layer of a texture"); return; }
    int rs_sw = sr[3], rs_sh = sr[4], rs_dw = dr[3], rs_dh = dr[4];
    if (vs->rs || vd->rs) {                        /* track O: the game's rectangles on the scaled images */
        if (vs->rs) {
            int lw = lvl_(vs->w, (int)b.srcSubresource.mipLevel), lh = lvl_(vs->h, (int)b.srcSubresource.mipLevel);
            b.srcOffsets[0].x = rs_edge(sr[0], vs->sx, lw); b.srcOffsets[0].y = rs_edge(sr[1], vs->sy, lh); b.srcOffsets[1].x = rs_edge(sr[0] + sr[3], vs->sx, lw); b.srcOffsets[1].y = rs_edge(sr[1] + sr[4], vs->sy, lh);
            rs_sw = b.srcOffsets[1].x - b.srcOffsets[0].x; rs_sh = b.srcOffsets[1].y - b.srcOffsets[0].y;
        }
        if (vd->rs) {
            int lw = lvl_(vd->w, (int)b.dstSubresource.mipLevel), lh = lvl_(vd->h, (int)b.dstSubresource.mipLevel);
            b.dstOffsets[0].x = rs_edge(dr[0], vd->sx, lw); b.dstOffsets[0].y = rs_edge(dr[1], vd->sy, lh); b.dstOffsets[1].x = rs_edge(dr[0] + dr[3], vd->sx, lw); b.dstOffsets[1].y = rs_edge(dr[1] + dr[4], vd->sy, lh);
            rs_dw = b.dstOffsets[1].x - b.dstOffsets[0].x; rs_dh = b.dstOffsets[1].y - b.dstOffsets[0].y;
        }
        if (!rs_sw || !rs_sh || !rs_dw || !rs_dh) return;
        g_rs_blits++;
    }
    if (vs->aspect != vd->aspect) { nvn_vk_missing("CopyTextureToTexture between a colour and a depth format"); return; }
    if (vs->samples > 1 && vd->samples == 1 && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT && rs_sw == rs_dw && rs_sh == rs_dh) {
        VkImageResolve rv;
        memset(&rv, 0, sizeof rv);
        rv.srcSubresource = b.srcSubresource; rv.dstSubresource = b.dstSubresource; rv.srcOffset = b.srcOffsets[0]; rv.dstOffset = b.dstOffsets[0];
        rv.extent.width = (uint32_t)rs_sw; rv.extent.height = (uint32_t)rs_sh; rv.extent.depth = 1;
        vkCmdResolveImage(R.cmd, vs->img, sl, vd->img, dl, 1, &rv);
        return;
    }
    if (vs->samples > 1 || vd->samples > 1) { nvn_vk_missing("a scaled or depth copy of a multisample texture"); return; }
    /* glBlitFramebuffer with FRAMEBUFFER_SRGB off copies the stored values; vkCmdBlitImage converts between an sRGB and a
     * linear format. Same class on both sides = the same values. */
    vkCmdBlitImage(R.cmd, vs->img, sl, vd->img, dl, 1, &b, linear && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---------------------------------------------------------------- replay */
static void replay(uint64_t handle, int depth);
/* The two kinds of draw (nvnCommandBufferDrawArrays* / DrawElements*, and each draw of an indirect one). */
static LONG64 g_indirect_draws, g_indirect_disp;
static void vk_draw_arrays(unsigned prim, uint32_t first, uint32_t count, uint32_t base_inst, uint32_t inst) {
    int gsr = R.prog[2] && g_gs_draws % 20 == 0 && g_gs_draws <= 400 && (gs_dbg_mode() == 1 || gs_dbg_mode() == 3);      /* SMO_VK_GS_DEBUG: gs_dbg_read */
    if (gsr) gs_dbg_read(0);
    vtx_switches();
    if (count) vtx_need((uint64_t)first + count - 1, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 0); else memset(g_vneed, 0, sizeof g_vneed);
    { LONG64 t0 = tk(); int ok = draw_prepare(prim); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); return; } }
    occ_draw();
    if (R.prog[2]) gsq_begin();
    vkCmdDraw(R.cmd, count, inst, first, base_inst);
    gsq_end();
    if (gsr) gs_dbg_read(1);
    memset(g_vneed, 0, sizeof g_vneed);
    R.frame_draws++; nvn_front_count(0, 1);
}
static void vk_draw_elements(unsigned prim, unsigned type, int count, uint64_t indices, int32_t base_vertex, uint32_t base_inst, uint32_t inst) {
    unsigned t = type < 3 ? type : 1;
    if (count <= 0 || !indices) { R.frame_skipped++; nvn_front_count(1, 1); return; }
    if (t == 0 && !D.has_iu8) { nvn_vk_missing("8-bit indices on a GPU without VK_EXT_index_type_uint8"); return; }
    BufRef ib;
    if (g_cb_live && cb_overlap(indices, (uint64_t)count << t)) cb_sync();      /* index_max reads the indices from guest memory: compute results there first */
    buf_ref(indices, (uint64_t)count << t, (uint64_t)1 << t, &ib);    /* before the draw is prepared: a ring wrap submits */
    vtx_switches();
    if (g_wlog_n && wlog_overlap(0, indices, indices + ((uint64_t)count << t))) GEN_BUMP(GEN_INDEXW);      /* index_max's cache knows generations only */
    {   int64_t mv = (int64_t)index_max(indices, count, t) + base_vertex;
        vtx_need(mv > 0 ? (uint64_t)mv : 0, inst ? (uint64_t)base_inst + inst - 1 : (uint64_t)base_inst, 1); }
    { LONG64 t0 = tk(); int ok = draw_prepare(prim); g_tm[8] += tk() - t0; if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); return; } }
    vkCmdBindIndexBuffer(R.cmd, ib.buf, ib.off, t == 0 ? VK_INDEX_TYPE_UINT8_EXT : t == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    occ_draw();
    vkCmdDrawIndexed(R.cmd, (uint32_t)count, inst, 0, base_vertex, base_inst);
    memset(g_vneed, 0, sizeof g_vneed);
    R.frame_draws++; nvn_front_count(0, 1);
}
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    switch (m->op) {
    case OP_NOP: case OP_DEBUG: case OP_DISCARD: case OP_CONSDILATE: case OP_SUBPIXEL: break;
    case OP_TEXPOOL: R.tpool = P(a[0]); break;
    case OP_SMPPOOL: R.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); R.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? R.ubo[stage] : R.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            unsigned u = (unsigned)(first + i);
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            Tex *t;
            View v;
            SamplerParams sp;
            memcpy(&h, blob + i * 8, 8);
            R.nvn_tex[stage][u] = h;
            if (nvn_front_handle(R.tpool, R.spool, h, &t, &v, &sp) & 1) {      /* what TXQ reports, and whether the shader must use an integer sampler (resolve_handle's rules) */
                int glevels = nvn_front_tex_levels(t), l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : glevels - l0, is3d = t->b.target == 2;
                int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : nvn_front_tex_layers(t), nf = (v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
                dims[0] = (uint32_t)lvl_(t->b.width, l0); dims[1] = (uint32_t)lvl_(t->b.height, l0);
                dims[2] = is3d ? (uint32_t)lvl_(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
                dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
                if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) isint = k == 3 || k == 8 || k == 11 ? 2 : 1;
                if (nf == 0x3E) isint = 1;
            }
            if (memcmp(&R.c2[stage][48 + 2 * u], dims, 16)) { memcpy(&R.c2[stage][48 + 2 * u], dims, 16); g_c2r[stage].valid = 0; }
            R.tex_int[stage] = (R.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            R.tex_sint[stage] = (R.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        break;
    }
    case OP_VBUF: for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&R.vbuf[a[0] + i], blob + i * 16, 16); break;
    case OP_VATTR: R.nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(R.vattr, blob, (size_t)R.nattr * 4); break;
    case OP_VSTREAM: for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&R.vstream[i], blob + i * 8, 8); break;
    case OP_BLEND: { const uint8_t *b = (const uint8_t *)&a[0]; if (b[7]) nvn_vk_missing("an advanced blend equation (nvnBlendStateSetAdvancedMode)"); memcpy(R.blend[b[0] & 7], b + 1, 6); R.dyn3_dirty = 1; break; }
    case OP_COLOR: { const uint8_t *b = (const uint8_t *)&a[0]; R.blend_on = b[0]; R.logic = b[1]; R.alpha_func = b[2]; R.dyn3_dirty = 1; if (b[1] != 3 && !D.f2.features.logicOp) nvn_vk_missing("a logic op on a GPU without logicOp"); break; }
    case OP_CHANMASK: R.chanmask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_MULTISAMPLE: { const uint8_t *b = (const uint8_t *)&a[0]; memcpy(R.ms, b, 24); R.dyn3_dirty = 1; if (b[3] || b[5]) nvn_vk_missing("a multisample state with coverage-to-colour, coverage modulation, sample locations or raster samples"); break; }
    case OP_POLYGON: memcpy(R.poly, &a[0], 4); R.dyn_dirty = 1; R.dyn3_dirty = 1; break;
    case OP_DEPTHSTENCIL: R.ds = a[0]; R.dyn_dirty = 1; break;
    case OP_VIEWPORTS: if (a[0] == 0 && a[1] > 0) { memcpy(R.viewport, blob, 16); R.have_viewport = 1; R.dyn_dirty = 1; } break;
    case OP_SCISSORS: if (a[0] == 0 && a[1] > 0) { memcpy(R.scissor, blob, 16); if (R.scissor[2] < 0) R.scissor[2] = 0; if (R.scissor[3] < 0) R.scissor[3] = 0; R.have_scissor = 1; R.dyn_dirty = 1; } break;
    case OP_DEPTHRANGES: if (a[0] == 0 && a[1] > 0) { memcpy(R.drange, blob, 8); R.dyn_dirty = 1; } break;
    case OP_TARGETS: {
        const TargetRef *col = (const TargetRef *)blob, *dep = col + 8;
        int n = (int)a[0] < 8 ? (int)a[0] : 8;
        pass_end();
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) ((VTex *)R.col[i].tex->vk)->feedback = 0;
        if (R.depth.tex && R.depth.tex->vk) ((VTex *)R.depth.tex->vk)->feedback = 0;
        memset(R.col, 0, sizeof R.col); memset(&R.depth, 0, sizeof R.depth);
        R.ncol = 0; R.have_targets = 0;
        for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { R.col[i] = col[i]; R.ncol = i + 1; R.have_targets = 1; }
        if (dep->tex && !dep->tex->dead) { R.depth = *dep; R.have_targets = 1; }
        R.dyn_dirty = 1;                           /* default viewport / scissor follow the target size */
        break;
    }
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? R.sref : m->op == OP_STENCIL_VMASK ? R.svmask : R.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        R.dyn_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: memcpy(R.blendc, blob, 16); R.dyn_dirty = 1; break;
    case OP_ALPHAREF: memcpy(&R.alpha_ref, &a[0], 4); break;
    case OP_POINTSIZE: memcpy(&R.point_size, &a[0], 4); break;
    case OP_LINEWIDTH: memcpy(&R.line_width, &a[0], 4); R.dyn_dirty = 1; break;
    case OP_POLYOFFSET: memcpy(R.polyoff, &a[0], 12); R.dyn_dirty = 1; break;
    case OP_SAMPLEMASK: R.sample_mask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_RASTDISCARD: R.rast_discard = a[0] & 1; R.dyn_dirty = 1; break;
    case OP_DEPTHCLAMP: R.depth_clamp = a[0] & 1; R.dyn3_dirty = 1; if ((a[0] & 1) && !D.f2.features.depthClamp) nvn_vk_missing("depth clamp on a GPU without depthClamp"); break;
    case OP_PRIMRESTART: R.restart = a[0] & 1; break;
    case OP_PATCHSIZE: break;                      /* only read by tessellation, which stops at the draw */
    case OP_IMAGES: { int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2]; for (int i = 0; i < count && first + i < 8; i++) memcpy(&R.nvn_img[stage][first + i], blob + i * 8, 8); break; }
    case OP_FENCE: if (a[0]) { SyncObj *o = P(a[0]); if (!o->dead) o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); } break;
    case OP_WAITSYNC: break;                       /* one queue: what was submitted before has run before */
    /* SMO port: reports, as nvn_gl.c's replay writes them (NVNcounterData {counter, timestamp in ns}), after the work
     * recorded before them has run (cb_sync). Counter type 0 carries only the timestamp; types measured by the GPU
     * (samples passed, primitives, shader invocations: Vulkan queries) are not implemented yet and stop with the type. */
    case OP_REPORT_VALUE: if (a[1]) { cb_sync(); uint64_t d[2] = {(uint32_t)a[0], vk_now_ns()}; memcpy(P(a[1]), d, 16); host_wrote(a[1], 16, GEN_REPORT); } break;
    /* NVN counter types: 0 TIMESTAMP (bottom of pipe: after earlier work), 16 TIMESTAMP_TOP (top of pipe: when reached,
     * no wait); 1..15 are measured by the GPU (samples passed ... ZCULL stats), as nvn_gl.c's counter_target lists */
    case OP_COUNTER_RESET: if (a[0] == 1) { occ_reset(); break; } if (a[0] != 0 && a[0] != 16) { static char msg[80]; snprintf(msg, sizeof msg, "nvnCommandBufferResetCounter of counter type %u (a GPU query)", (unsigned)a[0]); nvn_vk_missing(msg); } break;
    case OP_COUNTER_REPORT:
        if (a[0] == 1) { occ_report(a[1]); break; }
        if (a[0] >= 2 && a[0] <= 15) {             /* SMO: a pipeline statistics counter (vertices, invocations, primitives, zcull; profiling): reported as 0 with the time */
            static uint32_t said;
            if (!(said >> a[0] & 1)) { said |= 1u << a[0]; printf("nvn_vk: nvnCommandBufferReportCounter of counter type %u (pipeline statistics): reported as 0 (not counted)\n", (unsigned)a[0]); }
            if (a[1]) { uint64_t d[2] = {0, vk_now_ns()}; memcpy(P(a[1]), d, 16); host_wrote(a[1], 16, GEN_REPORT); }
            break;
        }
        if (a[0] != 0 && a[0] != 16) { static char msg[80]; snprintf(msg, sizeof msg, "nvnCommandBufferReportCounter of counter type %u (a GPU query)", (unsigned)a[0]); nvn_vk_missing(msg); break; }
        if (a[1]) { if (a[0] == 0) cb_sync(); uint64_t d[2] = {0, vk_now_ns()}; memcpy(P(a[1]), d, 16); host_wrote(a[1], 16, GEN_REPORT); }
        break;
    case OP_CONSRASTER: if (a[0] & 1) nvn_vk_missing("conservative rasterisation (nvnCommandBufferSetConservativeRasterEnable)"); break;
    case OP_DEPTHBOUNDS: if (a[0] & 1) nvn_vk_missing("the depth bounds test (nvnCommandBufferSetDepthBounds)"); break;
    case OP_RENDER_ENABLE:                         /* a1 = 0: a0 is the switch; otherwise mode + 1 (0 render if equal, 1 if not) and a2 the address of
                                                    * two counter reports (16 bytes each). SMO port: compared on the CPU as nvn_gl.c does, once the
                                                    * reports queued for those addresses are in guest memory (waited for here when they are not) */
        if (!a[1]) R.render_off = !(a[0] & 1);
        else {
            int need = 0;
            for (int i = 0; i < g_occ_qn; i++) { const OccEntry *e = &g_occ_q[(g_occ_qh + i) % OCC_Q]; if (e->kind == 0 && e->dst < a[2] + 32 && e->dst + 16 > a[2]) need = i + 1; }
            g_occ_cond++;
            if (need) {
                static int logon = -1, nlog;
                if (logon < 0) logon = getenv("SMO_VK_OCC_LOG") && atoi(getenv("SMO_VK_OCC_LOG"));
                if (logon && nlog++ < 200) {
                    const OccEntry *e = &g_occ_q[(g_occ_qh + need - 1) % OCC_Q];
                    printf("nvn_vk_occ: frame %lld: conditional render on %llx waits for a report queued in frame %lld (entry %d of %d queued, segments %llu..%llu, %s)\n",
                           nvn_front_frame_next(), (unsigned long long)a[2], (long long)e->frame, need, g_occ_qn, (unsigned long long)e->lo, (unsigned long long)e->hi,
                           e->serial > R.serial ? "in the command buffer being recorded" : "submitted");
                }
                g_occ_cond_waits++;
                occ_resolve(need);
            }
            cb_sync(); int eq = a[2] && rd64(a[2]) == rd64(a[2] + 16); R.render_off = a[1] == 1 ? !eq : eq;
            g_fr_cond++; g_fr_condoff += R.render_off; g_fr_condhash = (g_fr_condhash ^ (a[2] * 2 + (uint64_t)R.render_off)) * 1099511628211ull;
        }
        break;
    case OP_ROWSTRIDE: R.row_stride = (int64_t)a[0]; break;
    case OP_IMAGESTRIDE: R.image_stride = (int64_t)a[0]; break;
    case OP_BARRIER:                               /* NVNbarrierBits: 2 / 4 order fragments, 8 indirect data, 0x10 texture memory changed */
        if (a[0] & 0x10) nvn_front_barrier_texmem();
        if (a[0] & 6) tex_barrier();
        break;
    case OP_DISPATCH: { LONG64 t0 = tk(); compute_dispatch((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); g_tm[2] += tk() - t0; break; }
    case OP_COPY_BUF_BUF: cb_sync(); if (a[0] && a[1]) { memmove(P(a[1]), P(a[0]), (size_t)a[2]); host_wrote(a[1], a[2], GEN_BUF); } break;      /* guest memory is read / written: what compute wrote must be there (as OpenGL's compute_flush) */
    case OP_UPDATE_UBO:                            /* only compute results in the range written must be in guest memory first (it used to be all of them: a wait for the GPU at the first update after a dispatch) */
        if (g_cb_pending || (g_cb_live && cb_overlap(a[0] + a[2], a[3]))) cb_sync();
        if (a[0]) { memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); host_wrote(a[0] + a[2], a[3], GEN_UBO); }
        break;
    case OP_CLEAR_BUF: cb_sync(); if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); host_wrote(a[0], a[1], GEN_BUF); } break;
    case OP_CLEARCOLOR: {
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2] & 15;
        if (!R.have_targets || R.render_off || (int)i >= R.ncol || !R.col[i].tex) break;
        if (!pass_begin()) break;
        if (mask != 15) {                          /* a partial channel mask only matters for channels the format has */
            Fmt f; int comps = 4, nf = R.col[i].tex->b.format;
            if (nf >= 1 && nf <= 0x30) comps = (nf - 1) / 12 + 1;
            if (!nvn_front_fmt(nf, &f) || (mask & ((1u << comps) - 1)) != (1u << comps) - 1) { if (mask) nvn_vk_missing("ClearColor with a partial channel mask"); break; }
        }
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = i; memcpy(&ca.clearValue.color, blob, 16);
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_CLEARDS: {
        if (!R.have_targets || R.render_off || !R.depth.tex || !((a[0] & 1) || (uint32_t)a[2])) break;
        if (!pass_begin()) break;
        VTex *vt = R.depth.tex->vk;
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0 || !vt) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        {   static int want = -2; want = passshot_frame() ? 1 : -1;
            if (want >= 0) { float dv; memcpy(&dv, &a[3], 4); printf("nvn_vk_ds: clear depth %d (value %g) stencil mask %x value %u on depth tex %p fmt 0x%x %dx%d\n", (int)(a[0] & 1), dv, (unsigned)a[2], (unsigned)a[1], (void *)R.depth.tex, R.depth.tex->b.format, R.depth.tex->b.width, R.depth.tex->b.height); } }
        ca.aspectMask = ((a[0] & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((uint32_t)a[2] ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if ((ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && ((uint32_t)a[2] & 0xff) != 0xff) {
            stencil_clear_masked((uint32_t)a[2] & 0xff, (uint32_t)a[1], sx, sy, sw, sh);
            ca.aspectMask &= ~(VkFlags)VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        ca.aspectMask &= vt->aspect;
        if (!ca.aspectMask) break;
        memcpy(&ca.clearValue.depthStencil.depth, &a[3], 4); ca.clearValue.depthStencil.stencil = (uint32_t)a[1];
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_DRAW_ARRAYS: vk_draw_arrays((unsigned)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]); break;
    case OP_DRAW_ELEMENTS: vk_draw_elements((unsigned)a[0], (unsigned)a[1], (int)a[2], a[3], (int32_t)a[4], (uint32_t)a[5], (uint32_t)a[6]); break;
    case OP_DRAW_INDIRECT: {                       /* SMO: a0 mode, a1 index type, a2 index buffer, a3 indirect data, a4 draws (or the maximum), a5 stride, a6 bit 0 elements, bit 1 the count read from memory (blob: its address) */
        /* as the OpenGL path: the parameters are read from guest memory (a dispatch that wrote them is copied back first) and each
         * becomes an ordinary draw; the draw path needs the counts on this side anyway (the vertices a draw reads, vtx_need) */
        unsigned n = (unsigned)a[4], stride = (unsigned)a[5];
        int elements = (int)(a[6] & 1);
        if (!stride) stride = elements ? 20 : 16;
        if (a[6] & 2) { uint64_t param; memcpy(&param, blob, 8); if (param && g_cb_live && cb_overlap(param, 4)) cb_sync(); uint32_t cnt = 0; if (param) memcpy(&cnt, P(param), 4); if (cnt < n) n = cnt; }
        if (!a[3] || !n) break;
        if (g_cb_live && cb_overlap(a[3], (uint64_t)n * stride)) cb_sync();
        for (unsigned k = 0; k < n; k++) {
            uint32_t d[5];                         /* arrays: count, instances, first, base instance; elements: count, instances, first index, base vertex, base instance */
            unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
            memcpy(d, P(a[3] + (uint64_t)k * stride), elements ? 20 : 16);
            if (!d[0] || !d[1]) continue;
            if (elements) vk_draw_elements((unsigned)a[0], t, (int)d[0], a[2] ? a[2] + ((uint64_t)d[2] << t) : 0, (int32_t)d[3], d[4], d[1]);
            else vk_draw_arrays((unsigned)a[0], d[2], d[0], d[3], d[1]);
            g_indirect_draws++;
        }
        break;
    }
    case OP_DISPATCH_INDIRECT: {                   /* SMO: a0 the address of three group counts (read as the OpenGL path does) */
        uint32_t g[3] = {0, 0, 0};
        if (a[0]) { if (g_cb_live && cb_overlap(a[0], 12)) cb_sync(); memcpy(g, P(a[0]), 12); }
        LONG64 t0 = tk(); compute_dispatch(g[0], g[1], g[2]); g_tm[2] += tk() - t0; g_indirect_disp++;
        break;
    }
    case OP_COPY_BUF_TEX: cb_sync(); copy_buf_tex(m); break;
    case OP_COPY_TEX_BUF: {                        /* a0 texture, a3 buffer address; blob: view, region (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        if (!t || t->dead || !a[3]) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        cb_sync();
        read_region(t, &v, r, P(a[3]), R.row_stride, R.image_stride);
        GEN_BUMP(GEN_TEXBUF);                      /* guest memory changed: ranges are streamed again */
        break;
    }
    case OP_COPY_TEX_TEX: {
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(sr, blob + 24, 24); memcpy(&dv, blob + 48, sizeof dv); memcpy(dr, blob + 72, 24);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1));
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_CLEARTEX: {                            /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        Fmt f;
        if (!t || t->dead) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        int whole = a[6] || (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] >= lvl_(t->b.width, level) && r[4] >= lvl_(t->b.height, level));
        pass_end();
        VTex *vt = vt_get(t);
        int comps = t->b.format >= 1 && t->b.format <= 0x30 ? (t->b.format - 1) / 12 + 1 : 4;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !nvn_front_fmt(t->b.format, &f) || f.kind == 4 || vt->decode || vt->expand) { nvn_vk_missing("nvnCommandBufferClearTexture of a depth, compressed or 3-component texture"); break; }
        if (!whole || ((unsigned)a[4] & ((1u << comps) - 1)) != (1u << comps) - 1) { nvn_vk_missing("nvnCommandBufferClearTexture of a part of a level or with a partial channel mask"); break; }
        cmd_begin();
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, (uint32_t)(vt->is3d ? 0 : layer), 1};
        VkClearColorValue cv;
        memcpy(&cv, blob + 48, 16);
        vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
        t->gpu_written = 1; t->uploaded = 1;
        break;
    }
    case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); } break;
    default: { static char msg[OP_COUNT][64]; snprintf(msg[m->op % OP_COUNT], 64, "nvnCommandBuffer%s", nvn_front_op_name(m->op)); nvn_vk_missing(msg[m->op % OP_COUNT]); break; }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) { static int n; const char *nvn_front_list_why(uint64_t h); void nvn_front_list_stale(void); nvn_front_list_stale(); if (n++ < 10) printf("nvn_vk: command handle %llx is unknown or expired: %s\n", (unsigned long long)handle, nvn_front_list_why(handle)); return;}
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- the RBackend members */
static void prewarm_wait(void);
static void vk_submit(void *arg) {
    SubmitJob *j = arg;
    if (!V.alive) return;                          /* after the shutdown at exit: the device is gone */
    prewarm_wait();
    LONG64 t0 = tk();
    cb_sync();                                     /* what the last batch's dispatches wrote is in guest memory before this batch reads it */
    if (g_occ_qn) occ_resolve(0);                  /* occlusion counts whose submissions have finished */
    LONG64 t1 = tk();
    nvn_front_batch_begin();
    GEN_BUMP(GEN_BATCH);                           /* guest memory may have changed since the last batch */
    g_batch_no++;
    if (g_scan_on) { LONG64 ts = tk(); Scan s; scan_begin(&s); for (int i = 0; i < j->count; i++) scan_list(j->handles[i], 0, &s); g_tm[9] += tk() - ts; }      /* session 4: what this batch will need goes to the workers before it is replayed */
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    cb_batch_end();
    occ_fold();
    g_tm[7] += t1 - t0; g_tm[3] += tk() - t1;
    nvn_front_count(3, 1);
}
static void vk_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); vk_submit(j); }
static void vk_idle(void) { if (V.alive) { cb_poll(); if (g_occ_qn) occ_resolve(0); nvn_front_take_dead(vk_drop); } }
static void vk_housekeeping(void *arg) { (void)arg; if (V.alive) nvn_front_take_dead(vk_drop); }
static void vk_flush(void *arg) { (void)arg; if (V.alive) cmd_submit(NULL, NULL); }
static void vk_finish(void *arg) { (void)arg; if (V.alive) { cmd_wait_all(); if (g_occ_qn) occ_resolve(0); } }
static void vk_fence(void *arg) { SyncJob *j = arg; if (j->o->dead || !V.alive) return; j->o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); }
static void vk_sync_wait_(SyncJob *j) {
    uint64_t want = (uint64_t)(uintptr_t)j->o->s;
    if (!want || !V.alive) { j->result = 3; return; }       /* never fenced */
    want--;
    if (want <= R.done) { j->result = 0; return; }
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending || s->serial != want) continue;
        VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, j->timeout);
        if (r == VK_SUCCESS) { if (s->serial > R.done) R.done = s->serial; j->result = 1; cb_poll(); } else j->result = r == VK_TIMEOUT ? 2 : 3;
        return;
    }
    j->result = 0;                                  /* its command buffer was reused: finished long ago */
}
static void vk_sync_wait(void *arg) { vk_sync_wait_(arg); if (V.alive && g_occ_qn) occ_resolve(0); }      /* occlusion counts the fence covers are in guest memory when the game goes on */
static void vk_server_wait(void *arg) { SyncJob *j = arg; j->timeout = ~0ull; if (j->o->s) vk_sync_wait(j); j->result = j->o->s != NULL; }
static void vk_sync_drop(void *arg) { SyncObj *o = arg; o->s = NULL; }
static void vk_reset_status(void *arg) { *(int *)arg = 0; }       /* NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR: a lost device ends the process with its own message */
static void vk_no_grid(void *arg) { nvn_vk_missing("nvnMultisampleStateGetSampleLocationsGrid"); ((int *)arg)[0] = ((int *)arg)[1] = 1; }

/* The window texture as a 24-bit BMP, the file save_bmp of the OpenGL path writes (row 0 of the texture is the top row). */
static void save_bmp_vk(const char *path, VTex *vt) {
    VMem mem;
    int w = vt->w, h = vt->h, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (vt->rs && getenv("SMO_VK_SHOT_NATIVE") && atoi(getenv("SMO_VK_SHOT_NATIVE"))) {      /* track O: the scaled image itself, texel for texel (the default below is the game's size, filtered up as the window shows it) */
        VMem m2;
        uint64_t size = (uint64_t)w * h * 4;
        VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m2);
        VkBufferImageCopy c;
        if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2); return; }
        pass_end(); cmd_begin();
        VkImageLayout was = vt->layout;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
        cmd_wait_all();
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t head[54] = {'B', 'M'};
            uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, fsize = 54 + row * (uint32_t)h, v;
            uint8_t *rowb = calloc(1, row);
            memcpy(head + 2, &fsize, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
            fwrite(head, 1, 54, f);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *s = (const uint8_t *)m2.map + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
                fwrite(rowb, 1, row, f);
            }
            free(rowb); fclose(f);
            printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
        }
        vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2);
        return;
    }
    if (vt->rs) { w = vt->t->b.width; h = vt->t->b.height; }      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk: screenshot: the window texture's format %d is not an 8-bit RGBA one; not saved\n", (int)vt->fmt); return; }
    /* session 5: through read_region, the path of ReadTexels / CopyTextureToBuffer (so every screenshot exercises it) */
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    (void)mem;
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
    uint8_t head[54] = {'B', 'M'}, *rowb;
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    FILE *f = fopen(path, "wb");
    if (f) {
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        fwrite(head, 1, 54, f);
        rowb = calloc(1, row);
        for (int y = h - 1; y >= 0; y--) {
            const uint8_t *s = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
            fwrite(rowb, 1, row, f);
        }
        free(rowb); fclose(f);
        printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);      /* the front end's line, word for word: tests/ref/port_capture.py and the shot tools look for it */
    }
    free(px);
}
/* SMO port, debug aid SMO_VK_PASSSHOT=<frame> (or a range, passshot_frame): at the end of every render pass of that frame,
 * colour target 0 is read back (read_region, the ReadTexels path) and saved as pass_[<frame>_]<n>_<format>.bmp in SMO_SHOT_DIR, with one line of figures
 * (pixels not black, mean of each channel). RGB10A2 (0x3d) is decoded to 8 bits per channel; 8-bit RGBA as is. */
static void pass_shot(void) {
    static int want = -2, n, busy;
    want = passshot_frame() ? 1 : -1;
    static int logonly = -1;                       /* SMO_VK_PASSSHOT_LOGONLY=1: the draws are logged, no pictures saved */
    if (logonly < 0) logonly = getenv("SMO_VK_PASSSHOT_LOGONLY") && atoi(getenv("SMO_VK_PASSSHOT_LOGONLY"));
    if (want < 0 || logonly || busy || !R.ncol || !R.col[0].tex || !R.col[0].tex->vk) return;
    static long long last_frame = -1;
    char pre[24] = "";                             /* "<frame>_" before the pass number when a range is captured */
    if (nvn_front_frame_next() != last_frame) { last_frame = nvn_front_frame_next(); n = 0; }
    if (passshot_range()) snprintf(pre, sizeof pre, "%lld_", last_frame);
    Tex *t = R.col[0].tex;
    VTex *vt = t->vk;
    int half4 = t->b.format == 0x29 && vt->bpb == 8;      /* SMO: RGBA16F, shown clamped to 0..1 (alpha too) */
    int half1 = t->b.format == 0x5 && vt->bpb == 2;       /* SMO: R16F (linear depth, shadow maps): raw dump + min/max/mean */
    if (half1) {
        int w1 = vt->w, h1 = vt->h, r1[6] = {0, 0, 0, w1, h1, 1};
        uint16_t *hp = malloc((size_t)w1 * h1 * 2);
        if (hp && read_region(t, NULL, r1, hp, 0, 0)) {
            double mn = 1e30, mx = -1e30, sm = 0;
            for (size_t k = 0; k < (size_t)w1 * h1; k++) { uint16_t hv = hp[k]; int ex = (hv >> 10) & 31, man = hv & 1023; float f = ex == 0 ? ldexpf((float)man, -24) : ex == 31 ? 65504.0f : ldexpf(1.0f + man / 1024.0f, ex - 15); if (hv & 0x8000) f = -f; if (f < mn) mn = f; if (f > mx) mx = f; sm += f; }
            printf("nvn_vk_pass: pass %d: %dx%d R16F min %g max %g mean %g\n", n, w1, h1, mn, mx, sm / ((double)w1 * h1));
            if (getenv("SMO_VK_PASSSHOT_RAW")) { char rp[320]; const char *dd = getenv("SMO_SHOT_DIR"); snprintf(rp, sizeof rp, "%s%spass_%s%03d_05_%dx%d.half", dd ? dd : "", dd ? "/" : "", pre, n, w1, h1); FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(hp, 2, (size_t)w1 * h1, fr); fclose(fr); } }
        }
        free(hp); n++; return;
    }
    if ((vt->bpb != 4 && !half4) || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk_pass: pass %d: colour format 0x%x not captured\n", n++, (unsigned)t->b.format); return; }
    busy = 1;
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    int got = 0;
    if (px && half4) {
        uint16_t *hp = malloc((size_t)w * h * 8);
        if (hp && read_region(t, NULL, rr, hp, 0, 0)) {
            got = 1;
            for (size_t k = 0; k < (size_t)w * h * 4; k++) {
                uint16_t hv = hp[k]; int ex = (hv >> 10) & 31, man = hv & 1023;
                float f = ex == 0 ? ldexpf((float)man, -24) : ex == 31 ? 65504.0f : ldexpf(1.0f + man / 1024.0f, ex - 15);
                if (hv & 0x8000) f = -f;
                px[k] = (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255.0f + 0.5f);
            }
            if (getenv("SMO_VK_PASSSHOT_RAW")) { char rp[320]; const char *dd = getenv("SMO_SHOT_DIR"); snprintf(rp, sizeof rp, "%s%spass_%s%03d_29_%dx%d.half", dd ? dd : "", dd ? "/" : "", pre, n, w, h); FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(hp, 8, (size_t)w * h, fr); fclose(fr); } }
        }
        free(hp);
    } else if (px) got = read_region(t, NULL, rr, px, 0, 0) != 0;
    if (got) {
        int ten = t->b.format == 0x3d || t->b.format == 0x3e;
        long long lit = 0, alpha_hist[4] = {0, 0, 0, 0}; double sum[3] = {0, 0, 0};
        for (size_t k = 0; k < (size_t)w * h; k++) { uint32_t q; memcpy(&q, px + k * 4, 4); alpha_hist[ten ? q >> 30 : (q >> 24) >> 6]++; }
        printf("nvn_vk_pass: pass %d: alpha (top two bits) histogram 0:%lld 1:%lld 2:%lld 3:%lld\n", n, alpha_hist[0], alpha_hist[1], alpha_hist[2], alpha_hist[3]);
        uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
        char path[300];
        const char *dir = getenv("SMO_SHOT_DIR");
        snprintf(path, sizeof path, "%s%spass_%s%03d_%02x.bmp", dir ? dir : "", dir ? "/" : "", pre, n, (unsigned)t->b.format);
        FILE *f = fopen(path, "wb");
        uint8_t head[54] = {'B', 'M'}, *rowb = calloc(1, row);
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        if (f) fwrite(head, 1, 54, f);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                const uint8_t *s = px + ((size_t)y * w + x) * 4;
                int r, g, b;
                if (ten) { uint32_t q; memcpy(&q, s, 4); r = (int)((q & 1023) >> 2); g = (int)(((q >> 10) & 1023) >> 2); b = (int)(((q >> 20) & 1023) >> 2); }
                else { r = s[0]; g = s[1]; b = s[2]; }
                rowb[x * 3] = (uint8_t)b; rowb[x * 3 + 1] = (uint8_t)g; rowb[x * 3 + 2] = (uint8_t)r;
                sum[0] += r; sum[1] += g; sum[2] += b; lit += (r | g | b) > 8;
            }
            if (f) fwrite(rowb, 1, row, f);
        }
        if (f) fclose(f);
        free(rowb);
        if (getenv("SMO_VK_PASSSHOT_RAW")) {         /* SMO debug aid: the texels as read (4 bytes each, rows top first), for alpha and exact values */
            char rp[320]; snprintf(rp, sizeof rp, "%s%spass_%s%03d_%02x_%dx%d.raw", dir ? dir : "", dir ? "/" : "", pre, n, (unsigned)t->b.format, w, h);
            FILE *fr = fopen(rp, "wb"); if (fr) { fwrite(px, 4, (size_t)w * h, fr); fclose(fr); }
        }
        printf("nvn_vk_pass: pass %d: %dx%d format 0x%x: %.1f%% not black, mean %.1f %.1f %.1f -> %s\n", n, w, h, (unsigned)t->b.format,
               100.0 * (double)lit / ((double)w * h), sum[0] / ((double)w * h), sum[1] / ((double)w * h), sum[2] / ((double)w * h), path);
    }
    free(px);
    n++;
    busy = 0;
}
static VkSemaphore g_sem[8][2];
static VkSemaphore g_sem_img[16];                  /* session 11: the "rendering finished" semaphore, one per swapchain image */
static int g_isem;
/* Session 11, the PROGRESS BAR: our own overlay, drawn over the swapchain image after the blit of the game's picture (never
 * into the game's textures, so pictures saved by SMO_SHOT* do not have it). A bar at the bottom of the window:
 *   - while the loading barrier holds a loading screen: the same picture is presented again about ten times a second with
 *     the bar growing (jobs done of jobs queued when the hold began), so the screen is not a still picture;
 *   - while the container store is read from data.arc (first start) and while the preparation runs: a thinner bar on every
 *     presented frame, NOT near a fight.
 * SMO_VK_PROGRESS=0 turns it off. The image: 8x8, two layers (bar colour, track colour), cleared once; drawn by two blits. */
static struct { VkImage img; VMem mem; int made, on; float frac; int thick; void *bar_arg; } g_ovl = {.on = -1};
/* SMO port: the amiibo menu's picture (vk_menu): an image of its own, blitted over the whole swapchain image */
static struct { VkImage img; VMem mem; VkBuffer buf; VMem bmem; VkFormat fmt; int w, h, on; } g_menu;
static void overlay_draw(uint32_t index) {
    if (g_menu.on && g_menu.img) {
        VkImageBlit b;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1].x = g_menu.w; b.srcOffsets[1].y = g_menu.h; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)V.extent.width; b.dstOffsets[1].y = (int32_t)V.extent.height; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, g_menu.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        return;
    }
    if (g_ovl.on < 0) g_ovl.on = !(getenv("SMO_VK_PROGRESS") && !atoi(getenv("SMO_VK_PROGRESS")));
    if (!g_ovl.on || g_ovl.thick <= 0) return;
    if (g_ovl.thick < 8) g_ovl_frames++;
    VkImageSubresourceRange sr2 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!g_ovl.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        VkClearColorValue c0 = {{0.92f, 0.92f, 0.92f, 1.0f}}, c1 = {{0.06f, 0.06f, 0.06f, 1.0f}};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent.width = 8; ci.extent.height = 8; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 2;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_ovl.img));
        vkGetImageMemoryRequirements(V.device, g_ovl.img, &rq);
        g_ovl.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_ovl.img, g_ovl.mem.mem, g_ovl.mem.off));
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &sr2);
        sr2.baseArrayLayer = 1;
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c1, 1, &sr2);
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        g_ovl.made = 1;
    }
    int W = (int)V.extent.width, H = (int)V.extent.height, th = g_ovl.thick * H / 720, x0 = W / 10, x1 = W - W / 10, y1 = H - H / 40, y0;
    float f = g_ovl.frac < 0 ? 0 : g_ovl.frac > 1 ? 1 : g_ovl.frac;
    if (th < 2) th = 2;
    y0 = y1 - th;
    if (x1 - x0 < 8 || y0 < 0) return;
    for (int pass = 0; pass < 2; pass++) {          /* the track, then the bar over it */
        VkImageBlit b;
        int xe = pass ? x0 + (int)((float)(x1 - x0) * f) : x1;
        if (xe <= x0) continue;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.srcSubresource.baseArrayLayer = pass ? 0 : 1;
        b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = 8; b.srcOffsets[1].y = 8; b.srcOffsets[1].z = 1;
        b.dstOffsets[0].x = pass ? x0 : x0 - 2; b.dstOffsets[0].y = pass ? y0 : y0 - 2; b.dstOffsets[1].x = pass ? xe : x1 + 2; b.dstOffsets[1].y = pass ? y1 : y1 + 2; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
}
/* SMO port: the console's Capture button (F12 in the game window; a pad's share / capture button: display.c and
 * nnpad_sdl.c call smo_capture_request). The frame being presented is read back like a SMO_SHOT picture, made 8-bit
 * opaque RGBA, and handed to nnalbum.c, which turns it as the game asked (nn::oe::SetScreenShotImageOrientation: snapshot
 * mode with the camera turned) and saves it in the album folder beside the game's own screenshots. */
static volatile LONG g_capture_req;
void smo_capture_request(void) { InterlockedExchange(&g_capture_req, 1); }
int album_save_capture(const uint8_t *rgba, int w, int h, char *out_path, size_t out_cap);       /* nnalbum.c */
static void capture_vk(VTex *vt) {
    int w = vt->rs ? (int)vt->t->b.width : vt->w, h = vt->rs ? (int)vt->t->b.height : vt->h;
    int ten = vt->t->b.format == 0x3d || vt->t->b.format == 0x3e;          /* RGB10A2 window texture */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("capture: the window texture's format %d is not a 32-bit colour one; no picture\n", (int)vt->fmt); return; }
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); printf("capture: the frame could not be read back; no picture\n"); return; }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t *s = px + i * 4;
        if (ten) { uint32_t q; memcpy(&q, s, 4); s[0] = (uint8_t)((q & 1023) >> 2); s[1] = (uint8_t)(((q >> 10) & 1023) >> 2); s[2] = (uint8_t)(((q >> 20) & 1023) >> 2); }
        s[3] = 255;
    }
    album_save_capture(px, w, h, NULL, 0);
    free(px);
}
static void vk_present2(void *arg, int progress);
/* nvn_vk_m4.c m4_barrier, render thread, g_sh_lock NOT held: the held loading screen once more, with the bar at `frac` */
static void present_progress(float frac) { if (!g_ovl.bar_arg) return; g_ovl.frac = frac; g_ovl.thick = 8; vk_present2(g_ovl.bar_arg, 1); g_ovl.thick = 0; }
/* SMO port: the amiibo menu (smo_amiibo.c), render thread. READ: the frame the PresentJob presented (its window texture),
 * read back like a Capture-button picture. SHOW: the menu's picture, copied when it changed into an image of the swapchain's
 * colour class (a blit between them keeps the stored values, as the frame's own blit does), shown over the whole window by
 * a present of the held frame (vk_present2 progress = 1, as the loading-screen bar; overlay_draw blits the picture). END: the
 * image and its upload buffer freed. */
static void vk_menu(void *arg) {
    UiMenuJob *m = arg;
    PresentJob *j = m->present;
    m->ok = 0;
    if (!V.alive) return;
    if (m->op == UI_MENU_READ) {
        Tex *t = j && j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
        VTex *vt = t && !t->dead ? t->vk : NULL;
        if (!vt || vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
        int w = vt->rs ? (int)vt->t->b.width : vt->w, h = vt->rs ? (int)vt->t->b.height : vt->h, ten = vt->t->b.format == 0x3d || vt->t->b.format == 0x3e;
        int rr[6] = {0, 0, 0, w, h, 1};
        uint8_t *px = malloc((size_t)w * h * 4);
        if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t *s = px + i * 4;
            if (ten) { uint32_t q; memcpy(&q, s, 4); s[0] = (uint8_t)((q & 1023) >> 2); s[1] = (uint8_t)(((q >> 10) & 1023) >> 2); s[2] = (uint8_t)(((q >> 20) & 1023) >> 2); }
            s[3] = 255;
        }
        m->out = px; m->out_w = w; m->out_h = h; m->ok = 1;
        return;
    }
    VkFormat fmt = V.format.format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (g_menu.img && (m->op == UI_MENU_END || g_menu.w != m->w || g_menu.h != m->h || g_menu.fmt != fmt)) {
        cmd_wait_all();
        vkDestroyImage(V.device, g_menu.img, NULL); vmem_free(g_menu.mem);
        vkDestroyBuffer(V.device, g_menu.buf, NULL); vmem_free(g_menu.bmem);
        g_menu.img = NULL;
    }
    if (m->op == UI_MENU_END) { m->ok = 1; return; }
    if (!j || !m->rgba || m->w <= 0 || m->h <= 0) return;
    int fresh = 0;
    if (!g_menu.img) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent.width = (uint32_t)m->w; ci.extent.height = (uint32_t)m->h; ci.extent.depth = 1;
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_menu.img));
        vkGetImageMemoryRequirements(V.device, g_menu.img, &rq);
        g_menu.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_menu.img, g_menu.mem.mem, g_menu.mem.off));
        g_menu.buf = vbuf_new((uint64_t)m->w * m->h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_menu.bmem);
        g_menu.w = m->w; g_menu.h = m->h; g_menu.fmt = fmt;
        fresh = 1;
    }
    if (m->changed || fresh) {                          /* the previous present's copy was waited for by its own cmd_wait_all */
        VkBufferImageCopy c;
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
        c.imageExtent.width = (uint32_t)m->w; c.imageExtent.height = (uint32_t)m->h; c.imageExtent.depth = 1;
        memcpy(g_menu.bmem.map, m->rgba, (size_t)m->w * m->h * 4);
        pass_end();
        cmd_begin();
        image_barrier(g_menu.img, VK_IMAGE_ASPECT_COLOR_BIT, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(R.cmd, g_menu.buf, g_menu.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        image_barrier(g_menu.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    g_menu.on = 1;
    vk_present2(j, 1);
    g_menu.on = 0;
    m->ok = 1;
}
static void vk_present(void *arg) { vk_present2(arg, 0); }
static void vk_present2(void *arg, int progress) {
    PresentJob *j = arg;
    static int shot = -2, every, shot_from, shot_to;
    if (!V.alive) return;
    LONG64 t_present = tk();
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    LONG64 frame = nvn_front_frame_next();
    {   /* session 12, TEST ONLY: SMO_VK_WD_TEST=<frame>[,<seconds>] holds the render thread at that frame (default 12 s), to see the watchdog's report */
        static int wt = -2, ws = 12;
        if (wt == -2) { const char *e = getenv("SMO_VK_WD_TEST"); wt = e ? atoi(e) : -1; if (e && strchr(e, ',')) ws = atoi(strchr(e, ',') + 1); }
        if (wt > 0 && frame == wt && !progress) { printf("nvn_vk_watchdog: TEST: the render thread is held %d s at frame %lld (SMO_VK_WD_TEST)\n", ws, (long long)frame); fflush(stdout); wt = -1; Sleep((DWORD)ws * 1000); }
    }
    if (progress) goto swap;
    if (j->win) for (int i = 0; i < j->win->count && i < 8; i++) wintex_note(j->win->tex[i]);
    g_ovl.bar_arg = arg;
    g_wd_where = 1;
    m4_barrier(frame, R.frame_draws);
    g_wd_where = 0;
    g_ovl.bar_arg = NULL;
    /* the thin bar of a normal frame: the store being read, or the preparation running, and no fight near */
    g_ovl.thick = 0;
    if ((g_prep_state == 5 || g_prep_state == 1) && !fight_recent()) {
        g_ovl.thick = 4;
        g_ovl.frac = g_prep_state == 5 ? (float)g_store_pct / 100.0f : g_prep_nfiles ? (float)g_pq_done / (float)(g_pq_queued > 0 ? g_pq_queued + (LONG64)(g_prep_nfiles - g_prep_read) * 90 : 1) : 0;      /* about 90 variations a file */
    }
    g_render_tid = GetCurrentThreadId();
    if (g_skip_fr_n) { g_skip_frames++; printf("nvn_vk_skipf: frame %lld (%s): %lld draws left out (shader_wait = skip)\n", (long long)frame, g_in_fight ? "fight" : "no fight", (long long)g_skip_fr_n); g_skip_fr_n = 0; }
    if (shot == -2) { shot = getenv("SMO_SHOT") ? atoi(getenv("SMO_SHOT")) : -1; every = getenv("SMO_SHOT_EVERY") ? atoi(getenv("SMO_SHOT_EVERY")) : 0; shot_from = getenv("SMO_SHOT_FROM") ? atoi(getenv("SMO_SHOT_FROM")) : 0; shot_to = getenv("SMO_SHOT_TO") ? atoi(getenv("SMO_SHOT_TO")) : 0x7fffffff; }
    pass_end();
    { int step = nvn_front_take_step_shot(); if (step && vt) { char path[300]; const char *dir = getenv("SMO_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp_vk(path, vt); } }
    if (vt && (frame == shot || (every > 0 && frame >= shot_from && frame <= shot_to && frame % every == 0))) {
        char path[300];
        const char *dir = getenv("SMO_SHOT_DIR");
        snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
        save_bmp_vk(path, vt);
    }
    {   static char at[256] = {1};                    /* SMO_CAPTURE_AT=<frame>[,<frame>...]: the Capture button pressed at those frames (tests) */
        if (at[0] == 1) { const char *e = getenv("SMO_CAPTURE_AT"); snprintf(at, sizeof at, ",%s,", e ? e : ""); }
        char key[32];
        snprintf(key, sizeof key, ",%lld,", (long long)frame);
        if (at[1] != ',' && strstr(at, key)) smo_capture_request();
    }
    {   /* SMO_CAPTURE_GS=<n>: the Capture button pressed at n frames with a draw with a geometry stage (the lightning), at least
         * 60 frames apart (tests) */
        static int left = -1;
        static LONG64 seen, last = -1000;
        if (left < 0) left = getenv("SMO_CAPTURE_GS") ? atoi(getenv("SMO_CAPTURE_GS")) : 0;
        if (left > 0 && g_gs_draws != seen && frame - last >= 60) { left--; last = frame; smo_capture_request(); }
        seen = g_gs_draws;
    }
    gsq_poll();
    if (vt && g_capture_req && InterlockedExchange(&g_capture_req, 0)) capture_vk(vt);       /* the Capture button */
    {   /* session 9, SMO_SHOT_BATTLE=<n>: one picture at the n-th battle frame (dispatches and more than 120 draws), i.e. at the
         * same moment of the fight whatever the loading took: battle_<n>.bmp in SMO_SHOT_DIR */
        static int bshot = -2;
        if (bshot == -2) bshot = getenv("SMO_SHOT_BATTLE") ? atoi(getenv("SMO_SHOT_BATTLE")) : -1;
        if (vt && bshot > 0 && g_battle_frames == bshot) {
            char path[300];
            const char *dir = getenv("SMO_SHOT_DIR");
            snprintf(path, sizeof path, "%s%sbattle_%d.bmp", dir ? dir : "", dir ? "/" : "", bshot);
            save_bmp_vk(path, vt);
            printf("nvn_vk: picture of battle frame %d saved at frame %lld: %s\n", bshot, (long long)frame, path);
            bshot = -1;
        }
    }
swap: ;
    pass_end();
    /* an extra present of a held loading screen: everything submitted so far is waited for first, so that the semaphore pair
     * taken below has no pending operation (validation run s11v2: VUID-vkAcquireNextImageKHR-semaphore-01779, three times,
     * all at such presents: with one submission a present the pair came round before its command buffer had been waited for) */
    if (progress) cmd_wait_all();
    /* The swapchain has the window texture's colour class: a blit between them then keeps the stored values (the OpenGL
     * path blits with FRAMEBUFFER_SRGB off). */
    VkFormat want = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    if (want != V.format.format) { cmd_wait_all(); swapchain_destroy(); V.format.format = want; printf("nvn_vk: swapchain format %d (the window texture's colour class)\n", (int)want); }
    uint32_t index = 0;
    VkResult r = VK_SUCCESS;
    /* one pair of semaphores per present, 8 in turn: a semaphore handed to vkAcquireNextImageKHR must not still be waited
     * for by an earlier submission (validation layer: VUID-vkAcquireNextImageKHR-semaphore-01779) */
    g_isem = (g_isem + 1) % 8;
    if (!g_sem[g_isem][0]) { VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][0])); VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][1])); }
    VkSemaphore acq = g_sem[g_isem][0], rend = g_sem[g_isem][1];
    if (V.swapchain) {      /* the window changed size (F11, display.c): a driver need not say so with OUT_OF_DATE / SUBOPTIMAL */
        RECT rc;
        if (GetClientRect(V.hwnd, &rc) && rc.right > 0 && rc.bottom > 0 && ((uint32_t)rc.right != V.extent.width || (uint32_t)rc.bottom != V.extent.height)) {
            cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated);
        }
    }
    int ok = V.swapchain || swapchain_create();
    InterlockedIncrement64(&g_wd_try);
    if (ok) {
        g_wd_where = 2;
        r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); ok = swapchain_create(); if (ok) r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index); }
        if (ok && r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_ok("vkAcquireNextImageKHR", r); ok = 0; }
    }
    if (!ok) { InterlockedIncrement64(&g_vk_skipped); cmd_submit(NULL, NULL); }
    else {
        int suboptimal = r == VK_SUBOPTIMAL_KHR;
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {   /* session 11: the semaphore a present waits for is one PER SWAPCHAIN IMAGE (validation run s11v3, twice in 9,783
             * frames: VUID-vkQueueSubmit-pSignalSemaphores-00067, "may still be in use by the swapchain", when the driver
             * hands out the same image many times in a row and the ring of 8 comes round); the acquire semaphore stays in the ring */
            if (index < 16) {
                if (!g_sem_img[index]) { VkSemaphoreCreateInfo smi2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi2, NULL, &g_sem_img[index])); }
                rend = g_sem_img[index];
            }
        }
        cmd_begin();
        if (vt) vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd_begin();
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (vt && vt->samples == 1) {
            VkImageBlit b;
            int s4[4] = {0, 0, vt->w, vt->h};
            if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4, j->win->crop, sizeof s4);
            if (vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {      /* track O: the crop is in the game's pixels */
                int x1 = rs_edge(s4[0] + s4[2], vt->sx, vt->w), y1 = rs_edge(s4[1] + s4[3], vt->sy, vt->h);
                s4[0] = rs_edge(s4[0], vt->sx, vt->w); s4[1] = rs_edge(s4[1], vt->sy, vt->h); s4[2] = x1 - s4[0]; s4[3] = y1 - s4[1];
            }
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)V.extent.width; b.dstOffsets[1].y = (int32_t)V.extent.height; b.dstOffsets[1].z = 1;
            vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        } else { VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f; vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
        overlay_draw(index);
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        cmd_submit(acq, rend);
        VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rend; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
        g_wd_where = 4;
        r = vkQueuePresentKHR(V.queue, &pr);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
        else vk_ok("vkQueuePresentKHR", r);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { LONG64 nowt = tk(); if (g_gap_prev && nowt - g_gap_prev > g_gap_max) g_gap_max = nowt - g_gap_prev; g_gap_prev = nowt; g_wd_beat = nowt; }      /* session 12: the watchdog's heartbeat = a frame reached the window */
        if (!progress) InterlockedIncrement64(&g_vk_frames);
    }
    g_wd_where = 0;
    if (progress) { g_ovl_presents++; return; }
    if (g_occ_qn) { LONG64 w0 = g_occ_waits; occ_resolve(g_occ_present ? g_occ_qn : 0); g_occ_present_waits += g_occ_waits > w0; }      /* the frame's occlusion counts (read by the game in the next frame) */
    g_tm[4] += tk() - t_present;
    nvn_front_frame_end(R.frame_draws, R.frame_skipped, R.frame_clears);
    { static LONG64 disp_prev; g_in_fight = R.dispatches != disp_prev; disp_prev = R.dispatches; if (g_in_fight) { g_fight_last = frame; if (g_prep_state == 1) g_prep_fight_frames++; } }      /* a frame with compute dispatches (skinning): fighters are on screen */
    {   /* session 9: per-frame record for the "nvn_vk_gpu_fight:" line */
        static LONG64 t_prev;
        LONG64 t_now = tk();
        int battle = g_in_fight && R.frame_draws > 120;      /* the fight draws 126..450 a frame (entrance over 205), loading screens 38..45 */
        if (battle) g_battle_frames++;
        {   /* how long the way to the fight took (menus are scripted by polls, so two runs differ by their loading) */
            static LONG64 t_first, t_load; static int said;
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            if (!t_first) t_first = t_now;
            if (!battle && R.frame_draws <= 60 && frame > 600) { if (!t_load) t_load = t_now; } else if (!battle && R.frame_draws > 60) t_load = 0;      /* t_load: the start of the run of loading-screen frames before the battle */
            if (battle && !said) { said = 1; printf("nvn_vk: first battle frame is frame %lld, %.1f s after the first present; the loading screen before it lasted %.1f s\n", (long long)frame, (double)(t_now - t_first) / (double)fq.QuadPart, t_load ? (double)(t_now - t_load) / (double)fq.QuadPart : 0.0); }
        }
        if (frame >= 0 && frame < FRSTAT_N) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            g_fr_fight[frame] = (uint8_t)battle;
            g_fr_wall[frame] = t_prev ? (float)((double)(t_now - t_prev) * 1000.0 / (double)fq.QuadPart) : 0.0f;
        }
        t_prev = t_now;
        g_cur_frame = frame + 1;
    }
    {   /* session 8: what this frame paid for textures made or read at their use (hold log), then the textures made ahead */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        if (holdlog_on() && g_up_fr_tk * 1000 > 3 * fq.QuadPart) { static int nl; if (nl++ < 300) printf("nvn_vk_upl: frame %lld (%s): %d textures made or read at their use, %.1f MB, %.1f ms\n", (long long)frame, g_in_fight ? "fight" : "no fight", g_up_fr_n, (double)g_up_fr_bytes / 1048576.0, (double)g_up_fr_tk * 1000.0 / (double)fq.QuadPart); }
        g_up_fr_tk = g_up_fr_bytes = 0; g_up_fr_n = 0;
        tex_ahead(frame);
    }
    if (frame % 300 == 0) {                      /* where the render thread's time went, per frame, in the last 300 frames */
        static LONG64 last[12], last_sets, last_flush, last_reused, last_disp;
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart / 300.0;
        if (g_ts_on > 0) { static double ts_last; static LONG64 n_last; printf("nvn_vk_gpu: frames %lld..%lld: GPU time %.2f ms per frame (first to last command of %lld command buffers finished in the window)\n", (long long)frame - 300, (long long)frame, (g_ts_ns - ts_last) / 1e6 / 300.0, (long long)(g_ts_n - n_last)); ts_last = g_ts_ns; n_last = g_ts_n; }
        printf("nvn_vk_perf: frames %lld..%lld ms/frame: batch replay %.2f (of it: dispatch %.2f, draw preparation %.2f, pipelines %.2f, shader modules %.2f), wait for the GPU %.2f, copy-back %.2f (waiting at batch start %.2f), present %.2f | per frame: %.1f dispatches, %.0f descriptor sets allocated, %.0f reused, %.1f submissions | pipelines not made ahead so far: %lld | holds so far: %lld, %.1f ms, longest %.1f ms\n",
               (long long)frame - 300, (long long)frame, (double)(g_tm[3] - last[3]) * k, (double)(g_tm[2] - last[2]) * k, (double)(g_tm[8] - last[8]) * k, (double)(g_tm[5] - last[5]) * k, (double)(g_tm[6] - last[6]) * k,
               (double)(g_tm[0] - last[0]) * k, (double)(g_tm[1] - last[1]) * k, (double)(g_tm[7] - last[7]) * k, (double)(g_tm[4] - last[4]) * k, (double)(R.dispatches - last_disp) / 300.0, (double)(R.sets - last_sets) / 300.0, (double)(R.sets_reused - last_reused) / 300.0, (double)(R.flushes - last_flush) / 300.0, (long long)g_pipe_miss,
               (long long)(g_hold_n[0] + g_hold_n[1] + g_hold_n[2] + g_hold_n[3]), (double)(g_hold_tk[0] + g_hold_tk[1] + g_hold_tk[2] + g_hold_tk[3]) * k * 300.0, (double)g_hold_max * k * 300.0);
        if (g_gs_draws) printf("nvn_vk_gs: frames %lld..%lld: draws with a geometry stage so far %lld, their pipelines %lld\n", (long long)frame - 300, (long long)frame, (long long)g_gs_draws, (long long)g_gs_pipes);
        {   static LONG64 lg[GEN_N], lw[GEN_N], lcalls, lhits, lagain, lagain_b, lbytes;
            double g[GEN_N], w[GEN_N];
            for (int i = 0; i < GEN_N; i++) { g[i] = (double)(g_gen_why[i] - lg[i]) / 300.0; w[i] = (double)(g_wlog_why[i] - lw[i]) / 300.0; }
            printf("nvn_vk_ring: frames %lld..%lld per frame: streamed %.2f MB, ring_put %.0f calls (%.1f%% cached), copied again within a batch %.0f ranges %.2f MB | new generations: batch %.1f, wrap %.2f, texture to buffer %.2f, write log full %.2f, index range written %.2f | host writes logged: copy-back %.1f, occlusion %.1f, UpdateUniformBuffer %.1f, report %.1f, buffer copy/clear %.1f\n",
                   (long long)frame - 300, (long long)frame, (double)(g_ring_bytes - lbytes) / 1048576.0 / 300.0, (double)(g_rput_calls - lcalls) / 300.0,
                   g_rput_calls > lcalls ? 100.0 * (double)(g_rput_hits - lhits) / (double)(g_rput_calls - lcalls) : 0.0, (double)(g_rput_again - lagain) / 300.0, (double)(g_rput_again_bytes - lagain_b) / 1048576.0 / 300.0,
                   g[GEN_BATCH], g[GEN_WRAP], g[GEN_TEXBUF], g[GEN_LOGFULL], g[GEN_INDEXW], w[GEN_CB], w[GEN_OCC], w[GEN_UBO], w[GEN_REPORT], w[GEN_BUF]);
            memcpy(lg, g_gen_why, sizeof lg); memcpy(lw, g_wlog_why, sizeof lw); lcalls = g_rput_calls; lhits = g_rput_hits; lagain = g_rput_again; lagain_b = g_rput_again_bytes; lbytes = g_ring_bytes;
        }
        if (frame % 3000 == 0) m4_report();
        memcpy(last, g_tm, sizeof last); last_sets = R.sets; last_flush = R.flushes; last_reused = R.sets_reused; last_disp = R.dispatches;
    }
    if (frame % 600 == 0)
        printf("nvn_vk: totals at frame %lld: %lld images made (%lld live, %lld views), %lld uploads (%lld MB), %lld shader modules, %lld pipelines, %lld descriptor sets, %lld command buffers, %lld ring wraps, %lld feedback passes, %lld MB device memory\n",
               (long long)frame, (long long)R.images, (long long)R.images_live, (long long)R.views, (long long)R.uploads, (long long)(R.upload_bytes >> 20), (long long)R.modules, (long long)R.pipelines, (long long)R.sets,
               (long long)R.flushes, (long long)R.ring_wraps, (long long)R.feedback, (long long)(g_mem_total >> 20));
    if (frame % 600 == 0 && R.dispatches) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_vk: compute at frame %lld: %lld dispatches, %lld KB copied back to guest memory, draws using a vertex buffer written by compute in the same batch: %lld, copy-backs forced inside a batch: %lld, waits for the GPU before a copy-back: %lld (%.1f ms in all), %lld buffers made; words the guest wrote into a range pending copy-back: %lld (also written by compute: %lld)\n",
               (long long)frame, (long long)R.dispatches, (long long)(R.cb_bytes >> 10), (long long)R.cb_draws, (long long)R.cb_syncs, (long long)R.cb_waits, (double)R.cb_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)R.cb_made,
               (long long)R.cb_guest_words, (long long)R.cb_conflict_words);
    }
    if (frame % 600 == 0) { printf("nvn_vk: images made per NVN texture format (format:count):"); for (int i = 0; i < 256; i++) if (g_fmt_hist[i]) printf(" 0x%x:%u", i, g_fmt_hist[i]); printf("\n"); }
    {   static long long lo = -2, hi;
        if (lo == -2) { const char *e = getenv("SMO_VK_FRAMELOG"); lo = e ? atoll(e) : -1; hi = e && strchr(e, '-') ? atoll(strchr(e, '-') + 1) : lo; }
        if (lo >= 0 && frame >= lo && frame <= hi)
            printf("nvn_vk_frame: %lld draws %lld skipped %lld (render off %lld) conditional %lld (off %lld) passes %lld texture barriers %lld vertex over-reads %lld ring wraps %lld streamed %lld KB condhash %016llx\n", (long long)frame, (long long)R.frame_draws,
                   (long long)R.frame_skipped, (long long)g_fr_offskip, (long long)g_fr_cond, (long long)g_fr_condoff, (long long)g_fr_passes, (long long)g_fr_texbar, (long long)g_fr_vtxover, (long long)g_fr_wraps, (long long)(g_ring_bytes - g_fr_ring0) >> 10, (unsigned long long)g_fr_condhash);
        g_fr_cond = g_fr_condoff = g_fr_offskip = g_fr_passes = g_fr_texbar = g_fr_wraps = g_fr_vtxover = 0; g_fr_ring0 = g_ring_bytes; g_fr_condhash = 1469598103934665603ull;
    }
    R.frame_draws = R.frame_skipped = R.frame_clears = 0;
}
const RBackend rb_vk = {"vk", vk_submit, vk_submit_async, vk_present, vk_present_movie, vk_fence, vk_sync_wait, vk_server_wait, vk_sync_drop, vk_flush, vk_finish, vk_write_texels, vk_read_texels, vk_housekeeping,
                        vk_reset_status, vk_no_grid, vk_idle, vk_menu};

/* ---------------------------------------------------------------- start and end */
static VTex *dummy_image(VkImageType type, uint32_t layers, VkFlags flags) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    VkClearColorValue z;
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vt->magic = 0x56544558; vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = vt->h = vt->d = 1; vt->levels = 1; vt->layers = (int)layers; vt->samples = 1;
    ci.imageType = type; ci.format = vt->fmt; ci.extent.width = ci.extent.height = ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.flags = flags;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&z, 0, sizeof z);
    vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    return vt;
}
static VkImageView dummy_view(VTex *vt, VkImageViewType type, uint32_t layers) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VView *n = calloc(1, sizeof *n);
    ci.image = vt->img; ci.viewType = type; ci.format = vt->fmt; ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.levelCount = 1; ci.subresourceRange.layerCount = layers;
    VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
    n->vtype = 100 + (int)type; n->next = vt->views; vt->views = n;
    return n->view;
}
/* ---------------------------------------------------------------- M4: pipelines made before they are needed (design section 5, step 1)
 * At start the worker threads read ..\cache\vk\pipes.bin and make every pipeline listed there (stages from their records and
 * SPIR-V files, pipelines through the driver's pipeline cache kept in ..\cache\vk\pipelines_<gpu>.bin). The render thread
 * waits for them before it replays its first batch, so the tables are never shared with a running worker. */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's start-up workers) */
static HANDLE g_pw_thread[16];
static int g_pw_nthread, g_pw_n, g_pw_waited;
static PipeRec *g_pw_rec;
static volatile LONG g_pw_next;
static LONG64 g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
static VStage *pw_stage(int s, uint64_t h) {      /* g_sh_lock held */
    for (VStage *v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s) return v;
    VStage *v = vstage_from_rec(s, h);
    if (v) InterlockedIncrement64(&g_pw_stages);
    return v;
}
static DWORD WINAPI pw_main(void *arg) {
    (void)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        EnterCriticalSection(&g_sh_lock);
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;
        LeaveCriticalSection(&g_sh_lock);
        if (!vs || (r->fs_h && !fs)) { InterlockedIncrement64(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made at the draw as on a first run */
        if (r->compute) { compute_pipe_make(vs); InterlockedIncrement64(&g_pw_pipes); continue; }
        double ms;
        r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
        Pipe *p = pipe_make(&r->k, vs, fs, &ms);
        uint64_t h = fnv(0xcbf29ce484222325ULL, &r->k, sizeof r->k);
        EnterCriticalSection(&g_sh_lock);
        p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
        LeaveCriticalSection(&g_sh_lock);
        InterlockedIncrement64(&g_pw_pipes);
    }
    InterlockedExchange64(&g_pw_t1, tk());
    return 0;
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    InitializeCriticalSection(&g_sh_lock);
    {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "../cache/vk/pipelines_%04x_%04x_%08x.bin", (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        printf("nvn_vk: pipeline cache %s: %llu KB read\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10));
        free(data);
    }
    if (getenv("SMO_VK_PREWARM") && !atoi(getenv("SMO_VK_PREWARM"))) { printf("nvn_vk: SMO_VK_PREWARM=0: no pipeline is made ahead\n"); return; }
    FILE *fp = fopen("../cache/vk/pipes.bin", "rb");
    if (!fp) { printf("nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC, every pipeline is made at the draw that first needs it\n"); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_pw_rec = malloc((size_t)n + 1);
    g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
    fclose(fp);
    int kept = 0;
    qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
    for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
    g_pw_n = kept;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SMO_VK_WORKERS") && atoi(getenv("SMO_VK_WORKERS")) > 0 && atoi(getenv("SMO_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SMO_VK_WORKERS"));
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, NULL, 0, NULL);
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin: %d worker threads make them now\n", g_pw_n, g_pw_nthread);
}
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    if (!g_pw_nthread) return;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk();
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, INFINITE);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(g_pw_rec); g_pw_rec = NULL;
}
static void pcache_save(void) {
    size_t n = 0;
    if (!g_pcache || vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (data && vkGetPipelineCacheData(V.device, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[176];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
        FILE *fp = fopen(tmp, "wb");
        if (fp) { fwrite(data, 1, n, fp); fclose(fp); MoveFileExA(tmp, g_pcache_path, MOVEFILE_REPLACE_EXISTING); printf("nvn_vk: pipeline cache saved: %llu KB\n", (unsigned long long)(n >> 10)); }
    }
    free(data);
}
#endif
#include "nvn_vk_wd.c"                             /* session 12: the freeze watchdog (wd_start / wd_stop) */
static void vkd_init(void) {
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has no " #n);
    VKD(X)
#undef X
    vkCmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(V.device, "vkCmdSetVertexInputEXT");
    if (D.has_push) { vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.device, "vkCmdPushDescriptorSetKHR"); if (!vkCmdPushDescriptorSetKHR) D.has_push = 0; }
    if (D.has_eds3) {
#define X3(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has VK_EXT_extended_dynamic_state3 but no " #n);
        X3(vkCmdSetColorBlendEnableEXT) X3(vkCmdSetColorBlendEquationEXT) X3(vkCmdSetColorWriteMaskEXT) X3(vkCmdSetPolygonModeEXT) X3(vkCmdSetAlphaToCoverageEnableEXT) X3(vkCmdSetSampleMaskEXT)
        if (D.f2.features.depthClamp) { X3(vkCmdSetDepthClampEnableEXT) }
#undef X3
    }
    printf("nvn_vk: descriptors: buffers (set 0) %s, textures (set 1) one set per distinct binding list per command buffer\n", D.has_push ? "pushed (VK_KHR_push_descriptor)" : "one allocated set per draw");
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &D.mp);
    for (int i = 0; i < NSLOT; i++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ai.commandPool = V.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(V.device, &ai, &R.slot[i].cmd));
        VKC(vkCreateFence(V.device, &fi, NULL, &R.slot[i].fence));
    }
    R.ring_size = 256ull << 20;
    R.ring = vbuf_new(R.ring_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &R.ring_mem);
    R.zero = vbuf_new(65536 + 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &R.zero_mem);
    memset(R.zero_mem.map, 0, 65536 + 64);
    { float f1 = 1.0f; uint32_t i1 = 1; memcpy((uint8_t *)R.zero_mem.map + 65536 + 12, &f1, 4); memcpy((uint8_t *)R.zero_mem.map + 65536 + 28, &i1, 4); }
    R.gen = 1; R.point_size = 1.0f; R.line_width = 1.0f; R.drange[1] = 1.0f; R.sample_mask = 0xffffffffu; R.chanmask = 0xffffffffu; R.logic = 3; R.poly[2] = 2; R.poly[1] = 1;
    R.svmask[0] = R.svmask[1] = R.swmask[0] = R.swmask[1] = 0xff; R.ds = 0x10 * 2;
    for (int i = 0; i < 8; i++) { R.blend[i][0] = 2; R.blend[i][1] = 1; R.blend[i][2] = 2; R.blend[i][3] = 1; R.blend[i][4] = 1; R.blend[i][5] = 1; }
    cmd_begin();
    g_dummy.d2 = dummy_image(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT); g_dummy.d1 = dummy_image(VK_IMAGE_TYPE_1D, 1, 0); g_dummy.d3 = dummy_image(VK_IMAGE_TYPE_3D, 1, 0);
    g_dummy.v[DIM_1D] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D, 1); g_dummy.v[DIM_1DA] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
    g_dummy.v[DIM_2D] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D, 1); g_dummy.v[DIM_2DA] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
    g_dummy.v[DIM_CUBE] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE, 6); g_dummy.v[DIM_CUBEA] = D.f2.features.imageCubeArray ? dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6) : g_dummy.v[DIM_CUBE];
    g_dummy.v[DIM_3D] = dummy_view(g_dummy.d3, VK_IMAGE_VIEW_TYPE_3D, 1);
    {   /* a texture bound without a sampler: the texture's own OpenGL parameters (nearest, nearest mip, repeat) */
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.maxLod = 1000.0f; ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VKC(vkCreateSampler(V.device, &ci, NULL, &g_smp_default));
    }
    cmd_wait_all();
    CreateDirectoryA("../cache/vk", NULL);
    prewarm_start();
    wd_start();
    printf("nvn_vk: drawing part ready: %d command buffers in flight, stream ring %llu MB, %u memory types; debug checks %s\n", NSLOT, (unsigned long long)(R.ring_size >> 20), (unsigned)D.mp.memoryTypeCount, D.debug ? "on" : "off (SMO_VK_DEBUG=1)");
    if (RS_ON) printf("nvn_vk_scale: internal resolution scale %.3f (%s): screen-class targets are made at %d x %d instead of 1920 x 1080; fragment stages are another translation\n",
                      (double)rs_scale(), getenv("SMO_VK_SCALE") ? "SMO_VK_SCALE" : "graphics.ini", (int)(1920.0f * rs_scale() + 0.5f), (int)(1080.0f * rs_scale() + 0.5f));
    fflush(stdout);
}
static int frstat_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* Session 9: battle frames only (dispatches and more than 120 draws: the fight and the result screen's fighters); the first 30 are left out (their buffers were recorded
 * while the stage was still being set up). GPU ms = sum of the spans of the frame's command buffers (SMO_VK_GPUTIME=1). */
static void frstat_report(void) {
    static float g[FRSTAT_N], w[FRSTAT_N];
    int n = 0, seen = 0, slow = 0;
    double wsum = 0, gsum = 0;
    for (int f = 0; f < FRSTAT_N; f++) {
        if (!g_fr_fight[f] || seen++ < 30) continue;
        g[n] = g_fr_gpu[f]; w[n] = g_fr_wall[f]; wsum += w[n]; gsum += g[n]; if (w[n] > 33.4f) slow++; n++;
    }
    if (n < 100) return;
    qsort(g, (size_t)n, sizeof g[0], frstat_cmp); qsort(w, (size_t)n, sizeof w[0], frstat_cmp);
    printf("nvn_vk_gpu_fight: %d battle frames: GPU ms per frame median %.2f, p99 %.2f, max %.2f, mean %.2f%s | time between presents: median %.2f ms, p99 %.2f, max %.2f, %d frames over 33.4 ms | %.1f frames per second over the battle\n",
           n, (double)g[n / 2], (double)g[n * 99 / 100], (double)g[n - 1], gsum / n, g_ts_on > 0 ? "" : " (SMO_VK_GPUTIME not set: no GPU times)",
           (double)w[n / 2], (double)w[n * 99 / 100], (double)w[n - 1], slow, wsum > 0 ? 1000.0 * n / wsum : 0.0);
}
static void vkd_shutdown(void) {
    wd_stop();
    if (!R.ring) return;
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_tex: textures made ahead of their first use: %lld (%.0f MB, %.0f ms in all, at most %.1f ms after one present); made or read at their use: %lld times, %.0f ms (of them in fight frames: %lld, %.0f ms)\n",
               (long long)g_ta_n, (double)g_ta_bytes / 1048576.0, (double)g_ta_tk * k, (double)g_ta_max_tk * k, (long long)g_late_n, (double)g_late_tk * k, (long long)g_late_fight_n, (double)g_late_fight_tk * k);
    }
    frstat_report();
    if (RS_ON && rs_ui_native()) printf("nvn_vk_scale: SMO_VK_UI_NATIVE=1: the window's textures are kept at the game's size; passes with such a colour target and a SCALED depth target (drawn without depth): %lld\n", (long long)g_rs_mixed);
    if (RS_ON) printf("nvn_vk_scale: resolution scale %.3f: %lld images made scaled, %lld fragment stages translated in this run for it (%lld of them read the pixel position, %lld fetch texels by integer position), %lld uploads into and %lld read-backs from a scaled image, %lld copies / blits touching one\n",
                      (double)rs_scale(), (long long)g_rs_images, (long long)g_rs_stages, (long long)g_rs_fcs, (long long)g_rs_tf, (long long)g_rs_up, (long long)g_rs_down, (long long)g_rs_blits);
    m4_stop();
    pcache_save();
    if (g_pcache) { vkDestroyPipelineCache(V.device, g_pcache, NULL); g_pcache = NULL; }
    for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]);
    R.done = ~0ull; garbage_collect();
    while (g_vtex_all) { VTex *vt = g_vtex_all; if (vt->t) vt->t->vk = NULL; vt_destroy(vt); }
    for (int i = 0; i < 1024; i++) for (Pipe *p = g_pipes[i], *n; p; p = n) { n = p->next; vkDestroyPipeline(V.device, p->p, NULL); if (p->p_fast) vkDestroyPipeline(V.device, p->p_fast, NULL); if (!p->libbed) { vkDestroyPipelineLayout(V.device, p->layout, NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[0], NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[1], NULL); } free(p); }
    gpl_destroy();
    for (int i = 0; i < 8; i++) for (int k = 0; k < 2; k++) if (g_sem[i][k]) { vkDestroySemaphore(V.device, g_sem[i][k], NULL); g_sem[i][k] = NULL; }
    for (int i = 0; i < 16; i++) if (g_sem_img[i]) { vkDestroySemaphore(V.device, g_sem_img[i], NULL); g_sem_img[i] = NULL; }
    if (g_ovl.made) { vkDestroyImage(V.device, g_ovl.img, NULL); g_ovl.made = 0; }      /* the progress bar's image; its memory goes with the blocks */
    for (int i = 0; i < 256; i++) if (g_cb[i].buf) { vkDestroyBuffer(V.device, g_cb[i].buf, NULL); g_cb[i].buf = NULL; g_cb[i].state = 0; }
    g_cb_live = g_cb_pending = g_ncdisp = 0;
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i], *n; v; v = n) {
        n = v->next;
        if (v->cp) { vkDestroyPipeline(V.device, v->cp, NULL); vkDestroyPipelineLayout(V.device, v->cl, NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[0], NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[1], NULL); }
        if (v->mod) vkDestroyShaderModule(V.device, v->mod, NULL);
        free(v);
    }
    for (int i = 0; i < g_nsmp; i++) vkDestroySampler(V.device, g_smp[i].s, NULL);
    vkDestroySampler(V.device, g_smp_default, NULL);
    if (g_ts_pool) { vkDestroyQueryPool(V.device, g_ts_pool, NULL); g_ts_pool = NULL; g_ts_on = 0; }
    if (g_occ_pool) { vkDestroyQueryPool(V.device, g_occ_pool, NULL); g_occ_pool = NULL; g_occ_qn = 0; }
    for (int i = 0; i < NSLOT; i++) { for (int k = 0; k < R.slot[i].npool; k++) vkDestroyDescriptorPool(V.device, R.slot[i].pool[k], NULL); vkDestroyFence(V.device, R.slot[i].fence, NULL); }
    vkDestroyBuffer(V.device, R.ring, NULL); vkDestroyBuffer(V.device, R.zero, NULL);
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) { vkFreeMemory(V.device, g_blk[b]->mem, NULL); free(g_blk[b]); g_blk[b] = NULL; }
    R.ring = NULL;
    printf("nvn_vk: drawing part shut down: %lld images, %lld pipelines, %lld shader modules, %d samplers destroyed; %lld bytes of block space lost to fragmentation records\n", (long long)R.images, (long long)R.pipelines, (long long)R.modules, g_nsmp, (long long)g_mem_leak);
    printf("nvn_vk: draws reading vertices past the range the game bound: %lld%s, %lld left as bound (over 16 MB more); index ranges scanned for their largest index: %lld\n",
           (long long)g_vtx_over, g_vtxgrow > 0 ? " (copied as far as they read)" : g_vtxgrow == 0 ? " (SMO_VK_VTXGROW=0: left as bound)" : "", (long long)g_vtx_capped, (long long)g_imax_scans);
    printf("nvn_vk: texture barriers (nvnCommandBufferBarrier, order fragments): %lld, %lld of them inside a pass that samples its own target (the pass ended there), %lld inside other passes (nothing to do)\n", (long long)g_texbar, (long long)g_texbar_split, (long long)g_texbar_skip);
    printf("nvn_vk: stream ring: %lld wraps, %lld waits for a submission still reading the part about to be written%s\n", (long long)R.ring_wraps, (long long)g_ring_waits, g_ring_drain > 0 ? " (SMO_VK_RING_DRAIN=1: every wrap waited for the GPU)" : "");
    printf("nvn_vk: images: %lld textures made with storage use (NVN_TEXTURE_FLAGS_IMAGE), %lld storage views, %lld dispatches that wrote images; draws with a geometry stage: %lld (%lld pipelines); indirect: %lld draws, %lld dispatches\n", (long long)g_storage_tex, (long long)g_img_views, (long long)g_img_disp, (long long)g_gs_draws, (long long)g_gs_pipes, (long long)g_indirect_draws, (long long)g_indirect_disp);
    if (g_occ_reports || g_occ_folds) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_vk: occlusion counts: %lld reports (%lld written after their frame was presented), %lld compared by a conditional render (%lld of them waited for); "
               "%lld waits for the GPU, %.0f ms in all (%lld at a present%s); %lld long counts folded%s\n", (long long)g_occ_reports, (long long)g_occ_late, (long long)g_occ_cond,
               (long long)g_occ_cond_waits, (long long)g_occ_waits, (double)g_occ_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)g_occ_present_waits,
               g_occ_present ? "" : ", off: SMO_VK_OCC_PRESENT=0", (long long)g_occ_folds, g_occ_sync ? "; SMO_VK_OCC_SYNC=1: each report waited for" : "");
    }
}
