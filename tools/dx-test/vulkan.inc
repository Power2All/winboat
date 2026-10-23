// Vulkan: the loader, an instance and the GPU's device (in WinBoat Mesa's Venus driver), memory, compute, rendering,
// texture sampling and timeline semaphores, each read back and compared with the expected values, and a swapchain
// presenting to a window. There is no WARP for Vulkan: --warp skips the group.
//
// vulkan-1.dll is loaded at run time, so the test runs (and reports it missing) where there is no Vulkan.

#include "vulkan-spirv.inc"

#define VK_FUNCS(X)                                                                                                    \
   X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices)               \
   X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2)                 \
   X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)                                 \
   X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue)                   \
   X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                         \
   X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateSwapchainKHR)       \
   X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)                 \
   X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage)       \
   X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)   \
   X(vkCreateSampler) X(vkDestroySampler) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory)         \
   X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers)                 \
   X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle)          \
   X(vkDeviceWaitIdle) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)    \
   X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkSignalSemaphore) X(vkGetSemaphoreCounterValue) X(vkCmdFillBuffer) X(vkCmdCopyBuffer)      \
   X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline)                   \
   X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdDraw)             \
   X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)    \
   X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkCreateGraphicsPipelines)      \
   X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)              \
   X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer)                      \
   X(vkDestroyFramebuffer)

struct VkT {
   HMODULE lib = nullptr;
   PFN_vkGetInstanceProcAddr gipa = nullptr;
#define VK_DECLARE(name) PFN_##name name = nullptr;
   VK_FUNCS(VK_DECLARE)
#undef VK_DECLARE
   VkInstance inst = VK_NULL_HANDLE;
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   VkPhysicalDeviceMemoryProperties mem = {};
   VkDevice dev = VK_NULL_HANDLE;
   uint32_t family = 0;
   VkQueue queue = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   bool timeline = false;
};

static void vkLoad(VkT &v, VkInstance inst)
{
#define VK_LOAD(name) v.name = (PFN_##name)v.gipa(inst, #name);
   VK_FUNCS(VK_LOAD)
#undef VK_LOAD
}

static std::string vkr(VkResult r) { return fmt("VkResult %d", (int)r); }

static uint32_t vkMemType(VkT &v, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < v.mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (v.mem.memoryTypes[i].propertyFlags & want) == want) return i;
   return UINT32_MAX;
}

// A buffer with its own memory of the wanted kind
struct VkBuf {
   VkBuffer buf = VK_NULL_HANDLE;
   VkDeviceMemory mem = VK_NULL_HANDLE;
   void *map = nullptr;
};

static bool vkMakeBuffer(VkT &v, VkBuf &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want)
{
   VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   bci.size = size;
   bci.usage = usage;
   if (v.vkCreateBuffer(v.dev, &bci, nullptr, &b.buf)) return false;
   VkMemoryRequirements req;
   v.vkGetBufferMemoryRequirements(v.dev, b.buf, &req);
   VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   mai.allocationSize = req.size;
   mai.memoryTypeIndex = vkMemType(v, req.memoryTypeBits, want);
   if (mai.memoryTypeIndex == UINT32_MAX || v.vkAllocateMemory(v.dev, &mai, nullptr, &b.mem)) return false;
   if (v.vkBindBufferMemory(v.dev, b.buf, b.mem, 0)) return false;
   if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) return v.vkMapMemory(v.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map) == VK_SUCCESS;
   return true;
}

static void vkFreeBuf(VkT &v, VkBuf &b)
{
   if (b.map) v.vkUnmapMemory(v.dev, b.mem);
   if (b.buf) v.vkDestroyBuffer(v.dev, b.buf, nullptr);
   if (b.mem) v.vkFreeMemory(v.dev, b.mem, nullptr);
   b = VkBuf();
}

// A 2D image in device-local memory, with a view
struct VkImg {
   VkImage img = VK_NULL_HANDLE;
   VkDeviceMemory mem = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
};

static bool vkMakeImage(VkT &v, VkImg &i, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage)
{
   VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   ici.imageType = VK_IMAGE_TYPE_2D;
   ici.format = format;
   ici.extent = {w, h, 1};
   ici.mipLevels = ici.arrayLayers = 1;
   ici.samples = VK_SAMPLE_COUNT_1_BIT;
   ici.tiling = VK_IMAGE_TILING_OPTIMAL;
   ici.usage = usage;
   if (v.vkCreateImage(v.dev, &ici, nullptr, &i.img)) return false;
   VkMemoryRequirements req;
   v.vkGetImageMemoryRequirements(v.dev, i.img, &req);
   VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   mai.allocationSize = req.size;
   mai.memoryTypeIndex = vkMemType(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (mai.memoryTypeIndex == UINT32_MAX || v.vkAllocateMemory(v.dev, &mai, nullptr, &i.mem)) return false;
   if (v.vkBindImageMemory(v.dev, i.img, i.mem, 0)) return false;
   VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   vci.image = i.img;
   vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vci.format = format;
   vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   return v.vkCreateImageView(v.dev, &vci, nullptr, &i.view) == VK_SUCCESS;
}

static void vkFreeImg(VkT &v, VkImg &i)
{
   if (i.view) v.vkDestroyImageView(v.dev, i.view, nullptr);
   if (i.img) v.vkDestroyImage(v.dev, i.img, nullptr);
   if (i.mem) v.vkFreeMemory(v.dev, i.mem, nullptr);
   i = VkImg();
}

static VkCommandBuffer vkBegin(VkT &v)
{
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   cai.commandPool = v.pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   VkCommandBuffer cb = VK_NULL_HANDLE;
   if (v.vkAllocateCommandBuffers(v.dev, &cai, &cb)) return VK_NULL_HANDLE;
   VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   v.vkBeginCommandBuffer(cb, &bi);
   return cb;
}

// Ends, submits and waits for a command buffer (10 s at most), then frees it
static bool vkSubmitWait(VkT &v, VkCommandBuffer cb)
{
   v.vkEndCommandBuffer(cb);
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   if (v.vkCreateFence(v.dev, &fci, nullptr, &fence)) return false;
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cb;
   bool ok = v.vkQueueSubmit(v.queue, 1, &si, fence) == VK_SUCCESS &&
             v.vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
   v.vkDestroyFence(v.dev, fence, nullptr);
   v.vkFreeCommandBuffers(v.dev, v.pool, 1, &cb);
   return ok;
}

static void vkBarrier(VkT &v, VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                      VkPipelineStageFlags dst, VkAccessFlags dstAccess)
{
   VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
   mb.srcAccessMask = srcAccess;
   mb.dstAccessMask = dstAccess;
   v.vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

static void vkImageBarrier(VkT &v, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                           VkAccessFlags dstAccess)
{
   VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
   b.srcAccessMask = srcAccess;
   b.dstAccessMask = dstAccess;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = img;
   b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   v.vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static VkShaderModule vkModule(VkT &v, const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   ci.codeSize = size;
   ci.pCode = code;
   VkShaderModule m = VK_NULL_HANDLE;
   v.vkCreateShaderModule(v.dev, &ci, nullptr, &m);
   return m;
}

static VkRenderPass vkColorPass(VkT &v, VkFormat format, VkImageLayout finalLayout)
{
   VkAttachmentDescription att = {};
   att.format = format;
   att.samples = VK_SAMPLE_COUNT_1_BIT;
   att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   att.finalLayout = finalLayout;
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {};
   sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments = &ref;
   VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
   rpci.attachmentCount = 1;
   rpci.pAttachments = &att;
   rpci.subpassCount = 1;
   rpci.pSubpasses = &sub;
   VkRenderPass rp = VK_NULL_HANDLE;
   v.vkCreateRenderPass(v.dev, &rpci, nullptr, &rp);
   return rp;
}

// A pipeline drawing 3 vertices without vertex buffers (the shaders make them up)
static VkPipeline vkPipeline(VkT &v, VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
                             uint32_t w, uint32_t h)
{
   VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vs;
   stages[0].pName = "main";
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = fs;
   stages[1].pName = "main";
   VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
   VkRect2D sc = {{0, 0}, {w, h}};
   VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
   vps.viewportCount = 1;
   vps.pViewports = &vp;
   vps.scissorCount = 1;
   vps.pScissors = &sc;
   VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
   ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState cba = {};
   cba.colorWriteMask = 0xf;
   VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   cb.attachmentCount = 1;
   cb.pAttachments = &cba;
   VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
   gpci.stageCount = 2;
   gpci.pStages = stages;
   gpci.pVertexInputState = &vi;
   gpci.pInputAssemblyState = &ia;
   gpci.pViewportState = &vps;
   gpci.pRasterizationState = &rs;
   gpci.pMultisampleState = &ms;
   gpci.pColorBlendState = &cb;
   gpci.layout = layout;
   gpci.renderPass = rp;
   VkPipeline p = VK_NULL_HANDLE;
   v.vkCreateGraphicsPipelines(v.dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &p);
   return p;
}

static void vkBeginPass(VkT &v, VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h,
                        float r, float g, float b)
{
   VkClearValue clear = {};
   clear.color.float32[0] = r;
   clear.color.float32[1] = g;
   clear.color.float32[2] = b;
   clear.color.float32[3] = 1;
   VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
   rbi.renderPass = rp;
   rbi.framebuffer = fb;
   rbi.renderArea = {{0, 0}, {w, h}};
   rbi.clearValueCount = 1;
   rbi.pClearValues = &clear;
   v.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
}

static VkFramebuffer vkFramebuffer(VkT &v, VkRenderPass rp, VkImageView view, uint32_t w, uint32_t h)
{
   VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
   fci.renderPass = rp;
   fci.attachmentCount = 1;
   fci.pAttachments = &view;
   fci.width = w;
   fci.height = h;
   fci.layers = 1;
   VkFramebuffer fb = VK_NULL_HANDLE;
   v.vkCreateFramebuffer(v.dev, &fci, nullptr, &fb);
   return fb;
}

static void vkTestMemory(VkT &v, const char *name)
{
   const VkDeviceSize size = 64 << 20;
   VkBuf gpu, host;
   if (!vkMakeBuffer(v, gpu, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
       !vkMakeBuffer(v, host, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      check(false, name, "could not allocate the buffers");
      vkFreeBuf(v, gpu);
      vkFreeBuf(v, host);
      return;
   }
   uint32_t *p = (uint32_t *)host.map;
   bool ok = true;
   double best = 1e9;
   LARGE_INTEGER freq;
   QueryPerformanceFrequency(&freq);
   for (int round = 0; round < 8 && ok; round++) {
      const uint32_t pattern = 0xC0FFEE00u + round;
      memset(p, 0, size);
      LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      VkCommandBuffer cb = vkBegin(v);
      v.vkCmdFillBuffer(cb, gpu.buf, 0, size, pattern);
      vkBarrier(v, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
      VkBufferCopy region = {0, 0, size};
      v.vkCmdCopyBuffer(cb, gpu.buf, host.buf, 1, &region);
      vkBarrier(v, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
      ok = vkSubmitWait(v, cb);
      QueryPerformanceCounter(&t1);
      best = std::min(best, (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart);
      for (size_t i = 0; ok && i < size / 4; i++) ok = p[i] == pattern;
   }
   check(ok, name, ok ? fmt("%.0f MB/s", 64 / best) : "wrong data in host-visible memory");
   vkFreeBuf(v, gpu);
   vkFreeBuf(v, host);
}

static void vkTestCompute(VkT &v, const char *name)
{
   const uint32_t count = 1 << 20;
   VkBuf buf;
   if (!vkMakeBuffer(v, buf, count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      check(false, name, "could not allocate the buffer");
      vkFreeBuf(v, buf);
      return;
   }
   memset(buf.map, 0xff, count * 4);
   VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   dlci.bindingCount = 1;
   dlci.pBindings = &b;
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   v.vkCreateDescriptorSetLayout(v.dev, &dlci, nullptr, &dsl);
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   plci.setLayoutCount = 1;
   plci.pSetLayouts = &dsl;
   VkPipelineLayout pl = VK_NULL_HANDLE;
   v.vkCreatePipelineLayout(v.dev, &plci, nullptr, &pl);
   VkShaderModule cs = vkModule(v, spv_comp_comp, sizeof spv_comp_comp);
   VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
   cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   cpci.stage.module = cs;
   cpci.stage.pName = "main";
   cpci.layout = pl;
   VkPipeline pipe = VK_NULL_HANDLE;
   VkResult r = v.vkCreateComputePipelines(v.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
   dpci.maxSets = 1;
   dpci.poolSizeCount = 1;
   dpci.pPoolSizes = &ps;
   VkDescriptorPool dp = VK_NULL_HANDLE;
   v.vkCreateDescriptorPool(v.dev, &dpci, nullptr, &dp);
   VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   dsai.descriptorPool = dp;
   dsai.descriptorSetCount = 1;
   dsai.pSetLayouts = &dsl;
   VkDescriptorSet ds = VK_NULL_HANDLE;
   v.vkAllocateDescriptorSets(v.dev, &dsai, &ds);
   VkDescriptorBufferInfo dbi = {buf.buf, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
   w.dstSet = ds;
   w.descriptorCount = 1;
   w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   w.pBufferInfo = &dbi;
   v.vkUpdateDescriptorSets(v.dev, 1, &w, 0, nullptr);
   bool ok = r == VK_SUCCESS;
   if (ok) {
      VkCommandBuffer cb = vkBegin(v);
      v.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      v.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
      v.vkCmdDispatch(cb, count / 64, 1, 1);
      vkBarrier(v, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
      ok = vkSubmitWait(v, cb);
   }
   size_t bad = 0;
   const uint32_t *p = (const uint32_t *)buf.map;
   for (uint32_t i = 0; ok && i < count; i++) bad += p[i] != i * 3u + 1u;
   check(ok && !bad, name, r ? "pipeline: " + vkr(r) : fmt("1M invocations, %zu wrong", bad));
   if (pipe) v.vkDestroyPipeline(v.dev, pipe, nullptr);
   v.vkDestroyShaderModule(v.dev, cs, nullptr);
   v.vkDestroyDescriptorPool(v.dev, dp, nullptr);
   v.vkDestroyPipelineLayout(v.dev, pl, nullptr);
   v.vkDestroyDescriptorSetLayout(v.dev, dsl, nullptr);
   vkFreeBuf(v, buf);
}

// Draws into a 64x64 RGBA8 target and reads it back; withTexture samples a 2x2 texture over the whole target instead
// of drawing the colored triangle
static void vkTestRender(VkT &v, const char *name, bool withTexture)
{
   const uint32_t w = 64, h = 64;
   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   VkImg target, tex;
   VkBuf readback, staging;
   bool ok = vkMakeImage(v, target, w, h, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
             vkMakeBuffer(v, readback, w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   // Texture: red, green / blue, white (2x2), sampled with nearest filtering: one color per quarter of the target
   const uint32_t texels[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
   if (ok && withTexture) {
      ok = vkMakeImage(v, tex, 2, 2, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
           vkMakeBuffer(v, staging, sizeof texels, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (ok) memcpy(staging.map, texels, sizeof texels);
   }
   VkRenderPass rp = ok ? vkColorPass(v, format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) : VK_NULL_HANDLE;
   VkFramebuffer fb = rp ? vkFramebuffer(v, rp, target.view, w, h) : VK_NULL_HANDLE;
   VkSampler sampler = VK_NULL_HANDLE;
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   VkDescriptorPool dp = VK_NULL_HANDLE;
   VkDescriptorSet ds = VK_NULL_HANDLE;
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   if (ok && withTexture) {
      VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
      sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      v.vkCreateSampler(v.dev, &sci, nullptr, &sampler);
      VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      dlci.bindingCount = 1;
      dlci.pBindings = &b;
      v.vkCreateDescriptorSetLayout(v.dev, &dlci, nullptr, &dsl);
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dpci.maxSets = 1;
      dpci.poolSizeCount = 1;
      dpci.pPoolSizes = &ps;
      v.vkCreateDescriptorPool(v.dev, &dpci, nullptr, &dp);
      VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dsai.descriptorPool = dp;
      dsai.descriptorSetCount = 1;
      dsai.pSetLayouts = &dsl;
      v.vkAllocateDescriptorSets(v.dev, &dsai, &ds);
      VkDescriptorImageInfo dii = {sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr.dstSet = ds;
      wr.descriptorCount = 1;
      wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr.pImageInfo = &dii;
      v.vkUpdateDescriptorSets(v.dev, 1, &wr, 0, nullptr);
      plci.setLayoutCount = 1;
      plci.pSetLayouts = &dsl;
   }
   VkPipelineLayout pl = VK_NULL_HANDLE;
   v.vkCreatePipelineLayout(v.dev, &plci, nullptr, &pl);
   VkShaderModule vs = withTexture ? vkModule(v, spv_tex_vert, sizeof spv_tex_vert) : vkModule(v, spv_tri_vert, sizeof spv_tri_vert);
   VkShaderModule fs = withTexture ? vkModule(v, spv_tex_frag, sizeof spv_tex_frag) : vkModule(v, spv_tri_frag, sizeof spv_tri_frag);
   VkPipeline pipe = ok && fb ? vkPipeline(v, rp, pl, vs, fs, w, h) : VK_NULL_HANDLE;
   std::string detail;
   if (!pipe) {
      ok = false;
      detail = "could not create the target, render pass or pipeline";
   }
   if (ok) {
      VkCommandBuffer cb = vkBegin(v);
      if (withTexture) {
         vkImageBarrier(v, cb, tex.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
         VkBufferImageCopy up = {};
         up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
         up.imageExtent = {2, 2, 1};
         v.vkCmdCopyBufferToImage(cb, staging.buf, tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
         vkImageBarrier(v, cb, tex.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
      }
      vkBeginPass(v, cb, rp, fb, w, h, 0, 0, 0);
      v.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      if (withTexture) v.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, nullptr);
      v.vkCmdDraw(cb, 3, 1, 0, 0);
      v.vkCmdEndRenderPass(cb);
      VkBufferImageCopy region = {};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {w, h, 1};
      v.vkCmdCopyImageToBuffer(cb, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &region);
      vkBarrier(v, cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
      ok = vkSubmitWait(v, cb);
      if (!ok) detail = "the submission failed or timed out";
   }
   if (ok) {
      const uint32_t *px = (const uint32_t *)readback.map;
      auto at = [&](uint32_t x, uint32_t y) { return px[y * w + x]; };
      if (withTexture) {
         // texel (0,0) at the top left of the target (uv 0,0), (1,0) top right, (0,1) bottom left, (1,1) bottom right
         uint32_t tl = at(8, 8), tr = at(56, 8), bl = at(8, 56), br = at(56, 56);
         ok = tl == texels[0] && tr == texels[1] && bl == texels[2] && br == texels[3];
         detail = fmt("quarters %08x %08x %08x %08x", tl, tr, bl, br);
      } else {
         // black corners, a mix of the three vertex colors in the middle, mostly red at the top vertex
         uint32_t corner = at(0, 0), center = at(32, 32), top = at(32, 12);
         auto c = [](uint32_t p, int i) { return (int)((p >> (8 * i)) & 0xff); };
         ok = corner == 0xff000000 && c(center, 0) > 40 && c(center, 1) > 40 && c(center, 2) > 40 && c(top, 0) > 150 &&
              c(top, 1) < 100 && c(top, 2) < 100;
         detail = fmt("corner %08x center %08x top %08x", corner, center, top);
      }
   }
   check(ok, name, detail);
   if (pipe) v.vkDestroyPipeline(v.dev, pipe, nullptr);
   v.vkDestroyShaderModule(v.dev, vs, nullptr);
   v.vkDestroyShaderModule(v.dev, fs, nullptr);
   v.vkDestroyPipelineLayout(v.dev, pl, nullptr);
   if (dp) v.vkDestroyDescriptorPool(v.dev, dp, nullptr);
   if (dsl) v.vkDestroyDescriptorSetLayout(v.dev, dsl, nullptr);
   if (sampler) v.vkDestroySampler(v.dev, sampler, nullptr);
   if (fb) v.vkDestroyFramebuffer(v.dev, fb, nullptr);
   if (rp) v.vkDestroyRenderPass(v.dev, rp, nullptr);
   vkFreeImg(v, target);
   vkFreeImg(v, tex);
   vkFreeBuf(v, readback);
   vkFreeBuf(v, staging);
}

static void vkTestTimeline(VkT &v, const char *name)
{
   if (!v.timeline || !v.vkWaitSemaphores) {
      report(Status::Skip, name, "no timeline semaphores (Vulkan 1.2)");
      return;
   }
   VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
   tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
   tci.initialValue = 1;
   VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   sci.pNext = &tci;
   VkSemaphore sem = VK_NULL_HANDLE;
   VkResult r = v.vkCreateSemaphore(v.dev, &sci, nullptr, &sem);
   uint64_t signal = 5, value = 0;
   if (!r) {
      VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      tsi.signalSemaphoreValueCount = 1;
      tsi.pSignalSemaphoreValues = &signal;
      VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.pNext = &tsi;
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &sem;
      r = v.vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE);
   }
   if (!r) {
      VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
      wi.semaphoreCount = 1;
      wi.pSemaphores = &sem;
      wi.pValues = &signal;
      r = v.vkWaitSemaphores(v.dev, &wi, 10000000000ull);
   }
   if (!r) r = v.vkGetSemaphoreCounterValue(v.dev, sem, &value);
   check(!r && value == 5, name, r ? vkr(r) : fmt("value %llu", (unsigned long long)value));
   v.vkQueueWaitIdle(v.queue);

   // The other way: the GPU waits for a value the CPU signals (vkSignalSemaphore), as ffmpeg's Vulkan filters order
   // their frames. The fence must wait for the signal, then follow it
   const char *name2 = "Timeline semaphores (signalled from the CPU)";
   if (!r && startTest(name2)) {
      VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence = VK_NULL_HANDLE;
      uint64_t wait = 7;
      VkResult early = VK_SUCCESS;
      r = v.vkSignalSemaphore ? v.vkCreateFence(v.dev, &fci, nullptr, &fence) : VK_ERROR_EXTENSION_NOT_PRESENT;
      if (!r) {
         VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
         tsi.waitSemaphoreValueCount = 1;
         tsi.pWaitSemaphoreValues = &wait;
         VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
         si.pNext = &tsi;
         si.waitSemaphoreCount = 1;
         si.pWaitSemaphores = &sem;
         si.pWaitDstStageMask = &stage;
         r = v.vkQueueSubmit(v.queue, 1, &si, fence);
      }
      if (!r) {
         early = v.vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 100000000ull);
         VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
         ssi.semaphore = sem;
         ssi.value = wait;
         r = v.vkSignalSemaphore(v.dev, &ssi);
      }
      if (!r) r = v.vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 5000000000ull);
      check(!r && early == VK_TIMEOUT, name2,
            r ? fmt("%s (the GPU's wait for the CPU's signal)", vkr(r).c_str())
              : early != VK_TIMEOUT ? "the fence signalled before the CPU's signal" : "");
      v.vkQueueWaitIdle(v.queue);
      if (fence) v.vkDestroyFence(v.dev, fence, nullptr);
   }
   if (sem) v.vkDestroySemaphore(v.dev, sem, nullptr);
}

// Presents frames cleared to a known color behind the colored triangle, then checks the color on the desktop
static void vkTestPresent(VkT &v, const char *name, bool haveSwapchain)
{
   if (!haveSwapchain) {
      check(false, name, "no VK_KHR_swapchain");
      return;
   }
   if (!interactiveDesktop()) {
      report(Status::Skip, name, "no interactive desktop");
      return;
   }
   const int W = 320, H = 240;
   HWND hwnd = createWindow("WinBoat Vulkan test", W, H, true);
   VkWin32SurfaceCreateInfoKHR wsci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
   wsci.hinstance = GetModuleHandleA(nullptr);
   wsci.hwnd = hwnd;
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   VkResult r = v.vkCreateWin32SurfaceKHR(v.inst, &wsci, nullptr, &surface);
   VkBool32 supported = VK_FALSE;
   if (!r) v.vkGetPhysicalDeviceSurfaceSupportKHR(v.pd, v.family, surface, &supported);
   VkSurfaceCapabilitiesKHR caps = {};
   VkSurfaceFormatKHR formats[32];
   uint32_t nf = 32;
   if (!r) r = v.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.pd, surface, &caps);
   if (!r) r = v.vkGetPhysicalDeviceSurfaceFormatsKHR(v.pd, surface, &nf, formats);
   VkSurfaceFormatKHR format = formats[0];
   for (uint32_t i = 0; i < nf; i++)
      if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) format = formats[i];
   VkExtent2D extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{(uint32_t)W, (uint32_t)H};
   VkSwapchainKHR sc = VK_NULL_HANDLE;
   if (!r && supported) {
      VkSwapchainCreateInfoKHR swci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
      swci.surface = surface;
      swci.minImageCount = std::max(caps.minImageCount, 2u);
      if (caps.maxImageCount) swci.minImageCount = std::min(swci.minImageCount, caps.maxImageCount);
      swci.imageFormat = format.format;
      swci.imageColorSpace = format.colorSpace;
      swci.imageExtent = extent;
      swci.imageArrayLayers = 1;
      swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      swci.preTransform = caps.currentTransform;
      swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      swci.clipped = VK_TRUE;
      r = v.vkCreateSwapchainKHR(v.dev, &swci, nullptr, &sc);
   }
   std::string detail;
   bool ok = !r && supported;
   VkImage images[8];
   uint32_t ni = 8;
   VkImageView views[8] = {};
   VkFramebuffer fbs[8] = {};
   VkRenderPass rp = VK_NULL_HANDLE;
   VkPipelineLayout pl = VK_NULL_HANDLE;
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   if (ok) ok = v.vkGetSwapchainImagesKHR(v.dev, sc, &ni, images) >= VK_SUCCESS;
   if (ok) {
      rp = vkColorPass(v, format.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
      VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      v.vkCreatePipelineLayout(v.dev, &plci, nullptr, &pl);
      vs = vkModule(v, spv_tri_vert, sizeof spv_tri_vert);
      fs = vkModule(v, spv_tri_frag, sizeof spv_tri_frag);
      pipe = vkPipeline(v, rp, pl, vs, fs, extent.width, extent.height);
      for (uint32_t i = 0; i < ni; i++) {
         VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
         vci.image = images[i];
         vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
         vci.format = format.format;
         vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         v.vkCreateImageView(v.dev, &vci, nullptr, &views[i]);
         fbs[i] = vkFramebuffer(v, rp, views[i], extent.width, extent.height);
      }
      ok = pipe != VK_NULL_HANDLE;
   }
   int frames = 0;
   double seconds = 0;
   if (ok) {
      VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore acquired, rendered;
      v.vkCreateSemaphore(v.dev, &semci, nullptr, &acquired);
      v.vkCreateSemaphore(v.dev, &semci, nullptr, &rendered);
      VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      VkFence fence;
      v.vkCreateFence(v.dev, &fci, nullptr, &fence);
      VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = v.pool;
      cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cai.commandBufferCount = 1;
      VkCommandBuffer cb;
      v.vkAllocateCommandBuffers(v.dev, &cai, &cb);
      LARGE_INTEGER freq, t0, t1;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&t0);
      for (; frames < 120 && ok; frames++) {
         pumpMessages();
         ok = v.vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS;
         v.vkResetFences(v.dev, 1, &fence);
         uint32_t idx = 0;
         r = ok ? v.vkAcquireNextImageKHR(v.dev, sc, 10000000000ull, acquired, VK_NULL_HANDLE, &idx) : VK_ERROR_UNKNOWN;
         if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            ok = false;
            detail = "acquire: " + vkr(r);
            break;
         }
         v.vkResetCommandBuffer(cb, 0);
         VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
         v.vkBeginCommandBuffer(cb, &bi);
         // a dark cyan background (R 0x20, G 0x80, B 0xA0)
         vkBeginPass(v, cb, rp, fbs[idx], extent.width, extent.height, 0x20 / 255.0f, 0x80 / 255.0f, 0xA0 / 255.0f);
         v.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         v.vkCmdDraw(cb, 3, 1, 0, 0);
         v.vkCmdEndRenderPass(cb);
         v.vkEndCommandBuffer(cb);
         VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
         VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
         si.waitSemaphoreCount = 1;
         si.pWaitSemaphores = &acquired;
         si.pWaitDstStageMask = &waitStage;
         si.commandBufferCount = 1;
         si.pCommandBuffers = &cb;
         si.signalSemaphoreCount = 1;
         si.pSignalSemaphores = &rendered;
         ok = v.vkQueueSubmit(v.queue, 1, &si, fence) == VK_SUCCESS;
         VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
         pi.waitSemaphoreCount = 1;
         pi.pWaitSemaphores = &rendered;
         pi.swapchainCount = 1;
         pi.pSwapchains = &sc;
         pi.pImageIndices = &idx;
         r = v.vkQueuePresentKHR(v.queue, &pi);
         if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            ok = false;
            detail = "present: " + vkr(r);
         }
      }
      v.vkDeviceWaitIdle(v.dev);
      QueryPerformanceCounter(&t1);
      seconds = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
      v.vkFreeCommandBuffers(v.dev, v.pool, 1, &cb);
      v.vkDestroyFence(v.dev, fence, nullptr);
      v.vkDestroySemaphore(v.dev, acquired, nullptr);
      v.vkDestroySemaphore(v.dev, rendered, nullptr);
   }
   if (ok) {
      Sleep(200);
      pumpMessages();
      // the background in a corner; the triangle's middle (all three colors) and, right under its top vertex, red:
      // a frame shown upside down or shifted fails
      COLORREF c = 0, mid = 0, top = 0;
      const int w = (int)extent.width, h = (int)extent.height;
      bool got = windowPixel(hwnd, 4, 4, c) && windowPixel(hwnd, w / 2, h / 2, mid) &&
                 windowPixel(hwnd, w / 2, h / 8 + 6, top);
      ok = got && near8(GetRValue(c), 0x20, 8) && near8(GetGValue(c), 0x80, 8) && near8(GetBValue(c), 0xA0, 8) &&
           GetRValue(mid) > 40 && GetGValue(mid) > 40 && GetBValue(mid) > 40 && GetRValue(top) > 150 &&
           GetGValue(top) < 100 && GetBValue(top) < 100;
      detail = fmt("%d frames %ux%u, %.0f fps; window corner %s, middle %s, top %s", frames, extent.width, extent.height,
                   frames / seconds, fmt("%06x", (unsigned)c).c_str(), fmt("%06x", (unsigned)mid).c_str(),
                   fmt("%06x", (unsigned)top).c_str());
      if (!got) detail += " (window unreadable)";
   } else if (detail.empty()) {
      detail = r ? vkr(r) : supported ? "could not set up the swapchain" : "the queue can't present to the window";
   }
   check(ok, name, detail);
   if (pipe) v.vkDestroyPipeline(v.dev, pipe, nullptr);
   if (vs) v.vkDestroyShaderModule(v.dev, vs, nullptr);
   if (fs) v.vkDestroyShaderModule(v.dev, fs, nullptr);
   if (pl) v.vkDestroyPipelineLayout(v.dev, pl, nullptr);
   for (uint32_t i = 0; i < 8; i++) {
      if (fbs[i]) v.vkDestroyFramebuffer(v.dev, fbs[i], nullptr);
      if (views[i]) v.vkDestroyImageView(v.dev, views[i], nullptr);
   }
   if (rp) v.vkDestroyRenderPass(v.dev, rp, nullptr);
   if (sc) v.vkDestroySwapchainKHR(v.dev, sc, nullptr);
   if (surface) v.vkDestroySurfaceKHR(v.inst, surface, nullptr);
   DestroyWindow(hwnd);
}

static void groupVulkan()
{
   const char *TESTS[] = {"Vulkan device on the GPU", "Vulkan 1.1 or higher", "Device-local and host-visible memory",
                          "Create a logical device", "Host-visible memory: GPU fill and copy (64 MB)",
                          "Compute shaders and storage buffers", "Rendering a triangle",
                          "Image upload and texture sampling", "Timeline semaphores", "Presenting to a window (swapchain)"};
   if (g_warp) {
      for (auto t : TESTS) report(Status::Skip, t, "no WARP for Vulkan");
      return;
   }
   VkT v;
   v.lib = LoadLibraryA("vulkan-1.dll");
   v.gipa = v.lib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(v.lib, "vkGetInstanceProcAddr") : nullptr;
   if (v.gipa || windows10OrLater()) check(v.gipa != nullptr, "Vulkan loader (vulkan-1.dll)", v.gipa ? "" : "not installed");
   else optional(false, "Vulkan loader (vulkan-1.dll)", "not installed (the Vulkan driver needs Windows 10 or later)");
   if (!v.gipa) {
      for (auto t : TESTS) report(Status::Skip, t, "no Vulkan loader");
      return;
   }
   vkLoad(v, VK_NULL_HANDLE);
   uint32_t loaderVersion = VK_API_VERSION_1_0;
   if (v.vkEnumerateInstanceVersion) v.vkEnumerateInstanceVersion(&loaderVersion);
   report(Status::Info, "Vulkan loader version", fmt("%u.%u.%u", VK_API_VERSION_MAJOR(loaderVersion),
                                                     VK_API_VERSION_MINOR(loaderVersion), VK_API_VERSION_PATCH(loaderVersion)));

   const char *instanceExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
   VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   ai.pApplicationName = "winboat-dxtest";
   ai.apiVersion = loaderVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_0;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &ai;
   ici.enabledExtensionCount = 2;
   ici.ppEnabledExtensionNames = instanceExts;
   VkResult r = v.vkCreateInstance(&ici, nullptr, &v.inst);
   if (r == VK_ERROR_EXTENSION_NOT_PRESENT) {
      ici.enabledExtensionCount = 0;
      r = v.vkCreateInstance(&ici, nullptr, &v.inst);
   }
   if (r == VK_ERROR_INCOMPATIBLE_DRIVER) missing("Create a Vulkan instance", vkr(r) + " (no Vulkan driver)");
   else check(r == VK_SUCCESS, "Create a Vulkan instance", r ? vkr(r) : "");
   if (r) {
      for (auto t : TESTS) report(Status::Skip, t, "no Vulkan instance");
      return;
   }
   vkLoad(v, v.inst);

   // The GPU: the first device that isn't a CPU (software) one, nor Dozen (Vulkan on Direct3D 12, from Microsoft's
   // compatibility pack, which would run on the GPU's own Direct3D 12)
   uint32_t n = 0;
   v.vkEnumeratePhysicalDevices(v.inst, &n, nullptr);
   std::vector<VkPhysicalDevice> pds(n);
   if (n) v.vkEnumeratePhysicalDevices(v.inst, &n, pds.data());
   VkPhysicalDeviceProperties props = {};
   std::string devices;
   static const char *TYPES[] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "CPU"};
   for (auto pd : pds) {
      VkPhysicalDeviceProperties p;
      v.vkGetPhysicalDeviceProperties(pd, &p);
      bool dozen = strncmp(p.deviceName, "Microsoft Direct3D12", 20) == 0;
      devices += std::string(devices.empty() ? "" : ", ") + p.deviceName + " (" + TYPES[std::min<int>(p.deviceType, 4)] + ")";
      if (!v.pd && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && !dozen) {
         v.pd = pd;
         props = p;
      }
   }
   report(Status::Info, "Vulkan devices", devices.empty() ? "none" : devices);
   if (v.pd) check(true, TESTS[0], props.deviceName);
   else missing(TESTS[0], n ? "only software devices: " + devices : "no devices: the GPU has no Vulkan");
   if (!v.pd) {
      for (size_t i = 1; i < ARRAYSIZE(TESTS); i++) report(Status::Skip, TESTS[i], "no Vulkan GPU");
      v.vkDestroyInstance(v.inst, nullptr);
      return;
   }
   if (props.apiVersion >= VK_API_VERSION_1_2 && v.vkGetPhysicalDeviceProperties2) {
      VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p2.pNext = &drv;
      v.vkGetPhysicalDeviceProperties2(v.pd, &p2);
      report(Status::Info, "Vulkan driver", fmt("%s (%s)", drv.driverName, drv.driverInfo));
   }
   check(props.apiVersion >= VK_API_VERSION_1_1, TESTS[1], fmt("Vulkan %u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion),
                                                               VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion)));
   optional(props.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3");

   v.vkGetPhysicalDeviceMemoryProperties(v.pd, &v.mem);
   bool deviceLocal = false, hostVisible = false;
   VkDeviceSize vram = 0;
   for (uint32_t i = 0; i < v.mem.memoryTypeCount; i++) {
      deviceLocal |= (v.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
      hostVisible |= (v.mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   }
   for (uint32_t i = 0; i < v.mem.memoryHeapCount; i++)
      if (v.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, v.mem.memoryHeaps[i].size);
   check(deviceLocal && hostVisible, TESTS[2], fmt("%u memory types, largest device-local heap %llu MB", v.mem.memoryTypeCount,
                                                  (unsigned long long)(vram >> 20)));

   // Features and extensions
   uint32_t ne = 0;
   v.vkEnumerateDeviceExtensionProperties(v.pd, nullptr, &ne, nullptr);
   std::vector<VkExtensionProperties> exts(ne);
   if (ne) v.vkEnumerateDeviceExtensionProperties(v.pd, nullptr, &ne, exts.data());
   auto hasExt = [&](const char *name) {
      for (auto &e : exts)
         if (!strcmp(e.extensionName, name)) return true;
      return false;
   };
   VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   if (props.apiVersion >= VK_API_VERSION_1_2) f2.pNext = &f12;
   if (v.vkGetPhysicalDeviceFeatures2) v.vkGetPhysicalDeviceFeatures2(v.pd, &f2);
   optional(f2.features.geometryShader, "Geometry shaders");
   optional(f2.features.tessellationShader, "Tessellation shaders");
   optional(f2.features.shaderFloat64, "64-bit floats in shaders");
   optional(f2.features.textureCompressionBC, "BC texture compression");
   optional(f12.descriptorIndexing, "Descriptor indexing");
   optional(hasExt(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) || props.apiVersion >= VK_API_VERSION_1_3, "Dynamic rendering");
   optional(hasExt("VK_KHR_ray_tracing_pipeline"), "Ray tracing pipelines (VK_KHR_ray_tracing_pipeline)");
   optional(hasExt("VK_EXT_mesh_shader"), "Mesh shaders (VK_EXT_mesh_shader)");
   optional(hasExt("VK_KHR_video_decode_queue"), "Video decoding (VK_KHR_video_decode_queue)");
   optional(hasExt("VK_KHR_video_encode_queue"), "Video encoding (VK_KHR_video_encode_queue)");
   report(Status::Info, "Device extensions", fmt("%u", ne));

   // A queue that does graphics and compute
   uint32_t nq = 0;
   v.vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nq, nullptr);
   std::vector<VkQueueFamilyProperties> qf(nq);
   if (nq) v.vkGetPhysicalDeviceQueueFamilyProperties(v.pd, &nq, qf.data());
   v.family = UINT32_MAX;
   for (uint32_t i = 0; i < nq && v.family == UINT32_MAX; i++)
      if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) v.family = i;
   // DXTEST_VK_FAMILY=<n>: another queue family (investigating a driver: compute on a compute-only family)
   if (const char *f = getenv("DXTEST_VK_FAMILY")) {
      uint32_t i = (uint32_t)atoi(f);
      if (i < nq) v.family = i;
      report(Status::Info, "Queue family", fmt("%u (DXTEST_VK_FAMILY)", v.family));
   }
   float priority = 1;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = v.family;
   qci.queueCount = 1;
   qci.pQueuePriorities = &priority;
   const bool swapchain = hasExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
   const char *deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   VkPhysicalDeviceVulkan12Features enable12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   enable12.timelineSemaphore = f12.timelineSemaphore;
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   if (props.apiVersion >= VK_API_VERSION_1_2 && ai.apiVersion >= VK_API_VERSION_1_2) dci.pNext = &enable12;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.enabledExtensionCount = swapchain ? 1 : 0;
   dci.ppEnabledExtensionNames = deviceExts;
   r = v.family != UINT32_MAX ? v.vkCreateDevice(v.pd, &dci, nullptr, &v.dev) : VK_ERROR_FEATURE_NOT_PRESENT;
   check(r == VK_SUCCESS, TESTS[3], r ? (v.family == UINT32_MAX ? "no graphics and compute queue" : vkr(r)) : "");
   if (r) {
      for (size_t i = 4; i < ARRAYSIZE(TESTS); i++) report(Status::Skip, TESTS[i], "no Vulkan device");
      v.vkDestroyInstance(v.inst, nullptr);
      return;
   }
   v.timeline = dci.pNext && enable12.timelineSemaphore;
   v.vkGetDeviceQueue(v.dev, v.family, 0, &v.queue);
   VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cpci.queueFamilyIndex = v.family;
   v.vkCreateCommandPool(v.dev, &cpci, nullptr, &v.pool);

   if (startTest(TESTS[4])) vkTestMemory(v, TESTS[4]);
   if (startTest(TESTS[5])) vkTestCompute(v, TESTS[5]);
   if (startTest(TESTS[6])) vkTestRender(v, TESTS[6], false);
   if (startTest(TESTS[7])) vkTestRender(v, TESTS[7], true);
   if (startTest(TESTS[8])) vkTestTimeline(v, TESTS[8]);
   if (startTest(TESTS[9])) vkTestPresent(v, TESTS[9], swapchain);

   v.vkDestroyCommandPool(v.dev, v.pool, nullptr);
   v.vkDestroyDevice(v.dev, nullptr);
   v.vkDestroyInstance(v.inst, nullptr);
}
