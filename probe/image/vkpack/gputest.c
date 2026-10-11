// gputest - 1401 Probe: does this GPU run Vulkan work correctly under Linux's own driver (Mesa ANV / RADV / NVK)?
//
// For every Vulkan device (or only the one named by GPUTEST_ONLY=<vendor>:<device>, hex) it runs two tests and prints
// one JSON object per device on stdout:
//   compute  a 1 Mi-element buffer filled by a compute shader, every element checked on the CPU
//   render   a 256x256 RGBA8 offscreen draw (no window system, no display), copied to a buffer, every pixel checked
// With Mesa's dump switches (INTEL_DEBUG=bat, RADV_DEBUG=dumpibs) and drmlog.so preloaded, the same run records the
// exact command streams and kernel calls the driver used - the reference a macOS driver for this GPU must reproduce.
// Nothing is drawn on the screen and nothing is written outside stdout/stderr.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "shaders.h"

#define N (1u << 20)
#define W 256u
#define H 256u

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void jstr(const char *k, const char *v, int comma) {
    printf("\"%s\":\"", k);
    for (const char *p = v; p && *p; p++) { if (*p == '"' || *p == '\\') putchar('\\'); if ((unsigned char)*p >= 0x20) putchar(*p); }
    printf("\"%s", comma ? "," : "");
}

static int mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static VkResult buffer(VkPhysicalDevice pd, VkDevice d, VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkResult r = vkCreateBuffer(d, &bi, NULL, b); if (r) return r;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(d, *b, &rq);
    int t = mem_type(pd, rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, (uint32_t)t };
    if ((r = vkAllocateMemory(d, &ai, NULL, m))) return r;
    if ((r = vkBindBufferMemory(d, *b, *m, 0))) return r;
    return vkMapMemory(d, *m, 0, size, 0, map);
}
static VkShaderModule module(VkDevice d, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code };
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(d, &ci, NULL, &m); return m;
}
static VkResult submit_wait(VkDevice d, VkQueue q, VkCommandBuffer cb) {
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 }; VkFence f;
    VkResult r = vkCreateFence(d, &fi, NULL, &f); if (r) return r;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    if (!(r = vkQueueSubmit(q, 1, &si, f))) r = vkWaitForFences(d, 1, &f, VK_TRUE, 10ull * 1000 * 1000 * 1000);  // 10 s: a hang is a result
    vkDestroyFence(d, f, NULL);
    return r;
}

// compute: returns the number of wrong elements (0 = pass), or -VkResult on an API failure
static long test_compute(VkPhysicalDevice pd, VkDevice d, VkQueue q, VkCommandPool pool, double *ms) {
    VkBuffer b; VkDeviceMemory m; uint32_t *v; VkResult r;
    if ((r = buffer(pd, d, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &b, &m, (void **)&v))) return -(long)(r ? r : 1);
    memset(v, 0, N * 4);
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb }; VkDescriptorSetLayout dl;
    vkCreateDescriptorSetLayout(d, &li, NULL, &dl);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dl, 0, NULL }; VkPipelineLayout pl;
    vkCreatePipelineLayout(d, &pli, NULL, &pl);
    VkShaderModule sm = module(d, compute_spv, sizeof compute_spv);
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL }, pl, VK_NULL_HANDLE, -1 };
    VkPipeline p; if ((r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &p))) return -(long)r;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps }; VkDescriptorPool dp;
    vkCreateDescriptorPool(d, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dl }; VkDescriptorSet ds;
    vkAllocateDescriptorSets(d, &dai, &ds);
    VkDescriptorBufferInfo dbi = { b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
    vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb;
    vkAllocateCommandBuffers(d, &cai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cb, &bi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    double t0 = now_ms();
    if ((r = submit_wait(d, q, cb))) return -(long)r;
    *ms = now_ms() - t0;
    long bad = 0;
    for (uint32_t i = 0; i < N; i++) bad += v[i] != i * 3u + 7u;
    return bad;
}

// render: returns the number of wrong pixels (0 = pass), or -VkResult on an API failure
static long test_render(VkPhysicalDevice pd, VkDevice d, VkQueue q, VkCommandPool pool, double *ms) {
    VkResult r; VkImage img; VkDeviceMemory im; VkImageView iv;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { W, H, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    if ((r = vkCreateImage(d, &ici, NULL, &img))) return -(long)r;
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(d, img, &rq);
    int t = mem_type(pd, rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (t < 0) t = mem_type(pd, rq.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, (uint32_t)t };
    if ((r = vkAllocateMemory(d, &ai, NULL, &im)) || (r = vkBindImageMemory(d, img, im, 0))) return -(long)r;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
        { 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCreateImageView(d, &vci, NULL, &iv);
    VkAttachmentDescription ad = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 0, NULL }; VkRenderPass rp;
    if ((r = vkCreateRenderPass(d, &rpi, NULL, &rp))) return -(long)r;
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &iv, W, H, 1 }; VkFramebuffer fb;
    vkCreateFramebuffer(d, &fbi, NULL, &fb);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL }; VkPipelineLayout pl;
    vkCreatePipelineLayout(d, &pli, NULL, &pl);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, module(d, tri_vert_spv, sizeof tri_vert_spv), "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, module(d, tri_frag_spv, sizeof tri_frag_spv), "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vs = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
        VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1 };
    VkPipelineMultisampleStateCreateInfo ms_ = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE };
    VkPipelineColorBlendAttachmentState cba = { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xF };
    VkPipelineColorBlendStateCreateInfo cb_ = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &cba, { 0, 0, 0, 0 } };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vs, &rs, &ms_, NULL, &cb_, NULL,
        pl, rp, 0, VK_NULL_HANDLE, -1 };
    VkPipeline p; if ((r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, NULL, &p))) return -(long)r;
    VkBuffer b; VkDeviceMemory bm; uint8_t *px;
    if ((r = buffer(pd, d, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &bm, (void **)&px))) return -(long)r;
    memset(px, 0, W * H * 4);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 }; VkCommandBuffer cb;
    vkAllocateCommandBuffers(d, &cai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue cv = { .color = { .float32 = { 1, 0, 1, 1 } } };  // magenta: a pixel the draw missed shows up as wrong
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, { { 0, 0 }, { W, H } }, 1, &cv };
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy bic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &bic);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    double t0 = now_ms();
    if ((r = submit_wait(d, q, cb))) return -(long)r;
    *ms = now_ms() - t0;
    long bad = 0;  // 0.25/0.5/0.75 in UNORM8 = 64, 128, 191 (+-1 for rounding)
    for (uint32_t i = 0; i < W * H; i++) {
        const uint8_t *c = px + i * 4;
        bad += abs(c[0] - 64) > 1 || abs(c[1] - 128) > 1 || abs(c[2] - 191) > 1 || c[3] != 255;
    }
    return bad;
}

int main(void) {
    const char *only = getenv("GPUTEST_ONLY");
    unsigned ov = 0, od = 0; if (only) sscanf(only, "%x:%x", &ov, &od);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "1401-gputest", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst; VkResult r = vkCreateInstance(&ii, NULL, &inst);
    if (r) { printf("{\"error\":\"vkCreateInstance %d\"}\n", r); return 2; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[16]; if (n > 16) n = 16; vkEnumeratePhysicalDevices(inst, &n, pds);
    int fails = 0;
    for (uint32_t k = 0; k < n; k++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[k], &pp);
        if (only && (pp.vendorID != ov || pp.deviceID != od)) continue;
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !getenv("GPUTEST_CPU")) continue;  // lavapipe is the control, not a GPU
        printf("{\"vendor\":\"%04x\",\"device\":\"%04x\",\"api\":\"%u.%u.%u\",\"driver_version\":%u,",
               pp.vendorID, pp.deviceID, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion), pp.driverVersion);
        jstr("name", pp.deviceName, 1);
        fflush(stdout);
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[k], &qn, NULL);
        VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(pds[k], &qn, qf);
        int fam = -1; for (uint32_t i = 0; i < qn; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { fam = (int)i; break; }
        if (fam < 0) { printf("\"error\":\"no graphics+compute queue\"}\n"); fails++; continue; }
        float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, (uint32_t)fam, 1, &prio };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
        VkDevice d; if ((r = vkCreateDevice(pds[k], &dci, NULL, &d))) { printf("\"error\":\"vkCreateDevice %d\"}\n", r); fails++; continue; }
        VkQueue q; vkGetDeviceQueue(d, (uint32_t)fam, 0, &q);
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, (uint32_t)fam }; VkCommandPool pool;
        vkCreateCommandPool(d, &pci, NULL, &pool);
        double cms = 0, rms = 0;
        long c = test_compute(pds[k], d, q, pool, &cms);
        printf("\"compute\":{\"wrong\":%ld,\"of\":%u,\"ms\":%.2f},", c, N, cms); fflush(stdout);
        long g = test_render(pds[k], d, q, pool, &rms);
        printf("\"render\":{\"wrong\":%ld,\"of\":%u,\"ms\":%.2f},", g, W * H, rms);
        printf("\"pass\":%s}\n", c == 0 && g == 0 ? "true" : "false"); fflush(stdout);
        fails += !(c == 0 && g == 0);
        vkDeviceWaitIdle(d);
    }
    return fails ? 1 : 0;
}
