/* GPU cost of a render pass on this Vulkan driver (macOS: KosmicKrisp, MoltenVK). No window. For each case, N passes
 * (vkCmdBeginRendering / vkCmdEndRendering, LOAD and STORE, one vkCmdClearAttachments inside) are recorded into one
 * command buffer between two timestamps; the GPU time per pass is printed. Cases: 128x128 one colour target; 1280x720 one
 * colour; 1280x720 four colour + depth; 128x128 with a full vkCmdPipelineBarrier between passes; LOAD_OP_DONT_CARE.
 * Build: clang -O1 tests/macos/vk_pass_cost.c -I$VULKAN_SDK/include -L$VULKAN_SDK/lib -lvulkan -o build/macos-check/vk_pass_cost */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, (int)r_); exit(1); } } while (0)
static VkInstance inst; static VkPhysicalDevice gpu; static VkDevice dev; static VkQueue q; static uint32_t qf; static VkCommandPool pool; static VkQueryPool qp; static float ts_period;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    return 0;
}
typedef struct { VkImage img; VkImageView view; VkDeviceMemory mem; } Img;
static Img make_img(uint32_t w, uint32_t h, VkFormat f, int depth) {
    Img o; VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT;
    CK(vkCreateImage(dev, &ci, NULL, &o.img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, o.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(dev, &ai, NULL, &o.mem)); CK(vkBindImageMemory(dev, o.img, o.mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    CK(vkCreateImageView(dev, &vi, NULL, &o.view));
    return o;
}
static void transition(VkCommandBuffer cb, VkImage img, int depth, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = to; b.image = img;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
/* one case: n passes; returns GPU ms per pass */
static double run_case(const char *name, uint32_t w, uint32_t h, int ncol, int with_depth, int barrier, int dontcare, int n) {
    Img col[4], dep = {0};
    for (int i = 0; i < ncol; i++) col[i] = make_img(w, h, VK_FORMAT_R8G8B8A8_UNORM, 0);
    if (with_depth) dep = make_img(w, h, VK_FORMAT_D32_SFLOAT, 1);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qp, 0, 2);
    for (int i = 0; i < ncol; i++) transition(cb, col[i].img, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (with_depth) transition(cb, dep.img, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    for (int p = 0; p < n; p++) {
        VkRenderingAttachmentInfo ca[4], da; memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da);
        for (int i = 0; i < ncol; i++) { ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].imageView = col[i].view; ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca[i].loadOp = dontcare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; }
        da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.imageView = dep.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = dontcare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea.extent.width = w; ri.renderArea.extent.height = h; ri.layerCount = 1;
        ri.colorAttachmentCount = (uint32_t)ncol; ri.pColorAttachments = ca; if (with_depth) ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cb, &ri);
        VkClearAttachment cl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0.1f, 0.2f, 0.3f, 1.0f}}}}; VkClearRect cr = {{{0, 0}, {8, 8}}, 0, 1};      /* a tiny clear: a draw stands in */
        vkCmdClearAttachments(cb, 1, &cl, 1, &cr);
        vkCmdEndRendering(cb);
        if (barrier) {
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
    }
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    uint64_t ts[2]; CK(vkGetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    double ms = (double)(ts[1] - ts[0]) * ts_period / 1e6;
    printf("%-52s %4d passes: %8.3f ms GPU, %7.1f us per pass\n", name, n, ms, ms * 1000.0 / n);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    for (int i = 0; i < ncol; i++) { vkDestroyImageView(dev, col[i].view, NULL); vkDestroyImage(dev, col[i].img, NULL); vkFreeMemory(dev, col[i].mem, NULL); }
    if (with_depth) { vkDestroyImageView(dev, dep.view, NULL); vkDestroyImage(dev, dep.img, NULL); vkFreeMemory(dev, dep.mem, NULL); }
    return ms / n;
}
int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    const char *iext[] = {"VK_KHR_portability_enumeration"}; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = iext; ici.flags = 0x1;   /* ENUMERATE_PORTABILITY */
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { ici.enabledExtensionCount = 0; ici.flags = 0; CK(vkCreateInstance(&ici, NULL, &inst)); }
    uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &gpu));
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(gpu, &pr); ts_period = pr.limits.timestampPeriod;
    printf("device: %s (driver %u.%u.%u), timestamp period %g ns\n", pr.deviceName, VK_VERSION_MAJOR(pr.driverVersion), VK_VERSION_MINOR(pr.driverVersion), VK_VERSION_PATCH(pr.driverVersion), ts_period);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL); VkQueueFamilyProperties qfp[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qfp);
    for (qf = 0; qf < nq; qf++) if (qfp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f; VkDeviceQueueCreateInfo dq = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; dq.queueFamilyIndex = qf; dq.queueCount = 1; dq.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &f13; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dq;
    const char *dext[] = {"VK_KHR_portability_subset"}; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
    if (vkCreateDevice(gpu, &dci, NULL, &dev) != VK_SUCCESS) { dci.enabledExtensionCount = 0; CK(vkCreateDevice(gpu, &dci, NULL, &dev)); }
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = qf; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qci.queryType = VK_QUERY_TYPE_TIMESTAMP; qci.queryCount = 2; CK(vkCreateQueryPool(dev, &qci, NULL, &qp));
    run_case("warm-up", 128, 128, 1, 0, 0, 0, 20);
    run_case("128x128, 1 colour, LOAD/STORE", 128, 128, 1, 0, 0, 0, 100);
    run_case("128x128, 1 colour, DONT_CARE load", 128, 128, 1, 0, 0, 1, 100);
    run_case("128x128, 1 colour, + memory barrier between passes", 128, 128, 1, 0, 1, 0, 100);
    run_case("1280x720, 1 colour, LOAD/STORE", 1280, 720, 1, 0, 0, 0, 100);
    run_case("1280x720, 1 colour, DONT_CARE load", 1280, 720, 1, 0, 0, 1, 100);
    run_case("1280x720, 4 colour + depth, LOAD/STORE", 1280, 720, 4, 1, 0, 0, 100);
    run_case("1280x720, 4 colour + depth, DONT_CARE load", 1280, 720, 4, 1, 0, 1, 100);
    run_case("1600x900, 1 colour, LOAD/STORE", 1600, 900, 1, 0, 0, 0, 100);
    return 0;
}
