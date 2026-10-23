/*
 * Copyright 2026 WinBoat contributors
 * SPDX-License-Identifier: MIT
 */

/* H.264/HEVC encoding on Vulkan Video, through the Khronos loader (vulkan-1.dll: in WinBoat's VM the Venus driver,
 * NVENC on NVIDIA hosts). I and P pictures, one reference (the previous picture); parameter sets built here and
 * written by the driver (vkGetEncodedVideoSessionParametersKHR: what it encodes with, its overrides included).
 * Synchronous: each picture is uploaded, encoded and read back before the call returns. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"
#include "vk_encoder.h"

void amf_log(const char *format, ...);

#define INSTANCE_FUNCS(X)                                                                                              \
   X(DestroyInstance)                                                                                                  \
   X(EnumeratePhysicalDevices)                                                                                         \
   X(GetPhysicalDeviceProperties)                                                                                      \
   X(GetPhysicalDeviceQueueFamilyProperties)                                                                           \
   X(GetPhysicalDeviceMemoryProperties)                                                                                \
   X(EnumerateDeviceExtensionProperties)                                                                               \
   X(GetPhysicalDeviceVideoCapabilitiesKHR)                                                                            \
   X(CreateDevice)                                                                                                     \
   X(GetDeviceProcAddr)

#define DEVICE_FUNCS(X)                                                                                                \
   X(DestroyDevice)                                                                                                    \
   X(GetDeviceQueue)                                                                                                   \
   X(CreateVideoSessionKHR)                                                                                            \
   X(DestroyVideoSessionKHR)                                                                                           \
   X(GetVideoSessionMemoryRequirementsKHR)                                                                             \
   X(BindVideoSessionMemoryKHR)                                                                                        \
   X(CreateVideoSessionParametersKHR)                                                                                  \
   X(DestroyVideoSessionParametersKHR)                                                                                 \
   X(GetEncodedVideoSessionParametersKHR)                                                                              \
   X(AllocateMemory)                                                                                                   \
   X(FreeMemory)                                                                                                       \
   X(MapMemory)                                                                                                        \
   X(InvalidateMappedMemoryRanges)                                                                                     \
   X(FlushMappedMemoryRanges)                                                                                          \
   X(CreateImage)                                                                                                      \
   X(DestroyImage)                                                                                                     \
   X(GetImageMemoryRequirements)                                                                                       \
   X(BindImageMemory)                                                                                                  \
   X(CreateImageView)                                                                                                  \
   X(DestroyImageView)                                                                                                 \
   X(CreateBuffer)                                                                                                     \
   X(DestroyBuffer)                                                                                                    \
   X(GetBufferMemoryRequirements)                                                                                      \
   X(BindBufferMemory)                                                                                                 \
   X(CreateQueryPool)                                                                                                  \
   X(DestroyQueryPool)                                                                                                 \
   X(GetQueryPoolResults)                                                                                              \
   X(CreateCommandPool)                                                                                                \
   X(DestroyCommandPool)                                                                                               \
   X(AllocateCommandBuffers)                                                                                           \
   X(BeginCommandBuffer)                                                                                               \
   X(EndCommandBuffer)                                                                                                 \
   X(CreateFence)                                                                                                      \
   X(DestroyFence)                                                                                                     \
   X(WaitForFences)                                                                                                    \
   X(ResetFences)                                                                                                      \
   X(QueueSubmit)                                                                                                      \
   X(CmdPipelineBarrier2)                                                                                              \
   X(CmdCopyBufferToImage)                                                                                             \
   X(CmdResetQueryPool)                                                                                                \
   X(CmdBeginQuery)                                                                                                    \
   X(CmdEndQuery)                                                                                                      \
   X(CmdBeginVideoCodingKHR)                                                                                           \
   X(CmdEndVideoCodingKHR)                                                                                             \
   X(CmdControlVideoCodingKHR)                                                                                         \
   X(CmdEncodeVideoKHR)

struct vk_encoder {
   vk_encoder_config c;
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   VkQueue queue;
   uint32_t family;
   VkPhysicalDeviceMemoryProperties memory;
#define X(f) PFN_vk##f f;
   INSTANCE_FUNCS(X)
   DEVICE_FUNCS(X)
#undef X
   /* the profile (pNext into the struct itself: it stays where it is) */
   VkVideoProfileInfoKHR profile;
   VkVideoEncodeH264ProfileInfoKHR h264Profile;
   VkVideoEncodeH265ProfileInfoKHR h265Profile;
   VkVideoProfileListInfoKHR profileList;
   VkVideoCapabilitiesKHR caps;
   VkVideoEncodeCapabilitiesKHR encodeCaps;
   VkVideoEncodeH264CapabilitiesKHR h264Caps;
   VkVideoEncodeH265CapabilitiesKHR h265Caps;
   VkFormat format;
   int bytes;
   uint32_t codedWidth, codedHeight;
   VkVideoSessionKHR session;
   VkDeviceMemory sessionMemory[16];
   uint32_t sessionMemoryCount;
   VkVideoSessionParametersKHR params;
   VkImage src, dpb;
   VkDeviceMemory srcMemory, dpbMemory;
   VkImageView srcView, dpbViews[2];
   VkBuffer staging, bitstream;
   VkDeviceMemory stagingMemory, bitstreamMemory;
   unsigned char *stagingMap, *bitstreamMap;
   VkDeviceSize stagingSize, bitstreamSize;
   int stagingCoherent, bitstreamCoherent;
   VkQueryPool query;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   /* the parameter sets, and the output (parameter sets + picture) */
   unsigned char *headers;
   size_t headersSize;
   unsigned char *out;
   size_t outSize;
   /* picture state */
   long long frame, sinceIdr;
   int slot, idrPicId;
   int started;
   int rcMode;
};

static HMODULE loader;
static PFN_vkGetInstanceProcAddr gipa;

static int
load_loader(void)
{
   if (gipa)
      return 1;
   loader = LoadLibraryA("vulkan-1.dll");
   if (loader)
      gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(loader, "vkGetInstanceProcAddr");
   if (!gipa)
      amf_log("no Vulkan loader (vulkan-1.dll)");
   return gipa != NULL;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Levels and profiles: AMF's numbers -> Vulkan's */

static StdVideoH264LevelIdc
h264_level(int level, StdVideoH264LevelIdc max)
{
   static const int idc[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
   for (int i = 0; i < (int)(sizeof(idc) / sizeof(idc[0])); i++)
      if (idc[i] == level && i <= (int)max)
         return (StdVideoH264LevelIdc)i;
   return max;
}

static StdVideoH265LevelIdc
h265_level(int level, StdVideoH265LevelIdc max)
{
   static const int idc[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
   for (int i = 0; i < (int)(sizeof(idc) / sizeof(idc[0])); i++)
      if (idc[i] == level && i <= (int)max)
         return (StdVideoH265LevelIdc)i;
   return max;
}

static StdVideoH264ProfileIdc
h264_profile(int amf)
{
   switch (amf) {
   case 66:
   case 256: return STD_VIDEO_H264_PROFILE_IDC_BASELINE;
   case 77: return STD_VIDEO_H264_PROFILE_IDC_MAIN;
   default: return STD_VIDEO_H264_PROFILE_IDC_HIGH;
   }
}

static void
profile_fill(vk_encoder *e)
{
   memset(&e->profile, 0, sizeof(e->profile));
   e->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
   e->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
   e->profile.lumaBitDepth = e->profile.chromaBitDepth =
      e->c.tenBit ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
   if (e->c.hevc) {
      e->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
      e->h265Profile = (VkVideoEncodeH265ProfileInfoKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR};
      e->h265Profile.stdProfileIdc = e->c.tenBit ? STD_VIDEO_H265_PROFILE_IDC_MAIN_10 : STD_VIDEO_H265_PROFILE_IDC_MAIN;
      e->profile.pNext = &e->h265Profile;
   } else {
      e->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
      e->h264Profile = (VkVideoEncodeH264ProfileInfoKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
      e->h264Profile.stdProfileIdc = h264_profile(e->c.profile);
      e->profile.pNext = &e->h264Profile;
   }
   e->profileList = (VkVideoProfileListInfoKHR){VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
   e->profileList.profileCount = 1;
   e->profileList.pProfiles = &e->profile;
   e->format = e->c.tenBit ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   e->bytes = e->c.tenBit ? 2 : 1;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* The device: the first with this codec's encoder */

static int
has_extension(const VkExtensionProperties *exts, uint32_t count, const char *name)
{
   for (uint32_t i = 0; i < count; i++)
      if (!strcmp(exts[i].extensionName, name))
         return 1;
   return 0;
}

static int
device_open(vk_encoder *e)
{
   if (!load_loader())
      return 0;
   PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)(void *)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.pApplicationName = "WinBoat AMF runtime";
   app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &app;
   VkResult r = createInstance ? createInstance(&ici, NULL, &e->instance) : VK_ERROR_INITIALIZATION_FAILED;
   if (r != VK_SUCCESS) {
      amf_log("vkCreateInstance: %d", (int)r);
      return 0;
   }
#define X(f)                                                                                                           \
   if (!(e->f = (PFN_vk##f)(void *)gipa(e->instance, "vk" #f))) {                                                     \
      amf_log("no vk" #f);                                                                                             \
      return 0;                                                                                                        \
   }
   INSTANCE_FUNCS(X)
#undef X
   const char *codecExt = e->c.hevc ? VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME : VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
   uint32_t n = 0;
   e->EnumeratePhysicalDevices(e->instance, &n, NULL);
   VkPhysicalDevice devices[16];
   n = n > 16 ? 16 : n;
   e->EnumeratePhysicalDevices(e->instance, &n, devices);
   profile_fill(e);
   for (uint32_t d = 0; d < n && !e->physical; d++) {
      VkPhysicalDeviceProperties props;
      e->GetPhysicalDeviceProperties(devices[d], &props);
      if (props.apiVersion < VK_API_VERSION_1_3)
         continue;
      uint32_t extCount = 0;
      e->EnumerateDeviceExtensionProperties(devices[d], NULL, &extCount, NULL);
      VkExtensionProperties *exts = calloc(extCount ? extCount : 1, sizeof(*exts));
      e->EnumerateDeviceExtensionProperties(devices[d], NULL, &extCount, exts);
      int ok = has_extension(exts, extCount, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
               has_extension(exts, extCount, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) &&
               has_extension(exts, extCount, codecExt);
      free(exts);
      if (!ok)
         continue;
      uint32_t qc = 0;
      e->GetPhysicalDeviceQueueFamilyProperties(devices[d], &qc, NULL);
      VkQueueFamilyProperties qf[32];
      qc = qc > 32 ? 32 : qc;
      e->GetPhysicalDeviceQueueFamilyProperties(devices[d], &qc, qf);
      for (uint32_t i = 0; i < qc; i++) {
         if ((qf[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) && (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            e->caps = (VkVideoCapabilitiesKHR){VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
            e->encodeCaps = (VkVideoEncodeCapabilitiesKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR};
            e->caps.pNext = &e->encodeCaps;
            if (e->c.hevc) {
               e->h265Caps = (VkVideoEncodeH265CapabilitiesKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR};
               e->encodeCaps.pNext = &e->h265Caps;
            } else {
               e->h264Caps = (VkVideoEncodeH264CapabilitiesKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
               e->encodeCaps.pNext = &e->h264Caps;
            }
            if (e->GetPhysicalDeviceVideoCapabilitiesKHR(devices[d], &e->profile, &e->caps) == VK_SUCCESS) {
               e->physical = devices[d];
               e->family = i;
               amf_log("encoding on %s, queue family %u", props.deviceName, i);
            }
            break;
         }
      }
   }
   if (!e->physical) {
      amf_log("no Vulkan device encodes %s%s", e->c.hevc ? "HEVC" : "H.264", e->c.tenBit ? " 10-bit" : "");
      return 0;
   }
   e->GetPhysicalDeviceMemoryProperties(e->physical, &e->memory);

   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = e->family;
   qci.queueCount = 1;
   qci.pQueuePriorities = &priority;
   const char *extensions[] = {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, codecExt};
   VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   v13.synchronization2 = VK_TRUE;
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   dci.pNext = &v13;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.enabledExtensionCount = 3;
   dci.ppEnabledExtensionNames = extensions;
   if ((r = e->CreateDevice(e->physical, &dci, NULL, &e->device)) != VK_SUCCESS) {
      amf_log("vkCreateDevice: %d", (int)r);
      return 0;
   }
#define X(f)                                                                                                           \
   if (!(e->f = (PFN_vk##f)(void *)e->GetDeviceProcAddr(e->device, "vk" #f))) {                                       \
      amf_log("no vk" #f);                                                                                             \
      return 0;                                                                                                        \
   }
   DEVICE_FUNCS(X)
#undef X
   e->GetDeviceQueue(e->device, e->family, 0, &e->queue);
   return 1;
}

static VkDeviceMemory
memory_alloc(vk_encoder *e, VkMemoryRequirements req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer,
             int *coherent)
{
   for (int pass = 0; pass < 2; pass++) {
      for (uint32_t i = 0; i < e->memory.memoryTypeCount; i++) {
         VkMemoryPropertyFlags f = e->memory.memoryTypes[i].propertyFlags;
         if (!(req.memoryTypeBits & (1u << i)) || (f & want) != want || (!pass && (f & prefer) != prefer))
            continue;
         VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
         info.allocationSize = req.size;
         info.memoryTypeIndex = i;
         VkDeviceMemory m;
         if (e->AllocateMemory(e->device, &info, NULL, &m) == VK_SUCCESS) {
            if (coherent)
               *coherent = !!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            return m;
         }
      }
   }
   return VK_NULL_HANDLE;
}

static int
image_make(vk_encoder *e, VkImageUsageFlags usage, uint32_t layers, VkImage *image, VkDeviceMemory *memory)
{
   VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   info.pNext = &e->profileList;
   info.imageType = VK_IMAGE_TYPE_2D;
   info.format = e->format;
   info.extent = (VkExtent3D){e->codedWidth, e->codedHeight, 1};
   info.mipLevels = 1;
   info.arrayLayers = layers;
   info.samples = VK_SAMPLE_COUNT_1_BIT;
   info.tiling = VK_IMAGE_TILING_OPTIMAL;
   info.usage = usage;
   info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (e->CreateImage(e->device, &info, NULL, image) != VK_SUCCESS)
      return 0;
   VkMemoryRequirements req;
   e->GetImageMemoryRequirements(e->device, *image, &req);
   *memory = memory_alloc(e, req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
   return *memory && e->BindImageMemory(e->device, *image, *memory, 0) == VK_SUCCESS;
}

static VkImageView
view_make(vk_encoder *e, VkImage image, uint32_t layer)
{
   VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   info.image = image;
   info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   info.format = e->format;
   info.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
   VkImageView view = VK_NULL_HANDLE;
   e->CreateImageView(e->device, &info, NULL, &view);
   return view;
}

static int
buffer_make(vk_encoder *e, VkBufferUsageFlags usage, VkDeviceSize size, int profile, VkBuffer *buf,
            VkDeviceMemory *memory, unsigned char **map, int *coherent)
{
   VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   info.pNext = profile ? &e->profileList : NULL;
   info.size = size;
   info.usage = usage;
   if (e->CreateBuffer(e->device, &info, NULL, buf) != VK_SUCCESS)
      return 0;
   VkMemoryRequirements req;
   e->GetBufferMemoryRequirements(e->device, *buf, &req);
   /* read back: cached is faster to read */
   *memory = memory_alloc(e, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          profile ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, coherent);
   return *memory && e->BindBufferMemory(e->device, *buf, *memory, 0) == VK_SUCCESS &&
          e->MapMemory(e->device, *memory, 0, VK_WHOLE_SIZE, 0, (void **)map) == VK_SUCCESS;
}

static void
range_sync(vk_encoder *e, VkDeviceMemory memory, int coherent, int invalidate)
{
   if (coherent)
      return;
   VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
   range.memory = memory;
   range.size = VK_WHOLE_SIZE;
   if (invalidate)
      e->InvalidateMappedMemoryRanges(e->device, 1, &range);
   else
      e->FlushMappedMemoryRanges(e->device, 1, &range);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Parameter sets */

static uint32_t
log2_of(uint32_t v)
{
   uint32_t l = 0;
   while ((1u << (l + 1)) <= v)
      l++;
   return l;
}

static uint32_t
ctb_size(vk_encoder *e)
{
   VkVideoEncodeH265CtbSizeFlagsKHR ctbs = e->h265Caps.ctbSizes;
   return (ctbs & VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR) ? 32 : (ctbs & VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR) ? 64 : 16;
}

static int
params_make(vk_encoder *e)
{
   VkVideoSessionParametersCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
   info.videoSession = e->session;
   uint32_t w = (uint32_t)e->c.width, h = (uint32_t)e->c.height;
   VkResult r;
   if (!e->c.hevc) {
      StdVideoH264SequenceParameterSet sps;
      StdVideoH264PictureParameterSet pps;
      memset(&sps, 0, sizeof(sps));
      memset(&pps, 0, sizeof(pps));
      uint32_t mbW = e->codedWidth / 16, mbH = e->codedHeight / 16;
      sps.flags.direct_8x8_inference_flag = 1;
      sps.flags.frame_mbs_only_flag = 1;
      sps.flags.frame_cropping_flag = mbW * 16 != w || mbH * 16 != h;
      sps.profile_idc = e->h264Profile.stdProfileIdc;
      sps.level_idc = h264_level(e->c.level, e->h264Caps.maxLevelIdc);
      sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
      sps.log2_max_frame_num_minus4 = 4;
      /* POC type 2: output order is decoding order (no B-pictures) */
      sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
      sps.max_num_ref_frames = 1;
      sps.pic_width_in_mbs_minus1 = mbW - 1;
      sps.pic_height_in_map_units_minus1 = mbH - 1;
      sps.frame_crop_right_offset = (mbW * 16 - w) / 2;
      sps.frame_crop_bottom_offset = (mbH * 16 - h) / 2;
      pps.flags.entropy_coding_mode_flag = e->c.cabac && sps.profile_idc != STD_VIDEO_H264_PROFILE_IDC_BASELINE;
      pps.flags.deblocking_filter_control_present_flag = 1;
      pps.flags.transform_8x8_mode_flag = sps.profile_idc == STD_VIDEO_H264_PROFILE_IDC_HIGH;
      pps.weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;
      VkVideoEncodeH264SessionParametersAddInfoKHR add = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
      add.stdSPSCount = 1;
      add.pStdSPSs = &sps;
      add.stdPPSCount = 1;
      add.pStdPPSs = &pps;
      VkVideoEncodeH264SessionParametersCreateInfoKHR ci = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
      ci.maxStdSPSCount = 1;
      ci.maxStdPPSCount = 1;
      ci.pParametersAddInfo = &add;
      info.pNext = &ci;
      r = e->CreateVideoSessionParametersKHR(e->device, &info, NULL, &e->params);
   } else {
      StdVideoH265ProfileTierLevel ptl;
      StdVideoH265DecPicBufMgr dpbm;
      StdVideoH265VideoParameterSet vps;
      StdVideoH265SequenceParameterSet sps;
      StdVideoH265PictureParameterSet pps;
      memset(&ptl, 0, sizeof(ptl));
      memset(&dpbm, 0, sizeof(dpbm));
      memset(&vps, 0, sizeof(vps));
      memset(&sps, 0, sizeof(sps));
      memset(&pps, 0, sizeof(pps));
      ptl.flags.general_tier_flag = e->c.highTier;
      ptl.flags.general_progressive_source_flag = 1;
      ptl.flags.general_non_packed_constraint_flag = 1;
      ptl.flags.general_frame_only_constraint_flag = 1;
      ptl.general_profile_idc = e->h265Profile.stdProfileIdc;
      ptl.general_level_idc = h265_level(e->c.level, e->h265Caps.maxLevelIdc);
      dpbm.max_dec_pic_buffering_minus1[0] = 1;
      dpbm.max_latency_increase_plus1[0] = 1;
      vps.flags.vps_temporal_id_nesting_flag = 1;
      vps.pDecPicBufMgr = &dpbm;
      vps.pProfileTierLevel = &ptl;
      uint32_t ctb = ctb_size(e);
      sps.flags.sps_temporal_id_nesting_flag = 1;
      sps.flags.conformance_window_flag = e->codedWidth != w || e->codedHeight != h;
      sps.flags.sample_adaptive_offset_enabled_flag =
         !!(e->h265Caps.stdSyntaxFlags & VK_VIDEO_ENCODE_H265_STD_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG_SET_BIT_KHR);
      sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
      sps.pic_width_in_luma_samples = e->codedWidth;
      sps.pic_height_in_luma_samples = e->codedHeight;
      sps.bit_depth_luma_minus8 = sps.bit_depth_chroma_minus8 = e->c.tenBit ? 2 : 0;
      sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
      sps.log2_min_luma_coding_block_size_minus3 = 0;
      sps.log2_diff_max_min_luma_coding_block_size = (uint8_t)(log2_of(ctb) - 3);
      sps.log2_min_luma_transform_block_size_minus2 = 0;
      sps.log2_diff_max_min_luma_transform_block_size = 3;
      sps.max_transform_hierarchy_depth_inter = (uint8_t)(log2_of(ctb) - 2);
      sps.max_transform_hierarchy_depth_intra = (uint8_t)(log2_of(ctb) - 2);
      sps.conf_win_right_offset = (e->codedWidth - w) / 2;
      sps.conf_win_bottom_offset = (e->codedHeight - h) / 2;
      sps.pProfileTierLevel = &ptl;
      sps.pDecPicBufMgr = &dpbm;
      pps.flags.cabac_init_present_flag = 1;
      pps.flags.cu_qp_delta_enabled_flag = 1;
      pps.flags.pps_slice_chroma_qp_offsets_present_flag = 1;
      pps.flags.pps_loop_filter_across_slices_enabled_flag = 1;
      pps.flags.deblocking_filter_control_present_flag = 1;
      VkVideoEncodeH265SessionParametersAddInfoKHR add = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
      add.stdVPSCount = 1;
      add.pStdVPSs = &vps;
      add.stdSPSCount = 1;
      add.pStdSPSs = &sps;
      add.stdPPSCount = 1;
      add.pStdPPSs = &pps;
      VkVideoEncodeH265SessionParametersCreateInfoKHR ci = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
      ci.maxStdVPSCount = 1;
      ci.maxStdSPSCount = 1;
      ci.maxStdPPSCount = 1;
      ci.pParametersAddInfo = &add;
      info.pNext = &ci;
      r = e->CreateVideoSessionParametersKHR(e->device, &info, NULL, &e->params);
   }
   if (r != VK_SUCCESS) {
      amf_log("vkCreateVideoSessionParametersKHR: %d", (int)r);
      return 0;
   }

   /* the parameter sets as the driver writes them */
   VkVideoEncodeSessionParametersGetInfoKHR get = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR};
   get.videoSessionParameters = e->params;
   VkVideoEncodeH264SessionParametersGetInfoKHR h264Get = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
   h264Get.writeStdSPS = h264Get.writeStdPPS = VK_TRUE;
   VkVideoEncodeH265SessionParametersGetInfoKHR h265Get = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR};
   h265Get.writeStdVPS = h265Get.writeStdSPS = h265Get.writeStdPPS = VK_TRUE;
   get.pNext = e->c.hevc ? (const void *)&h265Get : (const void *)&h264Get;
   size_t size = 0;
   if ((r = e->GetEncodedVideoSessionParametersKHR(e->device, &get, NULL, &size, NULL)) != VK_SUCCESS || !size) {
      amf_log("vkGetEncodedVideoSessionParametersKHR (size): %d", (int)r);
      return 0;
   }
   e->headers = malloc(size);
   if (!e->headers || (r = e->GetEncodedVideoSessionParametersKHR(e->device, &get, NULL, &size, e->headers)) != VK_SUCCESS) {
      amf_log("vkGetEncodedVideoSessionParametersKHR: %d", (int)r);
      return 0;
   }
   e->headersSize = size;
   char hex[512];
   for (size_t i = 0; i < size && i < 200; i++)
      sprintf(hex + 2 * i, "%02x", e->headers[i]);
   amf_log("parameter sets: %s", hex);
   return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Rate control */

typedef struct {
   VkVideoEncodeRateControlInfoKHR info;
   VkVideoEncodeRateControlLayerInfoKHR layer;
   VkVideoEncodeH264RateControlInfoKHR h264;
   VkVideoEncodeH265RateControlInfoKHR h265;
   VkVideoEncodeH264RateControlLayerInfoKHR h264Layer;
   VkVideoEncodeH265RateControlLayerInfoKHR h265Layer;
   VkVideoEncodeQualityLevelInfoKHR quality;
} rate_control;

static VkVideoEncodeRateControlInfoKHR *
rc_build(vk_encoder *e, rate_control *rc)
{
   memset(rc, 0, sizeof(*rc));
   rc->info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
   rc->layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
   int on = e->rcMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
   rc->info.rateControlMode = (VkVideoEncodeRateControlModeFlagBitsKHR)e->rcMode;
   if (on) {
      long long rate = e->c.bitrate > 0 ? e->c.bitrate : 20000000;
      long long peak = e->c.rc == VK_ENCODER_RC_CBR ? rate : (e->c.peak > rate ? e->c.peak : rate);
      long long ms = e->c.vbv > 0 ? e->c.vbv * 1000 / rate : 1000;
      ms = ms < 1 ? 1 : ms;
      rc->info.virtualBufferSizeInMs = (uint32_t)ms;
      rc->info.initialVirtualBufferSizeInMs = (uint32_t)(ms / 2);
      rc->info.layerCount = 1;
      rc->info.pLayers = &rc->layer;
      rc->layer.averageBitrate = (uint64_t)rate;
      rc->layer.maxBitrate = (uint64_t)peak;
      rc->layer.frameRateNumerator = (uint32_t)e->c.fpsNum;
      rc->layer.frameRateDenominator = (uint32_t)e->c.fpsDen;
   }
   uint32_t gop = e->c.idrPeriod > 0 && e->c.idrPeriod < (1 << 30) ? (uint32_t)e->c.idrPeriod : 0;
   if (e->c.hevc) {
      rc->h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR;
      rc->h265.flags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
      rc->h265.gopFrameCount = gop;
      rc->h265.idrPeriod = gop;
      rc->h265.subLayerCount = on ? 1 : 0;
      rc->info.pNext = &rc->h265;
      rc->h265Layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR;
      if (on)
         rc->layer.pNext = &rc->h265Layer;
   } else {
      rc->h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
      rc->h264.flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
      rc->h264.gopFrameCount = gop;
      rc->h264.idrPeriod = gop;
      rc->h264.temporalLayerCount = on ? 1 : 0;
      rc->info.pNext = &rc->h264;
      rc->h264Layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR;
      if (on)
         rc->layer.pNext = &rc->h264Layer;
   }
   return &rc->info;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Creation */

/* the support check's open device, handed to the first encoder of the codec (opening one takes ~0.5 s) */
static vk_encoder *probe[2];

int
vk_encoder_supported(int hevc)
{
   static int known[2], result[2];
   if (!known[hevc]) {
      vk_encoder *e = calloc(1, sizeof(*e));
      e->c.hevc = hevc;
      e->c.profile = hevc ? 1 : 100;
      result[hevc] = device_open(e);
      if (result[hevc])
         probe[hevc] = e;
      else
         vk_encoder_destroy(e);
      known[hevc] = 1;
   }
   return result[hevc];
}

vk_encoder *
vk_encoder_create(const vk_encoder_config *config)
{
   vk_encoder *e = calloc(1, sizeof(*e));
   if (!e)
      return NULL;
   e->c = *config;
   if (e->c.tenBit && !e->c.hevc) {
      amf_log("10-bit H.264: not supported");
      free(e);
      return NULL;
   }
   vk_encoder *p = probe[e->c.hevc];
   if (p && !e->c.tenBit && (e->c.hevc || h264_profile(e->c.profile) == p->h264Profile.stdProfileIdc)) {
      /* the probe's instance, device and caps (the same profile) */
      probe[e->c.hevc] = NULL;
      vk_encoder_config c = e->c;
      free(e);
      e = p;
      e->c = c;
   } else if (!device_open(e)) {
      goto fail;
   }
   uint32_t align = e->c.hevc ? ctb_size(e) : 16;
   uint32_t gw = e->caps.pictureAccessGranularity.width ? e->caps.pictureAccessGranularity.width : 1;
   uint32_t gh = e->caps.pictureAccessGranularity.height ? e->caps.pictureAccessGranularity.height : 1;
   align = align > gw ? align : gw;
   uint32_t alignH = align > gh ? align : gh;
   e->codedWidth = ((uint32_t)e->c.width + align - 1) / align * align;
   e->codedHeight = ((uint32_t)e->c.height + alignH - 1) / alignH * alignH;
   if (e->codedWidth > e->caps.maxCodedExtent.width || e->codedHeight > e->caps.maxCodedExtent.height ||
       e->codedWidth < e->caps.minCodedExtent.width || e->codedHeight < e->caps.minCodedExtent.height) {
      amf_log("%ux%u: outside the encoder's %ux%u..%ux%u", e->codedWidth, e->codedHeight, e->caps.minCodedExtent.width,
              e->caps.minCodedExtent.height, e->caps.maxCodedExtent.width, e->caps.maxCodedExtent.height);
      goto fail;
   }
   VkVideoEncodeRateControlModeFlagsKHR modes = e->encodeCaps.rateControlModes;
   e->rcMode = e->c.rc == VK_ENCODER_RC_CQP && (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR)
                  ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR
               : e->c.rc == VK_ENCODER_RC_CBR && (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
                  ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
               : (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR
                                                                         : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;

   /* the session */
   VkVideoSessionCreateInfoKHR sci = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
   sci.queueFamilyIndex = e->family;
   sci.pVideoProfile = &e->profile;
   sci.pictureFormat = e->format;
   sci.maxCodedExtent = (VkExtent2D){e->codedWidth, e->codedHeight};
   sci.referencePictureFormat = e->format;
   sci.maxDpbSlots = 2;
   sci.maxActiveReferencePictures = 1;
   sci.pStdHeaderVersion = &e->caps.stdHeaderVersion;
   VkResult r = e->CreateVideoSessionKHR(e->device, &sci, NULL, &e->session);
   if (r != VK_SUCCESS) {
      amf_log("vkCreateVideoSessionKHR: %d", (int)r);
      goto fail;
   }
   uint32_t count = 0;
   e->GetVideoSessionMemoryRequirementsKHR(e->device, e->session, &count, NULL);
   VkVideoSessionMemoryRequirementsKHR reqs[16];
   count = count > 16 ? 16 : count;
   for (uint32_t i = 0; i < count; i++)
      reqs[i] = (VkVideoSessionMemoryRequirementsKHR){VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR};
   e->GetVideoSessionMemoryRequirementsKHR(e->device, e->session, &count, reqs);
   VkBindVideoSessionMemoryInfoKHR binds[16];
   for (uint32_t i = 0; i < count; i++) {
      VkDeviceMemory m = memory_alloc(e, reqs[i].memoryRequirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
      if (!m)
         goto fail;
      e->sessionMemory[e->sessionMemoryCount++] = m;
      binds[i] = (VkBindVideoSessionMemoryInfoKHR){VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
      binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
      binds[i].memory = m;
      binds[i].memorySize = reqs[i].memoryRequirements.size;
   }
   if (count && e->BindVideoSessionMemoryKHR(e->device, e->session, count, binds) != VK_SUCCESS)
      goto fail;
   if (!params_make(e))
      goto fail;

   /* pictures and buffers */
   if (!image_make(e, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, 1, &e->src, &e->srcMemory) ||
       !image_make(e, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, 2, &e->dpb, &e->dpbMemory)) {
      amf_log("encoder pictures: no memory");
      goto fail;
   }
   e->srcView = view_make(e, e->src, 0);
   e->dpbViews[0] = view_make(e, e->dpb, 0);
   e->dpbViews[1] = view_make(e, e->dpb, 1);
   e->stagingSize = (VkDeviceSize)e->codedWidth * e->codedHeight * 3 / 2 * e->bytes;
   e->bitstreamSize = ((VkDeviceSize)e->codedWidth * e->codedHeight * 3 + (1 << 20) + 4095) & ~(VkDeviceSize)4095;
   if (!e->srcView || !e->dpbViews[0] || !e->dpbViews[1] ||
       !buffer_make(e, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, e->stagingSize, 0, &e->staging, &e->stagingMemory,
                    &e->stagingMap, &e->stagingCoherent) ||
       !buffer_make(e, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, e->bitstreamSize, 1, &e->bitstream,
                    &e->bitstreamMemory, &e->bitstreamMap, &e->bitstreamCoherent)) {
      amf_log("encoder buffers: no memory");
      goto fail;
   }
   VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback = {VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
   feedback.pNext = &e->profile;
   feedback.encodeFeedbackFlags =
      VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR | VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
   VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
   qpi.pNext = &feedback;
   qpi.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
   qpi.queryCount = 1;
   VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cpi.queueFamilyIndex = e->family;
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (e->CreateQueryPool(e->device, &qpi, NULL, &e->query) != VK_SUCCESS ||
       e->CreateCommandPool(e->device, &cpi, NULL, &e->pool) != VK_SUCCESS ||
       e->CreateFence(e->device, &fci, NULL, &e->fence) != VK_SUCCESS)
      goto fail;
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   cai.commandPool = e->pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   if (e->AllocateCommandBuffers(e->device, &cai, &e->cmd) != VK_SUCCESS)
      goto fail;
   amf_log("encoder ready: %ux%u coded, rate control mode 0x%x (%lld bit/s, peak %lld, VBV %lld, %d/%d fps), %zu bytes "
           "of parameter sets", e->codedWidth, e->codedHeight, (unsigned)e->rcMode, e->c.bitrate, e->c.peak, e->c.vbv,
           e->c.fpsNum, e->c.fpsDen, e->headersSize);
   return e;

fail:
   vk_encoder_destroy(e);
   return NULL;
}

void
vk_encoder_destroy(vk_encoder *e)
{
   if (!e)
      return;
   if (e->device) {
      if (e->fence)
         e->DestroyFence(e->device, e->fence, NULL);
      if (e->pool)
         e->DestroyCommandPool(e->device, e->pool, NULL);
      if (e->query)
         e->DestroyQueryPool(e->device, e->query, NULL);
      if (e->staging)
         e->DestroyBuffer(e->device, e->staging, NULL);
      if (e->bitstream)
         e->DestroyBuffer(e->device, e->bitstream, NULL);
      if (e->stagingMemory)
         e->FreeMemory(e->device, e->stagingMemory, NULL);
      if (e->bitstreamMemory)
         e->FreeMemory(e->device, e->bitstreamMemory, NULL);
      for (int i = 0; i < 2; i++)
         if (e->dpbViews[i])
            e->DestroyImageView(e->device, e->dpbViews[i], NULL);
      if (e->srcView)
         e->DestroyImageView(e->device, e->srcView, NULL);
      if (e->src)
         e->DestroyImage(e->device, e->src, NULL);
      if (e->dpb)
         e->DestroyImage(e->device, e->dpb, NULL);
      if (e->srcMemory)
         e->FreeMemory(e->device, e->srcMemory, NULL);
      if (e->dpbMemory)
         e->FreeMemory(e->device, e->dpbMemory, NULL);
      if (e->params)
         e->DestroyVideoSessionParametersKHR(e->device, e->params, NULL);
      if (e->session)
         e->DestroyVideoSessionKHR(e->device, e->session, NULL);
      for (uint32_t i = 0; i < e->sessionMemoryCount; i++)
         e->FreeMemory(e->device, e->sessionMemory[i], NULL);
      e->DestroyDevice(e->device, NULL);
   }
   if (e->instance && e->DestroyInstance)
      e->DestroyInstance(e->instance, NULL);
   free(e->headers);
   free(e->out);
   free(e);
}

const unsigned char *
vk_encoder_headers(vk_encoder *e, size_t *size)
{
   *size = e->headersSize;
   return e->headers;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Encoding */

/* the picture into the staging buffer: NV12 / P010 layout at the coded size, the edges repeated into the padding */
static void
upload(vk_encoder *e, const vk_encoder_picture *pic)
{
   uint32_t cw = e->codedWidth, ch = e->codedHeight, b = (uint32_t)e->bytes;
   uint32_t w = (uint32_t)pic->width, h = (uint32_t)pic->height;
   unsigned char *y = e->stagingMap, *uv = e->stagingMap + (size_t)cw * ch * b;
   for (uint32_t row = 0; row < ch; row++) {
      unsigned char *dst = y + (size_t)row * cw * b;
      const unsigned char *src = pic->planes[0] + (size_t)(row < h ? row : h - 1) * pic->pitches[0];
      memcpy(dst, src, (size_t)w * b);
      for (uint32_t x = w; x < cw; x++)
         memcpy(dst + (size_t)x * b, dst + (size_t)(w - 1) * b, b);
   }
   uint32_t cw2 = cw / 2, w2 = (w + 1) / 2, h2 = (h + 1) / 2;
   for (uint32_t row = 0; row < ch / 2; row++) {
      unsigned char *dst = uv + (size_t)row * cw2 * 2 * b;
      uint32_t r = row < h2 ? row : h2 - 1;
      if (pic->planeCount == 3) {
         const unsigned char *u = pic->planes[1] + (size_t)r * pic->pitches[1];
         const unsigned char *v = pic->planes[2] + (size_t)r * pic->pitches[2];
         for (uint32_t x = 0; x < w2; x++) {
            dst[2 * x] = u[x];
            dst[2 * x + 1] = v[x];
         }
      } else {
         memcpy(dst, pic->planes[1] + (size_t)r * pic->pitches[1], (size_t)w2 * 2 * b);
      }
      for (uint32_t x = w2; x < cw2; x++)
         memcpy(dst + (size_t)x * 2 * b, dst + (size_t)(w2 - 1) * 2 * b, 2 * b);
   }
   range_sync(e, e->stagingMemory, e->stagingCoherent, 0);
}

static VkImageMemoryBarrier2
image_barrier(VkImage image, uint32_t layer, uint32_t layers, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
   b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
   b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = image;
   b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, layers};
   return b;
}

static void
barrier(vk_encoder *e, const VkImageMemoryBarrier2 *images, uint32_t count)
{
   VkMemoryBarrier2 memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
   memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
   memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
   memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
   memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
   VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
   dep.memoryBarrierCount = 1;
   dep.pMemoryBarriers = &memory;
   dep.imageMemoryBarrierCount = count;
   dep.pImageMemoryBarriers = images;
   e->CmdPipelineBarrier2(e->cmd, &dep);
}

const unsigned char *
vk_encoder_encode(vk_encoder *e, const vk_encoder_picture *pic, size_t *size, int *idrOut)
{
   *size = 0;
   int idr = !e->started || pic->forceIdr || e->sinceIdr >= e->c.idrPeriod;
   if (idr) {
      e->sinceIdr = 0;
      e->idrPicId = (e->idrPicId + 1) & 0xffff;
   }
   int setup = e->slot, ref = 1 - e->slot;
   uint32_t n = (uint32_t)e->sinceIdr;
   int qp = e->rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR ? (idr ? e->c.qpI : e->c.qpP) : 0;
   qp = qp < 0 ? 0 : qp > 51 ? 51 : qp;
   VkExtent2D extent = {e->codedWidth, e->codedHeight};

   upload(e, pic);

   /* the codec's picture information */
   VkVideoEncodeInfoKHR encode = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
   StdVideoEncodeH264PictureInfo h264;
   StdVideoEncodeH264ReferenceListsInfo h264Lists;
   StdVideoEncodeH264SliceHeader h264Slice;
   VkVideoEncodeH264NaluSliceInfoKHR h264Nalu = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
   VkVideoEncodeH264PictureInfoKHR h264Info = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
   StdVideoEncodeH264ReferenceInfo h264Setup, h264Ref;
   VkVideoEncodeH264DpbSlotInfoKHR h264SetupSlot = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
   VkVideoEncodeH264DpbSlotInfoKHR h264RefSlot = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
   StdVideoEncodeH265PictureInfo h265;
   StdVideoEncodeH265ReferenceListsInfo h265Lists;
   StdVideoH265ShortTermRefPicSet rps;
   StdVideoEncodeH265LongTermRefPics ltr;
   StdVideoEncodeH265SliceSegmentHeader h265Slice;
   VkVideoEncodeH265NaluSliceSegmentInfoKHR h265Nalu = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR};
   VkVideoEncodeH265PictureInfoKHR h265Info = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR};
   StdVideoEncodeH265ReferenceInfo h265Setup, h265Ref;
   VkVideoEncodeH265DpbSlotInfoKHR h265SetupSlot = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
   VkVideoEncodeH265DpbSlotInfoKHR h265RefSlot = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR};
   const void *setupInfo, *refInfo;
   if (!e->c.hevc) {
      memset(&h264, 0, sizeof(h264));
      memset(&h264Lists, 0, sizeof(h264Lists));
      memset(&h264Slice, 0, sizeof(h264Slice));
      memset(&h264Setup, 0, sizeof(h264Setup));
      memset(&h264Ref, 0, sizeof(h264Ref));
      StdVideoH264PictureType type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
      h264.flags.IdrPicFlag = idr;
      h264.flags.is_reference = 1;
      h264.idr_pic_id = (uint16_t)e->idrPicId;
      h264.primary_pic_type = type;
      h264.frame_num = n & 0xff;
      h264.PicOrderCnt = (int32_t)(2 * (n & 0x7f));
      memset(h264Lists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(h264Lists.RefPicList0));
      memset(h264Lists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(h264Lists.RefPicList1));
      if (!idr)
         h264Lists.RefPicList0[0] = (uint8_t)ref;
      h264.pRefLists = &h264Lists;
      h264Slice.slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
      h264Slice.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
      h264Slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;
      h264Slice.slice_qp_delta = qp ? (int8_t)(qp - 26) : 0;
      h264Nalu.constantQp = qp;
      h264Nalu.pStdSliceHeader = &h264Slice;
      h264Info.naluSliceEntryCount = 1;
      h264Info.pNaluSliceEntries = &h264Nalu;
      h264Info.pStdPictureInfo = &h264;
      encode.pNext = &h264Info;
      h264Setup.primary_pic_type = type;
      h264Setup.FrameNum = h264.frame_num;
      h264Setup.PicOrderCnt = h264.PicOrderCnt;
      h264SetupSlot.pStdReferenceInfo = &h264Setup;
      h264Ref.primary_pic_type = n == 1 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
      h264Ref.FrameNum = (n - 1) & 0xff;
      h264Ref.PicOrderCnt = (int32_t)(2 * ((n - 1) & 0x7f));
      h264RefSlot.pStdReferenceInfo = &h264Ref;
      setupInfo = &h264SetupSlot;
      refInfo = &h264RefSlot;
   } else {
      memset(&h265, 0, sizeof(h265));
      memset(&h265Lists, 0, sizeof(h265Lists));
      memset(&rps, 0, sizeof(rps));
      memset(&ltr, 0, sizeof(ltr));
      memset(&h265Slice, 0, sizeof(h265Slice));
      memset(&h265Setup, 0, sizeof(h265Setup));
      memset(&h265Ref, 0, sizeof(h265Ref));
      StdVideoH265PictureType type = idr ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
      int sao = !!(e->h265Caps.stdSyntaxFlags & VK_VIDEO_ENCODE_H265_STD_SAMPLE_ADAPTIVE_OFFSET_ENABLED_FLAG_SET_BIT_KHR);
      h265.flags.is_reference = 1;
      h265.flags.IrapPicFlag = idr;
      h265.flags.pic_output_flag = 1;
      h265.pic_type = type;
      h265.PicOrderCntVal = (int32_t)n;
      if (!idr) {
         /* one reference: the previous picture */
         rps.num_negative_pics = 1;
         rps.delta_poc_s0_minus1[0] = 0;
         rps.used_by_curr_pic_s0_flag = 1;
      }
      h265.pShortTermRefPicSet = &rps;
      h265.pLongTermRefPics = &ltr;
      memset(h265Lists.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(h265Lists.RefPicList0));
      memset(h265Lists.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE, sizeof(h265Lists.RefPicList1));
      if (!idr)
         h265Lists.RefPicList0[0] = (uint8_t)ref;
      h265.pRefLists = &h265Lists;
      h265Slice.flags.first_slice_segment_in_pic_flag = 1;
      h265Slice.flags.slice_sao_luma_flag = sao;
      h265Slice.flags.slice_sao_chroma_flag = sao;
      h265Slice.flags.collocated_from_l0_flag = 1;
      h265Slice.flags.slice_loop_filter_across_slices_enabled_flag = 1;
      h265Slice.slice_type = idr ? STD_VIDEO_H265_SLICE_TYPE_I : STD_VIDEO_H265_SLICE_TYPE_P;
      h265Slice.MaxNumMergeCand = 5;
      h265Slice.slice_qp_delta = qp ? (int8_t)(qp - 26) : 0;
      h265Nalu.constantQp = qp;
      h265Nalu.pStdSliceSegmentHeader = &h265Slice;
      h265Info.naluSliceSegmentEntryCount = 1;
      h265Info.pNaluSliceSegmentEntries = &h265Nalu;
      h265Info.pStdPictureInfo = &h265;
      encode.pNext = &h265Info;
      h265Setup.pic_type = type;
      h265Setup.PicOrderCntVal = (int32_t)n;
      h265SetupSlot.pStdReferenceInfo = &h265Setup;
      h265Ref.pic_type = n == 1 ? STD_VIDEO_H265_PICTURE_TYPE_IDR : STD_VIDEO_H265_PICTURE_TYPE_P;
      h265Ref.PicOrderCntVal = (int32_t)n - 1;
      h265RefSlot.pStdReferenceInfo = &h265Ref;
      setupInfo = &h265SetupSlot;
      refInfo = &h265RefSlot;
   }

   VkVideoPictureResourceInfoKHR srcPicture = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
   srcPicture.codedExtent = extent;
   srcPicture.imageViewBinding = e->srcView;
   VkVideoPictureResourceInfoKHR setupPicture = srcPicture, refPicture = srcPicture;
   setupPicture.imageViewBinding = e->dpbViews[setup];
   refPicture.imageViewBinding = e->dpbViews[ref];
   VkVideoReferenceSlotInfoKHR setupSlot = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
   setupSlot.pNext = setupInfo;
   setupSlot.slotIndex = setup;
   setupSlot.pPictureResource = &setupPicture;
   VkVideoReferenceSlotInfoKHR refSlot = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
   refSlot.pNext = refInfo;
   refSlot.slotIndex = ref;
   refSlot.pPictureResource = &refPicture;

   /* record */
   VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   e->BeginCommandBuffer(e->cmd, &cbi);
   e->CmdResetQueryPool(e->cmd, e->query, 0, 1);
   VkImageMemoryBarrier2 images[2];
   uint32_t count = 0;
   images[count++] = image_barrier(e->src, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   if (!e->started)
      images[count++] = image_barrier(e->dpb, 0, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR);
   barrier(e, images, count);
   VkBufferImageCopy planes[2];
   memset(planes, 0, sizeof(planes));
   planes[0].bufferRowLength = e->codedWidth;
   planes[0].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
   planes[0].imageExtent = (VkExtent3D){e->codedWidth, e->codedHeight, 1};
   planes[1].bufferOffset = (VkDeviceSize)e->codedWidth * e->codedHeight * e->bytes;
   planes[1].bufferRowLength = e->codedWidth / 2;
   planes[1].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
   planes[1].imageExtent = (VkExtent3D){e->codedWidth / 2, e->codedHeight / 2, 1};
   e->CmdCopyBufferToImage(e->cmd, e->staging, e->src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, planes);
   images[0] = image_barrier(e->src, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR);
   barrier(e, images, 1);

   rate_control rc;
   VkVideoReferenceSlotInfoKHR beginSlots[2];
   uint32_t beginCount = 0;
   if (!idr)
      beginSlots[beginCount++] = refSlot;
   beginSlots[beginCount] = setupSlot;
   beginSlots[beginCount].slotIndex = -1;
   beginSlots[beginCount++].pNext = NULL;
   VkVideoBeginCodingInfoKHR begin = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
   begin.pNext = e->started ? (const void *)rc_build(e, &rc) : NULL;
   begin.videoSession = e->session;
   begin.videoSessionParameters = e->params;
   begin.referenceSlotCount = beginCount;
   begin.pReferenceSlots = beginSlots;
   e->CmdBeginVideoCodingKHR(e->cmd, &begin);
   if (!e->started) {
      VkVideoCodingControlInfoKHR control = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
      /* reset, rate control and the quality level together (as ffmpeg's Vulkan encoder does: NVIDIA applies the
       * rate control then) */
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR |
                      VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
      rc.quality = (VkVideoEncodeQualityLevelInfoKHR){VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR};
      rc.quality.pNext = rc_build(e, &rc);
      control.pNext = &rc.quality;
      e->CmdControlVideoCodingKHR(e->cmd, &control);
   }
   encode.dstBuffer = e->bitstream;
   encode.dstBufferOffset = 0;
   encode.dstBufferRange = e->bitstreamSize;
   encode.srcPictureResource = srcPicture;
   encode.pSetupReferenceSlot = &setupSlot;
   encode.referenceSlotCount = idr ? 0 : 1;
   encode.pReferenceSlots = idr ? NULL : &refSlot;
   e->CmdBeginQuery(e->cmd, e->query, 0, 0);
   e->CmdEncodeVideoKHR(e->cmd, &encode);
   e->CmdEndQuery(e->cmd, e->query, 0);
   VkVideoEndCodingInfoKHR end = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
   e->CmdEndVideoCodingKHR(e->cmd, &end);
   barrier(e, NULL, 0);
   e->EndCommandBuffer(e->cmd);

   VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
   submit.commandBufferCount = 1;
   submit.pCommandBuffers = &e->cmd;
   e->ResetFences(e->device, 1, &e->fence);
   VkResult r = e->QueueSubmit(e->queue, 1, &submit, e->fence);
   if (r == VK_SUCCESS)
      r = e->WaitForFences(e->device, 1, &e->fence, VK_TRUE, 10000000000ull);
   if (r != VK_SUCCESS) {
      amf_log("encode submission: %d", (int)r);
      return NULL;
   }
   /* offset, bytes written, status (32-bit values: what Venus gives for these) */
   int32_t result[3] = {0, 0, 0};
   r = e->GetQueryPoolResults(e->device, e->query, 0, 1, sizeof(result), result, sizeof(result),
                              VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
   if (r != VK_SUCCESS || result[2] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR || result[1] <= 0 ||
       (VkDeviceSize)result[0] + (VkDeviceSize)result[1] > e->bitstreamSize) {
      amf_log("encode feedback: result %d, offset %d, %d bytes, status %d", (int)r, result[0], result[1], result[2]);
      return NULL;
   }
   range_sync(e, e->bitstreamMemory, e->bitstreamCoherent, 1);

   size_t need = (idr ? e->headersSize : 0) + (size_t)result[1];
   if (need > e->outSize) {
      unsigned char *out = realloc(e->out, need);
      if (!out)
         return NULL;
      e->out = out;
      e->outSize = need;
   }
   size_t at = 0;
   if (idr) {
      memcpy(e->out, e->headers, e->headersSize);
      at = e->headersSize;
   }
   memcpy(e->out + at, e->bitstreamMap + result[0], (size_t)result[1]);
   *size = need;
   *idrOut = idr;

   e->started = 1;
   e->slot = ref;
   e->sinceIdr++;
   e->frame++;
   return e->out;
}
