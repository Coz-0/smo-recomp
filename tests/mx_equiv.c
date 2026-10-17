/* mx_equiv.c - equivalence test of the shader translator's two control-flow forms (SMO port).
 *
 * For every fragment stage dumped by the renderer (vk_prog_<n>_1.bin: program header + Maxwell code; SMO_VK_DUMP_MINBYTES=0
 * with an empty SMO_VK_CACHE dumps every stage it translates), mx_translate is run twice: structured (plan_structured:
 * guarded blocks + loops) and with SMO_MX_NOSTRUCT=1 (the pc switch). When the two texts differ, both are turned into
 * COMPUTE shaders (GL_NV_compute_shader_derivatives keeps implicit-LOD sampling and derivatives working: quads of 2x2):
 * inputs (in_attr*) come from a buffer of random values, gl_FragCoord is the invocation's pixel, every discard and
 * output is recorded. Both run on the GPU over 64x64 invocations with the same random constant buffers and textures;
 * discards must match exactly and outputs within 1e-4 (relative): the two forms are compiled separately, and the
 * driver may contract a multiply-add differently in each.
 * Not tested (counted): stages with images or storage buffers, multisample textures, a text glslang refuses.
 *
 *   build: python tests/build_mx_equiv.py      run: build\bin\mx_equiv.exe <folder of .bin> [max shaders]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>
#include <vulkan/vulkan.h>
#include "mx_translate.h"

typedef int (*GlslFn)(int, const char *, uint32_t **, size_t *, char *, size_t);
typedef void (*FreeFn)(uint32_t *);
static GlslFn g_glsl;
static FreeFn g_free;

#define W 64
#define H 64
#define NINV (W * H)
#define IN_STRIDE 128                              /* uints per invocation: 32 attributes x 4 */
#define OUT_STRIDE 40                              /* 8 outputs x 4, depth, discard flag, sample mask */

static VkInstance g_inst;
static VkPhysicalDevice g_gpu;
static VkDevice g_dev;
static VkQueue g_q;
static uint32_t g_qf;
static VkCommandPool g_pool;
static VkPhysicalDeviceMemoryProperties g_mp;
static uint32_t g_seed = 12345;
static uint32_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xffffff) / 16777216.0f; }
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s failed: %d\n", #x, r_); exit(2); } } while (0)

typedef struct { VkBuffer b; VkDeviceMemory m; void *map; VkDeviceSize size; } Buf;
static uint32_t mem_type(uint32_t bits, VkFlags want) {
    for (uint32_t i = 0; i < g_mp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("no memory type\n"); exit(2);
}
static Buf buf_new(VkDeviceSize size) {
    Buf b; memset(&b, 0, sizeof b); b.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VKC(vkCreateBuffer(g_dev, &ci, NULL, &b.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, b.b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(g_dev, &ai, NULL, &b.m)); VKC(vkBindBufferMemory(g_dev, b.b, b.m, 0)); VKC(vkMapMemory(g_dev, b.m, 0, size, 0, &b.map));
    return b;
}
static void buf_free(Buf *b) { vkDestroyBuffer(g_dev, b->b, NULL); vkFreeMemory(g_dev, b->m, NULL); }

/* one-time submit helper */
static VkCommandBuffer cmd_begin(void) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = g_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(g_dev, &ai, &c));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bi)); return c;
}
static void cmd_end(VkCommandBuffer c) {
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VKC(vkQueueWaitIdle(g_q));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &c);
}

/* textures: one image per sampler type, random contents, one level */
typedef struct { VkImage img; VkDeviceMemory m; VkImageView view; VkSampler smp; int ok; } Tex;
static Tex make_tex(const char *type, int shadow) {
    Tex t; memset(&t, 0, sizeof t);
    int is_u = type[0] == 'u', is_i = type[0] == 'i';
    const char *base = type + (is_u || is_i) + 7;                /* after "sampler" */
    VkImageType it = VK_IMAGE_TYPE_2D; VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D; uint32_t w = 32, h = 32, d = 1, layers = 1; VkImageCreateFlags fl = 0;
    if (!strncmp(base, "1DArray", 7)) { it = VK_IMAGE_TYPE_1D; vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; h = 1; layers = 4; }
    else if (!strncmp(base, "1D", 2)) { it = VK_IMAGE_TYPE_1D; vt = VK_IMAGE_VIEW_TYPE_1D; h = 1; }
    else if (!strncmp(base, "2DMS", 4)) return t;
    else if (!strncmp(base, "2DArray", 7)) { vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = 4; }
    else if (!strncmp(base, "2D", 2)) vt = VK_IMAGE_VIEW_TYPE_2D;
    else if (!strncmp(base, "3D", 2)) { it = VK_IMAGE_TYPE_3D; vt = VK_IMAGE_VIEW_TYPE_3D; d = 8; }
    else if (!strncmp(base, "CubeArray", 9)) { vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; layers = 12; fl = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; }
    else if (!strncmp(base, "Cube", 4)) { vt = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; fl = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; }
    else return t;
    VkFormat f = shadow ? VK_FORMAT_D32_SFLOAT : is_u ? VK_FORMAT_R32G32B32A32_UINT : is_i ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    VkImageAspectFlags asp = shadow ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.flags = fl; ci.imageType = it; ci.format = f; ci.extent.width = w; ci.extent.height = h; ci.extent.depth = d;
    ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VKC(vkCreateImage(g_dev, &ci, NULL, &t.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, t.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(g_dev, &ai, NULL, &t.m)); VKC(vkBindImageMemory(g_dev, t.img, t.m, 0));
    uint32_t texel = shadow ? 4 : 16, n = w * h * d * layers;
    Buf up = buf_new((VkDeviceSize)n * texel);
    for (uint32_t k = 0; k < n * texel / 4; k++) ((uint32_t *)up.map)[k] = shadow ? (union { float f; uint32_t u; }){rndf(0, 1)}.u : is_u || is_i ? rnd() % 64 : (union { float f; uint32_t u; }){rndf(-1, 2)}.u;
    VkCommandBuffer c = cmd_begin();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = t.img; b.subresourceRange.aspectMask = asp; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = layers;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp; memset(&cp, 0, sizeof cp); cp.imageSubresource.aspectMask = asp; cp.imageSubresource.layerCount = layers; cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = d;
    vkCmdCopyBufferToImage(c, up.b, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    cmd_end(c); buf_free(&up);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = t.img; vi.viewType = vt; vi.format = f; vi.subresourceRange.aspectMask = asp; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = layers;
    VKC(vkCreateImageView(g_dev, &vi, NULL, &t.view));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = (is_u || is_i || shadow) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.maxLod = 0.25f;
    if (shadow) { si.compareEnable = VK_TRUE; si.compareOp = VK_COMPARE_OP_LESS; }
    VKC(vkCreateSampler(g_dev, &si, NULL, &t.smp));
    t.ok = 1; return t;
}
static void tex_free(Tex *t) { if (!t->ok) return; vkDestroySampler(g_dev, t->smp, NULL); vkDestroyImageView(g_dev, t->view, NULL); vkDestroyImage(g_dev, t->img, NULL); vkFreeMemory(g_dev, t->m, NULL); }

/* ---- GLSL fragment text -> compute text */
static void sb_add(char **s, size_t *n, size_t *cap, const char *t, size_t len) {
    if (*n + len + 1 > *cap) { *cap = (*n + len + 1) * 2; *s = realloc(*s, *cap); }
    memcpy(*s + *n, t, len); *n += len; (*s)[*n] = 0;
}
static int is_id(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* replaces whole-word occurrences */
static char *replace_word(const char *src, const char *word, const char *with) {
    size_t n = 0, cap = strlen(src) + 64, wl = strlen(word); char *out = malloc(cap); out[0] = 0;
    for (const char *p = src; *p;) {
        if (!strncmp(p, word, wl) && (p == src || !is_id(p[-1])) && !is_id(p[wl])) { sb_add(&out, &n, &cap, with, strlen(with)); p += wl; }
        else { sb_add(&out, &n, &cap, p, 1); p++; }
    }
    return out;
}
static char *to_compute(const char *glsl, uint32_t *in_mask, char in_type[32][8]) {
    size_t n = 0, cap = strlen(glsl) * 2 + 4096; char *out = malloc(cap); out[0] = 0;
    *in_mask = 0;
    const char *p = glsl;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) + 1 : strlen(p);
        char line[1024]; size_t ll = len < 1023 ? len : 1023; memcpy(line, p, ll); line[ll] = 0;
        unsigned loc; char ty[16], nm[32];
        static int injected;
        if (p == glsl) injected = 0;
        if (!strncmp(line, "#version", 8)) {
            sb_add(&out, &n, &cap, p, len);
            const char *x = "#extension GL_NV_compute_shader_derivatives : require\n";
            sb_add(&out, &n, &cap, x, strlen(x));
        } else if (!injected && line[0] != '#') {          /* after the last directive: the compute declarations, then this line again below */
            const char *x = "layout(local_size_x = 8, local_size_y = 8) in;\nlayout(derivative_group_quadsNV) in;\n"
                            "vec4 mx_fc; float mx_fd; uint mx_disc = 0u; int mx_sm[1]; int mx_smi[1] = int[1](1);\n";
            sb_add(&out, &n, &cap, x, strlen(x));
            injected = 1;
            continue;                                        /* the same line is handled on the next pass of the loop */
        } else if (strstr(line, "layout(location") && strstr(line, " in ") && strstr(line, "in_attr")) {
            const char *q = strstr(line, "in_attr"); unsigned k = (unsigned)atoi(q + 7);
            const char *tq = strstr(line, " in ") + 4; sscanf(tq, "%15s", ty);
            if (k < 32) { *in_mask |= 1u << k; snprintf(in_type[k], 8, "%s", ty); }
            char d[96]; snprintf(d, sizeof d, "%s in_attr%u;\n", ty, k); sb_add(&out, &n, &cap, d, strlen(d));
        } else if (sscanf(line, "layout(location = %u) out %15s %31[^;];", &loc, ty, nm) == 3) {
            char d[96]; snprintf(d, sizeof d, "%s %s;\n", ty, nm); sb_add(&out, &n, &cap, d, strlen(d));
        } else if (strstr(line, "early_fragment_tests")) {
        } else sb_add(&out, &n, &cap, p, len);
        p += len;
    }
    static const char *const repl[][2] = {{"gl_FragCoord", "mx_fc"}, {"gl_FrontFacing", "true"}, {"gl_HelperInvocation", "false"}, {"gl_FragDepth", "mx_fd"},
                                          {"gl_SampleMaskIn", "mx_smi"}, {"gl_SampleMask", "mx_sm"}, {"gl_SampleID", "0"}, {"discard", "{ mx_disc = 1u; return; }"}, {"main", "mx_main"}};
    for (size_t k = 0; k < sizeof repl / sizeof repl[0]; k++) { char *t = replace_word(out, repl[k][0], repl[k][1]); free(out); out = t; }
    n = strlen(out); cap = n + 1;
    char tail[8192]; size_t tn = 0;
    tn += (size_t)snprintf(tail + tn, sizeof tail - tn, "void main() {\n uint id = gl_GlobalInvocationID.y * %uu + gl_GlobalInvocationID.x;\n mx_fc = vec4(vec2(gl_GlobalInvocationID.xy) + 0.5, 0.5, 1.0); mx_fd = 0.5; mx_sm[0] = -1;\n", W);
    for (int k = 0; k < 32; k++) if (*in_mask >> k & 1) {
        const char *t = in_type[k];
        const char *cv = t[0] == 'u' ? "" : t[0] == 'i' ? "ivec4" : "uintBitsToFloat";
        tn += (size_t)snprintf(tail + tn, sizeof tail - tn, " in_attr%d = %s(uvec4(mx_in[id * %uu + %du], mx_in[id * %uu + %du], mx_in[id * %uu + %du], mx_in[id * %uu + %du]));\n",
                               k, cv, IN_STRIDE, k * 4, IN_STRIDE, k * 4 + 1, IN_STRIDE, k * 4 + 2, IN_STRIDE, k * 4 + 3);
    }
    tn += (size_t)snprintf(tail + tn, sizeof tail - tn, " mx_mx_main_wrap();\n}\n");
    /* outputs: frag0..frag7 as declared */
    char store[4096]; size_t sn = 0;
    sn += (size_t)snprintf(store + sn, sizeof store - sn, "layout(std430, set = 2, binding = 0) readonly buffer MXIN { uint mx_in[]; };\nlayout(std430, set = 2, binding = 1) buffer MXOUT { uint mx_out[]; };\nvoid mx_mx_main_wrap() {\n mx_main();\n uint id = gl_GlobalInvocationID.y * %uu + gl_GlobalInvocationID.x;\n", W);
    for (int k = 0; k < 8; k++) {
        char name[16]; snprintf(name, sizeof name, "frag%d;", k);
        char pat[32]; snprintf(pat, sizeof pat, " frag%d;", k);
        if (strstr(out, pat)) sn += (size_t)snprintf(store + sn, sizeof store - sn, " for (int c = 0; c < 4; c++) mx_out[id * %uu + %du + uint(c)] = floatBitsToUint(frag%d[c]);\n", OUT_STRIDE, k * 4, k);
    }
    sn += (size_t)snprintf(store + sn, sizeof store - sn, " mx_out[id * %uu + 32u] = floatBitsToUint(mx_fd); mx_out[id * %uu + 33u] = mx_disc; mx_out[id * %uu + 34u] = uint(mx_sm[0]);\n}\n", OUT_STRIDE, OUT_STRIDE, OUT_STRIDE);
    /* the wrapper is defined before main(): put store, then tail */
    char *res = malloc(n + sn + tn + 16);
    memcpy(res, out, n); memcpy(res + n, store, sn); memcpy(res + n + sn, tail, tn); res[n + sn + tn] = 0;
    free(out);
    return res;
}

/* ---- run one compute text with the given resources; returns 0 when it could not be built */
typedef struct { Buf cb[18]; Tex tex[64]; MxOut *o; Buf in, out; } Res;
static int run(const char *text, Res *r, MxOut *o, uint32_t cbufs, char *why) {
    uint32_t *words; size_t count; char log[4096];
    if (!g_glsl(5, text, &words, &count, log, sizeof log)) { snprintf(why, 200, "glslang: %.150s", log); return 0; }
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mci.codeSize = count * 4; mci.pCode = words;
    VkShaderModule mod; VKC(vkCreateShaderModule(g_dev, &mci, NULL, &mod)); g_free(words);
    VkDescriptorSetLayoutBinding b0[18], b1[64], b2[2]; uint32_t n0 = 0, n1 = 0;
    for (uint32_t k = 0; k < 18; k++) if (cbufs >> k & 1) { memset(&b0[n0], 0, sizeof b0[n0]); b0[n0].binding = 40 + k; b0[n0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b0[n0].descriptorCount = 1; b0[n0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; n0++; }
    for (int k = 0; k < o->nsmp; k++) { memset(&b1[n1], 0, sizeof b1[n1]); b1[n1].binding = o->smp[k].binding; b1[n1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b1[n1].descriptorCount = 1; b1[n1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; n1++; }
    for (int k = 0; k < 2; k++) { memset(&b2[k], 0, sizeof b2[k]); b2[k].binding = (uint32_t)k; b2[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b2[k].descriptorCount = 1; b2[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayout dl[3]; VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = n0; dci.pBindings = b0; VKC(vkCreateDescriptorSetLayout(g_dev, &dci, NULL, &dl[0]));
    dci.bindingCount = n1; dci.pBindings = b1; VKC(vkCreateDescriptorSetLayout(g_dev, &dci, NULL, &dl[1]));
    dci.bindingCount = 2; dci.pBindings = b2; VKC(vkCreateDescriptorSetLayout(g_dev, &dci, NULL, &dl[2]));
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 3; pli.pSetLayouts = dl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pc;
    VkPipelineLayout pl; VKC(vkCreatePipelineLayout(g_dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = mod; cpi.stage.pName = "main"; cpi.layout = pl;
    VkPipeline pipe; VkResult pr = vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
    if (pr != VK_SUCCESS) { snprintf(why, 200, "vkCreateComputePipelines %d", pr); return 0; }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pci.maxSets = 3; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    VkDescriptorPool dp; VKC(vkCreateDescriptorPool(g_dev, &pci, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 3; dai.pSetLayouts = dl;
    VkDescriptorSet ds[3]; VKC(vkAllocateDescriptorSets(g_dev, &dai, ds));
    VkWriteDescriptorSet wr[96]; VkDescriptorBufferInfo bi[24]; VkDescriptorImageInfo ii[64]; uint32_t nw = 0, nb = 0;
    for (uint32_t k = 0; k < 18; k++) if (cbufs >> k & 1) {
        bi[nb].buffer = r->cb[k].b; bi[nb].offset = 0; bi[nb].range = VK_WHOLE_SIZE;
        memset(&wr[nw], 0, sizeof wr[nw]); wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[nw].dstSet = ds[0]; wr[nw].dstBinding = 40 + k; wr[nw].descriptorCount = 1; wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[nw].pBufferInfo = &bi[nb]; nw++; nb++;
    }
    for (int k = 0; k < o->nsmp; k++) {
        ii[k].sampler = r->tex[k].smp; ii[k].imageView = r->tex[k].view; ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&wr[nw], 0, sizeof wr[nw]); wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[nw].dstSet = ds[1]; wr[nw].dstBinding = o->smp[k].binding; wr[nw].descriptorCount = 1; wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[nw].pImageInfo = &ii[k]; nw++;
    }
    Buf *io[2] = {&r->in, &r->out};
    for (int k = 0; k < 2; k++) {
        bi[nb].buffer = io[k]->b; bi[nb].offset = 0; bi[nb].range = VK_WHOLE_SIZE;
        memset(&wr[nw], 0, sizeof wr[nw]); wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[nw].dstSet = ds[2]; wr[nw].dstBinding = (uint32_t)k; wr[nw].descriptorCount = 1; wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[nw].pBufferInfo = &bi[nb]; nw++; nb++;
    }
    vkUpdateDescriptorSets(g_dev, nw, wr, 0, NULL);
    memset(r->out.map, 0, r->out.size);
    VkCommandBuffer c = cmd_begin();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 3, ds, 0, NULL);
    uint32_t pcv[4] = {0, 0, 0x3f800000u, 0x3f800000u};          /* alpha test off, point size 1, scale 1 */
    vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pcv);
    vkCmdDispatch(c, W / 8, H / 8, 1);
    cmd_end(c);
    vkDestroyDescriptorPool(g_dev, dp, NULL); vkDestroyPipeline(g_dev, pipe, NULL); vkDestroyPipelineLayout(g_dev, pl, NULL);
    for (int k = 0; k < 3; k++) vkDestroyDescriptorSetLayout(g_dev, dl[k], NULL);
    vkDestroyShaderModule(g_dev, mod, NULL);
    return 1;
}

static void vk_init(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VKC(vkCreateInstance(&ici, NULL, &g_inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(g_inst, &n, &g_gpu);
    vkGetPhysicalDeviceMemoryProperties(g_gpu, &g_mp);
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(g_gpu, &nq, qp);
    for (g_qf = 0; g_qf < nq && !(qp[g_qf].queueFlags & VK_QUEUE_COMPUTE_BIT); g_qf++) {}
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex = g_qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesNV der = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.pNext = &der;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(g_gpu, &f2);
    if (!der.computeDerivativeGroupQuads) { printf("the GPU has no computeDerivativeGroupQuads (VK_NV_compute_shader_derivatives): cannot run this test\n"); exit(2); }
    const char *ext[] = {"VK_NV_compute_shader_derivatives"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = ext;
    VKC(vkCreateDevice(g_gpu, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = g_qf; cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKC(vkCreateCommandPool(g_dev, &cpci, NULL, &g_pool));
}

static int translate(const uint8_t *code, int len, int structured, MxOut *o) {
    MxIn in; memset(&in, 0, sizeof in);
    in.stage = MX_FRAGMENT; in.code = code; in.code_len = len; in.target = 1;
    in.binding_base = 40; in.ssbo_base = 60; in.tex_base = 40; in.image_base = 72; in.wmask_binding = 78;
    _putenv(structured ? "SMO_MX_NOSTRUCT=" : "SMO_MX_NOSTRUCT=1");
    memset(o, 0, sizeof *o);
    return mx_translate(&in, o);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: mx_equiv <folder of vk_prog_*_1.bin> [max]\n"); return 2; }
    int max = argc > 2 ? atoi(argv[2]) : 1 << 30;
    HMODULE m = LoadLibraryA("smo_glslang.dll");
    g_glsl = m ? (GlslFn)GetProcAddress(m, "smo_glsl_to_spirv") : NULL; g_free = m ? (FreeFn)GetProcAddress(m, "smo_spirv_free") : NULL;
    if (!g_glsl) { printf("smo_glslang.dll not found (run from build\\bin)\n"); return 2; }
    vk_init();
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\vk_prog_*_1.bin", argv[1]);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no vk_prog_*_1.bin in %s\n", argv[1]); return 2; }
    int files = 0, same_text = 0, untranslated = 0, skipped = 0, notbuilt = 0, tested = 0, equal = 0, different = 0;
    long long inv_total = 0;
    do {
        if (files >= max) break;
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\%s", argv[1], fd.cFileName);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *code = malloc((size_t)len); fread(code, 1, (size_t)len, f); fclose(f);
        files++;
        MxOut a, b;
        int ta = translate(code, (int)len, 1, &a), tb = translate(code, (int)len, 0, &b);
        if (!ta || !tb) { untranslated++; goto next; }
        if (!strcmp(a.glsl, b.glsl)) { same_text++; goto next; }
        if (a.images || a.ssbos) { skipped++; goto next; }
        {
            int ms = 0; for (int k = 0; k < a.nsmp; k++) if (strstr(a.smp[k].type, "2DMS")) ms = 1;
            if (ms) { skipped++; goto next; }
            uint32_t inmask; char intype[32][8];
            char *ca = to_compute(a.glsl, &inmask, intype), *cb = to_compute(b.glsl, &inmask, intype);
            if (getenv("MX_EQUIV_DUMP")) {                     /* the four texts of each tested stage, for a failing build */
                const char *t[4] = {a.glsl, b.glsl, ca, cb}, *sfx[4] = {"frag_struct", "frag_switch", "comp_struct", "comp_switch"};
                for (int k = 0; k < 4; k++) { char pp[MAX_PATH]; snprintf(pp, sizeof pp, "%s\\%s.%s.glsl", getenv("MX_EQUIV_DUMP"), fd.cFileName, sfx[k]); FILE *df = fopen(pp, "wb"); if (df) { fputs(t[k], df); fclose(df); } }
            }
            Res r; memset(&r, 0, sizeof r);
            g_seed = 0x9e3779b9u ^ (uint32_t)files;
            for (int k = 0; k < 18; k++) if (a.cbufs >> k & 1) {
                r.cb[k] = buf_new(65536);
                for (int q = 0; q < 16384; q++) ((uint32_t *)r.cb[k].map)[q] = (rnd() % 10) ? (union { float f; uint32_t u; }){rndf(-2, 2)}.u : rnd() % 16;
            }
            for (int k = 0; k < a.nsmp; k++) r.tex[k] = make_tex(a.smp[k].type, a.smp[k].shadow);
            r.in = buf_new((VkDeviceSize)NINV * IN_STRIDE * 4); r.out = buf_new((VkDeviceSize)NINV * OUT_STRIDE * 4);
            for (int q = 0; q < NINV * IN_STRIDE; q++) ((uint32_t *)r.in.map)[q] = (union { float f; uint32_t u; }){rndf(-2, 2)}.u;
            uint32_t *outa = malloc((size_t)NINV * OUT_STRIDE * 4);
            char why[200] = "";
            int ok = run(ca, &r, &a, a.cbufs, why);
            if (ok) { memcpy(outa, r.out.map, (size_t)NINV * OUT_STRIDE * 4); ok = run(cb, &r, &b, b.cbufs, why); }
            if (!ok) { notbuilt++; if (notbuilt <= 5) printf("  %s: not built: %s\n", fd.cFileName, why); }
            else {
                const uint32_t *outb = r.out.map;
                long bad = 0, bad_disc = 0;
                for (int v = 0; v < NINV; v++) {
                    const uint32_t *x = outa + v * OUT_STRIDE, *y = outb + v * OUT_STRIDE;
                    if (x[33] != y[33]) { bad_disc++; continue; }
                    if (x[33]) continue;
                    for (int c = 0; c < 33; c++) {
                        if (x[c] == y[c]) continue;
                        float fa, fb; memcpy(&fa, &x[c], 4); memcpy(&fb, &y[c], 4);
                        if (isnan(fa) && isnan(fb)) continue;
                        float mag = fabsf(fa) > fabsf(fb) ? fabsf(fa) : fabsf(fb);
                        if (fabsf(fa - fb) <= 1e-4f * (mag > 1 ? mag : 1)) continue;
                        bad++; break;
                    }
                }
                tested++; inv_total += NINV;
                if (bad || bad_disc) { different++; printf("  DIFFERENT %s: %ld invocations differ in outputs, %ld in discard (of %d)\n", fd.cFileName, bad, bad_disc, NINV); }
                else equal++;
            }
            free(outa); free(ca); free(cb);
            for (int k = 0; k < 18; k++) if (a.cbufs >> k & 1) buf_free(&r.cb[k]);
            for (int k = 0; k < a.nsmp; k++) tex_free(&r.tex[k]);
            buf_free(&r.in); buf_free(&r.out);
        }
    next:
        free(a.glsl); free(b.glsl); free(code);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("mx_equiv: %d fragment stages read; %d translate to the same text in both forms (no jumps or not structured), %d not translated;\n"
           "  %d skipped (images, storage buffers or multisample textures), %d could not be built as compute;\n"
           "  %d TESTED over %lld invocations: %d equivalent, %d DIFFERENT\n", files, same_text, untranslated, skipped, notbuilt, tested, inv_total, equal, different);
    return different ? 1 : 0;
}
