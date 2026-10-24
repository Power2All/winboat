// vkpresentbench <width> <height> <seconds> [fifo]: presentbench for Vulkan: a borderless window of that size, each
// swapchain image cleared to a changing color and presented (IMMEDIATE, else MAILBOX or FIFO; 3 images,
// B8G8R8A8_UNORM) as fast as it goes; prints frames per second and the average time in vkQueuePresentKHR.
// Build: x86_64-w64-mingw32-g++ -O2 -static -I<Vulkan-Headers>/include vkpresentbench.cpp -o vkpresentbench.exe -luser32
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

#define F(n) static PFN_##n n;
F(vkCreateInstance) F(vkEnumeratePhysicalDevices) F(vkCreateDevice) F(vkGetDeviceQueue) F(vkCreateWin32SurfaceKHR)
F(vkGetPhysicalDeviceSurfacePresentModesKHR) F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) F(vkCreateSwapchainKHR)
F(vkGetSwapchainImagesKHR) F(vkAcquireNextImageKHR) F(vkQueuePresentKHR) F(vkCreateCommandPool)
F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) F(vkCmdPipelineBarrier)
F(vkCmdClearColorImage) F(vkQueueSubmit) F(vkCreateSemaphore) F(vkCreateFence) F(vkWaitForFences) F(vkResetFences)
F(vkDeviceWaitIdle) F(vkDestroySwapchainKHR) F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkResetCommandBuffer) F(vkGetPhysicalDeviceProperties)

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("%s: %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
   if (argc < 4) { printf("usage: vkpresentbench <width> <height> <seconds> [fifo]\n"); return 2; }
   UINT w = atoi(argv[1]), h = atoi(argv[2]);
   double seconds = atof(argv[3]);
   bool fifo = argc > 4, still = getenv("VKPB_STILL"), resize = getenv("VKPB_RESIZE");
   HMODULE vk = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
   vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
   const char *iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "vkpresentbench", 1, nullptr, 0, VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext};
   VkInstance inst;
   CK(vkCreateInstance(&ici, nullptr, &inst));
#define L(n) n = (PFN_##n)gipa(inst, #n);
   L(vkEnumeratePhysicalDevices) L(vkCreateDevice) L(vkGetDeviceQueue) L(vkCreateWin32SurfaceKHR)
   L(vkGetPhysicalDeviceSurfacePresentModesKHR) L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) L(vkCreateSwapchainKHR)
   L(vkGetSwapchainImagesKHR) L(vkAcquireNextImageKHR) L(vkQueuePresentKHR) L(vkCreateCommandPool)
   L(vkAllocateCommandBuffers) L(vkBeginCommandBuffer) L(vkEndCommandBuffer) L(vkCmdPipelineBarrier)
   L(vkCmdClearColorImage) L(vkQueueSubmit) L(vkCreateSemaphore) L(vkCreateFence) L(vkWaitForFences) L(vkResetFences)
   L(vkDeviceWaitIdle) L(vkDestroySwapchainKHR) L(vkGetPhysicalDeviceQueueFamilyProperties) L(vkResetCommandBuffer) L(vkGetPhysicalDeviceProperties)
   // the device whose name has VKPB_DEVICE (default "Venus"), else the first
   uint32_t n = 0;
   vkEnumeratePhysicalDevices(inst, &n, nullptr);
   std::vector<VkPhysicalDevice> pds(n);
   vkEnumeratePhysicalDevices(inst, &n, pds.data());
   const char *want = getenv("VKPB_DEVICE") ? getenv("VKPB_DEVICE") : "Venus";
   VkPhysicalDevice pd = pds[0];
   VkPhysicalDeviceProperties props;
   for (VkPhysicalDevice d : pds) {
      vkGetPhysicalDeviceProperties(d, &props);
      if (strstr(props.deviceName, want)) { pd = d; break; }
   }
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("device: %s (%u devices)\n", props.deviceName, n);

   WNDCLASSA wc = {};
   wc.lpfnWndProc = proc;
   wc.lpszClassName = "vkpb";
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("vkpb", "vkpresentbench", WS_POPUP | WS_VISIBLE, 0, 0, w, h, nullptr, nullptr, nullptr, nullptr);
   VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleA(nullptr), hwnd};
   VkSurfaceKHR surf;
   CK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surf));

   float prio = 1;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
   const char *dext[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, dext};
   VkDevice dev;
   CK(vkCreateDevice(pd, &dci, nullptr, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   uint32_t pmn = 8;
   VkPresentModeKHR pms[8];
   vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &pmn, pms);
   VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
   for (int want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
      for (uint32_t i = 0; i < pmn && !fifo; i++) if (pms[i] == want) mode = pms[i];
   VkSurfaceCapabilitiesKHR caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
   VkSwapchainCreateInfoKHR swci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
   swci.surface = surf;
   swci.minImageCount = 3;
   swci.imageFormat = getenv("VKPB_SRGB") ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
   swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   swci.imageExtent = {w, h};
   swci.imageArrayLayers = 1;
   swci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   swci.preTransform = caps.currentTransform;
   swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   swci.presentMode = mode;
   swci.clipped = VK_TRUE;
   VkSwapchainKHR sc;
   CK(vkCreateSwapchainKHR(dev, &swci, nullptr, &sc));
   uint32_t ni = 0;
   vkGetSwapchainImagesKHR(dev, sc, &ni, nullptr);
   std::vector<VkImage> imgs(ni);
   vkGetSwapchainImagesKHR(dev, sc, &ni, imgs.data());

   VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
   VkCommandPool pool;
   vkCreateCommandPool(dev, &pci, nullptr, &pool);
   const int FR = 2;
   VkCommandBuffer cbs[FR];
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, FR};
   vkAllocateCommandBuffers(dev, &cai, cbs);
   VkSemaphore acq[FR], done[FR];
   VkFence fences[FR];
   VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
   for (int i = 0; i < FR; i++) {
      vkCreateSemaphore(dev, &semi, nullptr, &acq[i]);
      vkCreateSemaphore(dev, &semi, nullptr, &done[i]);
      vkCreateFence(dev, &fi, nullptr, &fences[i]);
   }

   LARGE_INTEGER freq, t0, t, a, b;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t0);
   double presentTime = 0;
   long frames = 0;
   for (;;) {
      MSG msg;
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
      QueryPerformanceCounter(&t);
      if ((t.QuadPart - t0.QuadPart) / (double)freq.QuadPart >= seconds) break;
      // VKPB_RESIZE: halfway, the window and a new swapchain (oldSwapchain: the first) at half the size
      if (resize && (t.QuadPart - t0.QuadPart) / (double)freq.QuadPart >= seconds / 2) {
         resize = false;
         vkDeviceWaitIdle(dev);
         SetWindowPos(hwnd, nullptr, 0, 0, w / 2, h / 2, SWP_NOZORDER | SWP_NOMOVE);
         swci.imageExtent = {w / 2, h / 2};
         swci.oldSwapchain = sc;
         CK(vkCreateSwapchainKHR(dev, &swci, nullptr, &sc));
         vkDestroySwapchainKHR(dev, swci.oldSwapchain, nullptr);
         vkGetSwapchainImagesKHR(dev, sc, &ni, nullptr);
         imgs.resize(ni);
         vkGetSwapchainImagesKHR(dev, sc, &ni, imgs.data());
      }
      int f = frames % FR;
      vkWaitForFences(dev, 1, &fences[f], VK_TRUE, UINT64_MAX);
      vkResetFences(dev, 1, &fences[f]);
      uint32_t idx;
      CK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[f], VK_NULL_HANDLE, &idx));
      VkCommandBuffer cb = cbs[f];
      vkResetCommandBuffer(cb, 0);
      VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      vkBeginCommandBuffer(cb, &bi);
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED, imgs[idx], range};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
      VkClearColorValue c = {{(frames % 256) / 255.0f, 0.5f, 1.0f - (frames % 256) / 255.0f, 1.0f}};
      if (still) c = {{1.0f, 0.5f, 0.0f, 1.0f}};   // (VKPB_STILL: orange, to check the presented colors)
      vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
      bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      bar.dstAccessMask = 0;
      bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
      vkEndCommandBuffer(cb);
      VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acq[f], &ws, 1, &cb, 1, &done[f]};
      CK(vkQueueSubmit(q, 1, &si, fences[f]));
      VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &done[f], 1, &sc, &idx};
      QueryPerformanceCounter(&a);
      CK(vkQueuePresentKHR(q, &pi));
      QueryPerformanceCounter(&b);
      presentTime += (b.QuadPart - a.QuadPart) / (double)freq.QuadPart;
      frames++;
   }
   vkDeviceWaitIdle(dev);
   double el = (t.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
   printf("vulkan %ux%u mode %d: %.0f fps, present %.2f ms\n", w, h, mode, frames / el, presentTime / frames * 1000);
   return 0;
}
