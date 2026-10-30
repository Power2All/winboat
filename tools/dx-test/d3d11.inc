// Direct3D 11: devices and optional features, format support, rendering features, and the shader model 4/5,
// compute, tessellation and MSAA tests of the driver's development (d3d11/). Rendering is compared with WARP.

// Readable names of the development tests
static std::string sm5_title(const std::string &name)
{
   static const char *titles[][2] = {
      {"idx_tables", "Lookup tables and local arrays indexed at run time"},
      {"cf_calls", "Subroutine calls ([call] switch)"},
      {"cf_loop_switch", "Loops with continue, switch statements"},
      {"cf_nested", "Nested loops with break and continue"},
      {"cf_sm4", "Control flow in shader model 4.0"},
      {"tex_sample", "Sample, offsets, SampleLevel, SampleBias, SampleGrad"},
      {"tex_pairs", "Textures and samplers in combinations, texture arrays"},
      {"tex_int_dims", "Integer textures, Load with offsets, GetDimensions"},
      {"tex_cmp", "Comparison sampling (shadow maps)"},
      {"tex_gather", "Gather (GatherRed/Green/Alpha, GatherCmp, offsets)"},
      {"tex_cube", "Cube map sampling"},
      {"tex_lod", "CalculateLevelOfDetail"},
      {"alu_bits", "Integer and bit operations"},
      {"alu_half", "Half precision conversions (f32tof16, f16tof32)"},
      {"buf_ps", "Typed buffers (Buffer<T>) in pixel shaders"},
      {"buf_ps_mismatch", "Typed buffers read through another integer type"},
      {"buf_vs", "Typed buffers in vertex shaders"},
      {"sb_vs", "Structured and raw buffers in vertex shaders"},
      {"alu_misc", "Derivatives (ddx/ddy fine and coarse) and math functions"},
      {"cs_basic", "Compute shaders: thread IDs and buffer writes"},
      {"cs_shared", "Group shared memory and barriers"},
      {"cs_atomics", "Atomic operations"},
      {"cs_structured_raw", "Structured and raw (byte address) buffers"},
      {"cs_append", "Append buffers"},
      {"cs_consume", "Consume buffers"},
      {"cs_texture", "Texture reads in compute shaders"},
      {"cs_indirect", "DispatchIndirect"},
      {"ps_uav", "UAVs written by pixel shaders"},
      {"uav_clear", "ClearUnorderedAccessView (uint and float)"},
      {"cs_uav_textures", "UAV textures (1D, 2D, arrays, 3D)"},
      {"cs_typed_load", "Typed UAV loads"},
      {"cs_typed_load_2d", "Typed UAV loads from 2D textures"},
      {"cs_typed_load_3d_only", "Typed UAV loads from 3D textures"},
      {"tex3d_roundtrip", "3D texture upload and read-back"},
      {"cs_big_groups", "Large thread groups (1024 threads, 3D groups)"},
      {"cs_indirect_offset", "DispatchIndirect with an argument offset"},
      {"ps_uav_structured", "Structured UAVs written by pixel shaders"},
      {"tess_quad", "Quad domain, control points and patch constant phases"},
      {"tess_tri_fractional", "Triangle domain, fractional_odd partitioning"},
      {"tess_passthrough", "Hull shader passing the control points through"},
      {"tess_isoline", "Isolines"},
      {"tess_points", "Point output"},
      {"tess_two_patches", "Two patches per draw, SV_PrimitiveID"},
      {"indirect_draw", "DrawInstancedIndirect"},
      {"indirect_indexed", "DrawIndexedInstancedIndirect"},
      {"indirect_indexed_ib_offset", "DrawIndexedInstancedIndirect with an index buffer offset"},
      {"indirect_args_from_cs", "Indirect draw arguments written by a compute shader"},
      {"depth_d24s8", "Depth buffer (D24S8)"},
      {"scissor", "Scissor rectangles"},
      {"scissor_msaa", "Scissor rectangles on a 4x MSAA target"},
      {"depth_d24s8_srv", "Depth buffer read in a shader (R24_UNORM_X8_TYPELESS)"},
      {"msaa_edges", "MSAA 4x: anti-aliased edges and ResolveSubresource"},
      {"msaa_depth", "MSAA 4x with a depth buffer"},
      {"msaa_sample_mask", "MSAA sample mask"},
      {"msaa_coverage_out", "MSAA coverage output (SV_Coverage)"},
      {"msaa_per_sample", "Per-sample shading (SV_SampleIndex) and Texture2DMS loads"},
      {"msaa_sample_info", "Coverage input and GetDimensions of a Texture2DMS"},
      {"msaa_quality_levels", "MSAA quality levels (2x to 16x, color and depth formats)"},
      {"msaa_swapchain", "4x multisampled swap chain presented to a window"},
   };
   std::string base = name, suffix;
   size_t cull = name.find("_cull_back");
   if (cull != std::string::npos) {
      base = name.substr(0, cull);
      suffix = " (back faces culled: winding)";
   }
   for (auto &t : titles) {
      if (base == t[0]) return t[1] + suffix;
   }
   return name;
}

static void sm5_result(bool pass, const std::string &name, const std::string &detail)
{
   check(pass, sm5_title(name), detail);
}

#include "d3d11/sm5core.inc"
#include "d3d11/cstests.inc"
#include "d3d11/tesstests.inc"
#include "d3d11/drawtests.inc"

// The GPU device (WARP with --warp) and the WARP reference, as the development tests set them up
static bool setupPair(Dev &hw, Dev &warp)
{
   bool ok = setup(hw, g_warp);
   // (the test inputs are compiled shaders: Windows 7 without KB4019990 has no compiler, a skip rather than a failure)
   if (!ok) checkAtLevel(D3D_FEATURE_LEVEL_11_0, false, "Create the Direct3D 11 device (feature level 11_0) with the test inputs",
                         haveHlslCompiler() ? "" : NO_HLSL_COMPILER);
   if (!setup(warp, true)) {
      report(Status::Skip, "The tests of this group", haveHlslCompiler() ? "WARP (the reference) failed" : NO_HLSL_COMPILER);
      return false;
   }
   return ok;
}

static void groupD3D11Shaders()
{
   Dev hw, warp;
   bool hw_ok = setupPair(hw, warp);
   if (!warp.dev) return;
   for (const Test &t : tests) {
      if (!startTest(sm5_title(t.name))) continue;
      std::vector<uint32_t> a, b;
      bool ok = hw_ok && run(hw, t, a);
      if (!run(warp, t, b)) {
         report(Status::Skip, sm5_title(t.name), "WARP failed");
         continue;
      }
      int bad = 0;
      if (ok) hashOutputs(sm5_title(t.name), a, b);
      for (int i = 0; ok && i < W * H * 4; i++) {
         bool same;
         if (t.uint_out) {
            same = (a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]) <= (uint32_t)t.tolerance;
         } else {
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            same = (std::isnan(fa) && std::isnan(fb)) || fabsf(fa - fb) <= t.tolerance * (1 + fabsf(fb));
         }
         bad += !same;
      }
      sm5_result(ok && !bad, t.name,
                 !ok ? "the GPU run failed (see the shader's translation)" : bad ? fmt("%d of %d values differ from WARP's", bad, W * H * 4) : "");
   }
}

static void groupD3D11Compute()
{
   Dev hw, warp;
   bool hw_ok = setupPair(hw, warp);
   if (!warp.dev) return;
   int total = 0;
   cs_compare_all(hw, hw_ok, warp, nullptr, total);
}

static void groupD3D11Tess()
{
   Dev hw, warp;
   bool hw_ok = setupPair(hw, warp);
   if (!warp.dev) return;
   int total = 0;
   tess_compare_all(hw, hw_ok, warp, nullptr, total);
}

static void groupD3D11Msaa()
{
   Dev hw, warp;
   bool hw_ok = setupPair(hw, warp);
   if (!warp.dev) return;
   int total = 0;
   draw_compare_all(hw, hw_ok, warp, nullptr, total);
}

// ---------------------------------------------------------------------------------------------------------------
// Devices, feature levels, optional features

static const char *flName(D3D_FEATURE_LEVEL fl)
{
   switch (fl) {
   case D3D_FEATURE_LEVEL_9_1: return "9_1";
   case D3D_FEATURE_LEVEL_9_2: return "9_2";
   case D3D_FEATURE_LEVEL_9_3: return "9_3";
   case D3D_FEATURE_LEVEL_10_0: return "10_0";
   case D3D_FEATURE_LEVEL_10_1: return "10_1";
   case D3D_FEATURE_LEVEL_11_0: return "11_0";
   case D3D_FEATURE_LEVEL_11_1: return "11_1";
   case D3D_FEATURE_LEVEL_12_0: return "12_0";
   case D3D_FEATURE_LEVEL_12_1: return "12_1";
   case D3D_FEATURE_LEVEL_12_2: return "12_2";
   default: return "?";
   }
}

static HRESULT createDevice11(ID3D11Device **dev, ID3D11DeviceContext **ctx, const D3D_FEATURE_LEVEL *levels, UINT count,
                              D3D_FEATURE_LEVEL *got = nullptr, UINT flags = 0)
{
   return D3D11CreateDevice(nullptr, g_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                            count, D3D11_SDK_VERSION, dev, got, ctx);
}

static void groupD3D11Device()
{
   const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                    D3D_FEATURE_LEVEL_9_1};
   for (int i = (int)ARRAYSIZE(all) - 1; i >= 0; i--) {
      ID3D11Device *dev = nullptr;
      HRESULT h = createDevice11(&dev, nullptr, &all[i], 1);
      std::string name = fmt("Feature level %s", flName(all[i]));
      if (all[i] == D3D_FEATURE_LEVEL_11_1) optional(SUCCEEDED(h), name, SUCCEEDED(h) ? "" : "not offered (optional)");
      else checkAtLevel(all[i], SUCCEEDED(h), name, SUCCEEDED(h) ? "" : hrs(h));
      release(dev);
   }
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
   HRESULT h = createDevice11(&dev, &ctx, all, ARRAYSIZE(all), &got, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
   // (Windows 7's runtime refuses a list with 11_1 in it: programs ask again without, as here)
   if (h == E_INVALIDARG) h = createDevice11(&dev, &ctx, all + 1, ARRAYSIZE(all) - 1, &got, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
   checkAtLevel(D3D_FEATURE_LEVEL_11_0, SUCCEEDED(h) && got >= D3D_FEATURE_LEVEL_11_0, "Applications get feature level 11_0 or higher",
         SUCCEEDED(h) ? fmt("an application asking for the best gets %s", flName(got)) : hrs(h));
   if (!dev) return;
   check(true, "BGRA support (Direct2D interop)");

   D3D11_FEATURE_DATA_THREADING th = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof th);
   report(Status::Info, "Threading", fmt("concurrent resource creation by the driver: %s, command lists by the driver: %s "
                                         "(the runtime provides both otherwise)",
                                         th.DriverConcurrentCreates ? "yes" : "no", th.DriverCommandLists ? "yes" : "no"));
   D3D11_FEATURE_DATA_DOUBLES dbl = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof dbl);
   optional(dbl.DoublePrecisionFloatShaderOps, "Double precision in shaders");
   D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x10 = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x10, sizeof x10);
   report(Status::Info, "Compute shaders on feature level 10_x devices",
          x10.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x ? "yes" : "no");

   D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
   if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o))) {
      const std::pair<const char *, BOOL> opts[] = {
         {"11.1: logic operations in the output merger", o.OutputMergerLogicOp},
         {"11.1: UAV-only rendering with a forced sample count", o.UAVOnlyRenderingForcedSampleCount},
         {"11.1: Discard APIs seen by the driver", o.DiscardAPIsSeenByDriver},
         {"11.1: update and copy flags seen by the driver", o.FlagsForUpdateAndCopySeenByDriver},
         {"11.1: ClearView", o.ClearView},
         {"11.1: copies with overlapping regions", o.CopyWithOverlap},
         {"11.1: partial constant buffer updates", o.ConstantBufferPartialUpdate},
         {"11.1: constant buffer offsetting", o.ConstantBufferOffsetting},
         {"11.1: Map NO_OVERWRITE on dynamic constant buffers", o.MapNoOverwriteOnDynamicConstantBuffer},
         {"11.1: Map NO_OVERWRITE on dynamic buffer views", o.MapNoOverwriteOnDynamicBufferSRV},
         {"11.1: multisampled RTV with a forced sample count of one", o.MultisampleRTVWithForcedSampleCountOne},
         {"11.1: SAD4 shader instructions", o.SAD4ShaderInstructions},
         {"11.1: extended double precision instructions", o.ExtendedDoublesShaderInstructions},
         {"11.1: extended resource sharing", o.ExtendedResourceSharing},
      };
      for (auto &opt : opts) optional(opt.second, opt.first);
   }
   D3D11_FEATURE_DATA_ARCHITECTURE_INFO arch = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_ARCHITECTURE_INFO, &arch, sizeof arch);
   report(Status::Info, "Tile-based deferred renderer", arch.TileBasedDeferredRenderer ? "yes" : "no");
   D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT mp = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &mp, sizeof mp);
   report(Status::Info, "Minimum precision (min16float)",
          mp.PixelShaderMinPrecision ? "lower precision used" : "full 32-bit precision (allowed)");
   D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT sh = {};
   dev->CheckFeatureSupport(D3D11_FEATURE_D3D9_SHADOW_SUPPORT, &sh, sizeof sh);
   report(Status::Info, "Shadow maps on feature level 9_x", sh.SupportsDepthAsTextureWithLessEqualComparisonFilter ? "yes" : "no");
   D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
   if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1))) {
      optional(o1.TiledResourcesTier != D3D11_TILED_RESOURCES_NOT_SUPPORTED, "11.2: tiled resources");
      optional(o1.MinMaxFiltering, "11.2: min/max filtering");
      optional(o1.ClearViewAlsoSupportsDepthOnlyFormats, "11.2: ClearView on depth formats");
      optional(o1.MapOnDefaultBuffers, "11.2: Map on default buffers");
   }
   D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
   if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2))) {
      optional(o2.PSSpecifiedStencilRefSupported, "11.3: stencil reference from pixel shaders");
      optional(o2.TypedUAVLoadAdditionalFormats, "11.3: typed UAV loads of more formats");
      optional(o2.ROVsSupported, "11.3: rasterizer ordered views");
      optional(o2.ConservativeRasterizationTier != D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED, "11.3: conservative rasterization");
      optional(o2.MapOnDefaultTextures, "11.3: Map on default textures");
      optional(o2.StandardSwizzle, "11.3: standard swizzle layouts");
      report(Status::Info, "Unified memory architecture", o2.UnifiedMemoryArchitecture ? "yes" : "no");
   }
   D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = {};
   if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof o3))) {
      optional(o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer, "11.3: viewport/render target index from any shader");
   }
   D3D11_FEATURE_DATA_D3D11_OPTIONS4 o4 = {};
   if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &o4, sizeof o4))) {
      optional(o4.ExtendedNV12SharedTextureSupported, "11.4: shared NV12 textures");
   }

   // Deferred contexts, video, sharing
   ID3D11DeviceContext *deferred = nullptr;
   h = dev->CreateDeferredContext(0, &deferred);
   check(SUCCEEDED(h), "Deferred contexts", SUCCEEDED(h) ? (th.DriverCommandLists ? "by the driver" : "emulated by the runtime") : hrs(h));
   release(deferred);
   ID3D11VideoDevice *video = nullptr;
   dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)&video);
   optional(video != nullptr, "Video decoding and processing (ID3D11VideoDevice)", video ? "" : "not offered (no video acceleration)");
   release(video);
   ID3D11Device1 *dev1 = nullptr;
   dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
   if (!dev1 && isWindows7())
      optional(false, "Direct3D 11.1 interfaces (ID3D11Device1)", "Windows 7: they come with its Platform Update (KB2670838)");
   else
      check(dev1 != nullptr, "Direct3D 11.1 interfaces (ID3D11Device1)");
   release(dev1);
   ctx->Release();
   dev->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Format support, compared with WARP's

static std::string formatBits(UINT bits)
{
   static const std::pair<UINT, const char *> names[] = {
      {D3D11_FORMAT_SUPPORT_BUFFER, "buffer"}, {D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER, "vertex buffer"},
      {D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER, "index buffer"}, {D3D11_FORMAT_SUPPORT_SO_BUFFER, "stream output"},
      {D3D11_FORMAT_SUPPORT_TEXTURE1D, "1D"}, {D3D11_FORMAT_SUPPORT_TEXTURE2D, "2D"},
      {D3D11_FORMAT_SUPPORT_TEXTURE3D, "3D"}, {D3D11_FORMAT_SUPPORT_TEXTURECUBE, "cube"},
      {D3D11_FORMAT_SUPPORT_SHADER_LOAD, "load"}, {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE, "sample"},
      {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON, "comparison"}, {D3D11_FORMAT_SUPPORT_MIP, "mips"},
      {D3D11_FORMAT_SUPPORT_MIP_AUTOGEN, "mip generation"}, {D3D11_FORMAT_SUPPORT_RENDER_TARGET, "render target"},
      {D3D11_FORMAT_SUPPORT_BLENDABLE, "blending"}, {D3D11_FORMAT_SUPPORT_DEPTH_STENCIL, "depth/stencil"},
      {D3D11_FORMAT_SUPPORT_CPU_LOCKABLE, "CPU access"}, {D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE, "MSAA resolve"},
      {D3D11_FORMAT_SUPPORT_DISPLAY, "display"}, {D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT, "casts"},
      {D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, "MSAA render target"},
      {D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD, "MSAA load"}, {D3D11_FORMAT_SUPPORT_SHADER_GATHER, "gather"},
      {D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST, "back buffer cast"},
      {D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW, "typed UAV"},
      {D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON, "gather comparison"},
      {D3D11_FORMAT_SUPPORT_DECODER_OUTPUT, "video decoding"}, {D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT, "video processor output"},
      {D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT, "video processor input"}, {D3D11_FORMAT_SUPPORT_VIDEO_ENCODER, "video encoding"},
   };
   std::string s;
   for (auto &n : names) {
      if (bits & n.first) s += (s.empty() ? "" : ", ") + std::string(n.second);
      bits &= ~n.first;
   }
   if (bits) s += (s.empty() ? "" : ", ") + fmt("0x%x", bits);
   return s;
}

static void groupD3D11Formats()
{
   const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   ID3D11Device *dev = nullptr, *ref = nullptr;
   HRESULT h = createDevice11(&dev, nullptr, &fl, 1);
   D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &ref, nullptr, nullptr);
   if (!dev || !ref) {
      if (dev) check(false, "Feature level 11_0 devices (GPU and WARP)", "no WARP");
      else checkAtLevel(D3D_FEATURE_LEVEL_11_0, false, "Feature level 11_0 devices (GPU and WARP)", hrs(h));
      return;
   }
   // Bits an application may rely on at 11_0 for a format that has them on WARP; the rest are optional
   const UINT optionalBits = D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON |
                             D3D11_FORMAT_SUPPORT_DECODER_OUTPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT |
                             D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT | D3D11_FORMAT_SUPPORT_VIDEO_ENCODER |
                             D3D11_FORMAT_SUPPORT_DISPLAY | D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST;
   const std::pair<DXGI_FORMAT, const char *> formats[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"}, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB"},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"}, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB"},
      {DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM"}, {DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT"},
      {DXGI_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM"}, {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM"},
      {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT"}, {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, "R9G9B9E5_SHAREDEXP"},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"}, {DXGI_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM"},
      {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT"}, {DXGI_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT"},
      {DXGI_FORMAT_R32G32B32_FLOAT, "R32G32B32_FLOAT"}, {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT"},
      {DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM"}, {DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT"},
      {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT"}, {DXGI_FORMAT_R32_UINT, "R32_UINT"}, {DXGI_FORMAT_R32_SINT, "R32_SINT"},
      {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT"}, {DXGI_FORMAT_R16_UNORM, "R16_UNORM"}, {DXGI_FORMAT_R16_UINT, "R16_UINT"},
      {DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM"}, {DXGI_FORMAT_R8_UNORM, "R8_UNORM"}, {DXGI_FORMAT_R8_UINT, "R8_UINT"},
      {DXGI_FORMAT_A8_UNORM, "A8_UNORM"}, {DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM"},
      {DXGI_FORMAT_B5G5R5A1_UNORM, "B5G5R5A1_UNORM"}, {DXGI_FORMAT_B4G4R4A4_UNORM, "B4G4R4A4_UNORM"},
      {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"}, {DXGI_FORMAT_D32_FLOAT, "D32_FLOAT"},
      {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32_FLOAT_S8X24_UINT"}, {DXGI_FORMAT_D16_UNORM, "D16_UNORM"},
      {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, "R24_UNORM_X8_TYPELESS"}, {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "R32_FLOAT_X8X24_TYPELESS"},
      {DXGI_FORMAT_BC1_UNORM, "BC1_UNORM"}, {DXGI_FORMAT_BC1_UNORM_SRGB, "BC1_UNORM_SRGB"}, {DXGI_FORMAT_BC2_UNORM, "BC2_UNORM"},
      {DXGI_FORMAT_BC3_UNORM, "BC3_UNORM"}, {DXGI_FORMAT_BC4_UNORM, "BC4_UNORM"}, {DXGI_FORMAT_BC5_UNORM, "BC5_UNORM"},
      {DXGI_FORMAT_BC5_SNORM, "BC5_SNORM"}, {DXGI_FORMAT_BC6H_UF16, "BC6H_UF16"}, {DXGI_FORMAT_BC6H_SF16, "BC6H_SF16"},
      {DXGI_FORMAT_BC7_UNORM, "BC7_UNORM"}, {DXGI_FORMAT_BC7_UNORM_SRGB, "BC7_UNORM_SRGB"},
      {DXGI_FORMAT_NV12, "NV12 (video)"}, {DXGI_FORMAT_YUY2, "YUY2 (video)"},
   };
   for (auto &f : formats) {
      UINT a = 0, b = 0;
      dev->CheckFormatSupport(f.first, &a);
      ref->CheckFormatSupport(f.first, &b);
      UINT missing = b & ~a;
      UINT optionalHere = optionalBits;
      // optional at 11_0 beyond buffers: 96-bit textures, the 11.1 format B4G4R4A4, video formats
      if (f.first == DXGI_FORMAT_R32G32B32_FLOAT) optionalHere |= ~(D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER);
      if (f.first == DXGI_FORMAT_B4G4R4A4_UNORM || f.first == DXGI_FORMAT_NV12 || f.first == DXGI_FORMAT_YUY2) optionalHere = ~0u;
      UINT missingRequired = missing & ~optionalHere;
      std::string name = fmt("Format %s", f.second);
      if (missingRequired) check(false, name, "missing: " + formatBits(missingRequired));
      else if (missing) optional(false, name, "optional, missing: " + formatBits(missing));
      else check(true, name, formatBits(a));
   }
   dev->Release();
   ref->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Rendering features, each run on the GPU and on WARP and compared

static const int RW = 32, RH = 32;

struct RT11 {
   ID3D11Texture2D *tex = nullptr, *staging = nullptr;
   ID3D11RenderTargetView *rtv = nullptr;
   DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
   UINT bytes = 4;
   void release()
   {
      ::release(rtv);
      ::release(tex);
      ::release(staging);
   }
};

static bool makeRT(Dev &d, RT11 &t, DXGI_FORMAT format, UINT bytes, const float clear[4], UINT w = RW, UINT h = RH)
{
   t.format = format;
   t.bytes = bytes;
   D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
   if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &t.tex))) {
      td.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &t.tex))) return false;
   }
   td.BindFlags = 0;
   td.Usage = D3D11_USAGE_STAGING;
   td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &t.staging))) return false;
   if (FAILED(d.dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) return false;
   d.ctx->ClearRenderTargetView(t.rtv, clear);
   d.ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
   D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
   d.ctx->RSSetViewports(1, &vp);
   return true;
}

// The raw bytes of a texture subresource (copied to a staging texture of the same size and format)
static bool readTexture(Dev &d, ID3D11Texture2D *tex, UINT sub, std::vector<uint32_t> &out)
{
   D3D11_TEXTURE2D_DESC td;
   tex->GetDesc(&td);
   UINT mips = td.MipLevels, level = sub % mips;
   UINT w = std::max(1u, td.Width >> level), h = std::max(1u, td.Height >> level);
   D3D11_TEXTURE2D_DESC sd = {w, h, 1, 1, td.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
   ID3D11Texture2D *st = nullptr;
   if (FAILED(d.dev->CreateTexture2D(&sd, nullptr, &st))) return false;
   d.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, sub, nullptr);
   D3D11_MAPPED_SUBRESOURCE m;
   bool ok = SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
   if (ok) {
      UINT rowBytes = (UINT)(m.RowPitch);
      UINT bpp = 4;
      switch (td.Format) {
      case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT: bpp = 8; break;
      case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: bpp = 16; break;
      case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: bpp = 1; break;
      case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_B5G6R5_UNORM:
      case DXGI_FORMAT_D16_UNORM: bpp = 2; break;
      default: break;
      }
      UINT row = w * bpp;
      std::vector<uint8_t> bytes(row * h);
      for (UINT y = 0; y < h; y++) memcpy(&bytes[y * row], (char *)m.pData + y * rowBytes, row);
      out.assign((bytes.size() + 3) / 4, 0);
      memcpy(out.data(), bytes.data(), bytes.size());
      d.ctx->Unmap(st, 0);
   }
   st->Release();
   return ok;
}

static ID3D11PixelShader *ps11(Dev &d, const std::string &src, std::string &detail, const char *profile = "ps_5_0")
{
   ID3DBlob *b = compileHlsl(src, profile, &detail);
   if (!b) return nullptr;
   ID3D11PixelShader *s = nullptr;
   HRESULT h = d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
   if (FAILED(h)) detail = "CreatePixelShader: " + hrs(h);
   b->Release();
   return s;
}

static ID3D11VertexShader *vs11(Dev &d, const std::string &src, std::string &detail, ID3DBlob **keep = nullptr)
{
   ID3DBlob *b = compileHlsl(src, "vs_5_0", &detail);
   if (!b) return nullptr;
   ID3D11VertexShader *s = nullptr;
   HRESULT h = d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
   if (FAILED(h)) detail = "CreateVertexShader: " + hrs(h);
   if (keep) *keep = b;
   else b->Release();
   return s;
}

static ID3D11GeometryShader *gs11(Dev &d, const std::string &src, std::string &detail)
{
   ID3DBlob *b = compileHlsl(src, "gs_5_0", &detail);
   if (!b) return nullptr;
   ID3D11GeometryShader *s = nullptr;
   HRESULT h = d.dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
   if (FAILED(h)) detail = "CreateGeometryShader: " + hrs(h);
   b->Release();
   return s;
}

static void fullscreen11(Dev &d, ID3D11PixelShader *ps)
{
   d.ctx->IASetInputLayout(nullptr);
   d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.ctx->VSSetShader(d.vs, nullptr, 0);
   d.ctx->GSSetShader(nullptr, nullptr, 0);
   d.ctx->HSSetShader(nullptr, nullptr, 0);
   d.ctx->DSSetShader(nullptr, nullptr, 0);
   d.ctx->PSSetShader(ps, nullptr, 0);
   d.ctx->Draw(3, 0);
}

static ID3D11Texture2D *tex11(Dev &d, UINT w, UINT h, DXGI_FORMAT format, const void *data, UINT pitch,
                              UINT bind = D3D11_BIND_SHADER_RESOURCE, UINT arraySize = 1, UINT mips = 1, UINT misc = 0)
{
   D3D11_TEXTURE2D_DESC td = {w, h, mips, arraySize, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, misc};
   std::vector<D3D11_SUBRESOURCE_DATA> init(arraySize * mips);
   for (UINT i = 0; i < arraySize * mips; i++) init[i] = {(const char *)data + (i / mips) * pitch * h, pitch, pitch * h};
   ID3D11Texture2D *t = nullptr;
   d.dev->CreateTexture2D(&td, data ? init.data() : nullptr, &t);
   return t;
}

static ID3D11Buffer *buf11(Dev &d, UINT bind, const void *data, UINT size, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                           UINT cpu = 0, UINT misc = 0)
{
   D3D11_BUFFER_DESC bd = {size, usage, bind, cpu, misc, 0};
   D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
   ID3D11Buffer *b = nullptr;
   d.dev->CreateBuffer(&bd, data ? &init : nullptr, &b);
   return b;
}

// The bytes of a buffer (through a staging copy)
static bool readBuffer11(Dev &d, ID3D11Buffer *b, UINT size, std::vector<uint32_t> &out)
{
   ID3D11Buffer *st = buf11(d, 0, nullptr, size, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
   if (!st) return false;
   d.ctx->CopyResource(st, b);
   D3D11_MAPPED_SUBRESOURCE m;
   bool ok = SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
   if (ok) {
      out.assign(size / 4, 0);
      memcpy(out.data(), m.pData, size);
      d.ctx->Unmap(st, 0);
   }
   st->Release();
   return ok;
}

// A rasterizer state with 11.1's ForcedSampleCount (target-independent rasterization)
static ID3D11RasterizerState1 *forcedRaster11(Dev &d, UINT samples)
{
   ID3D11Device1 *dev1 = nullptr;
   d.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
   if (!dev1) return nullptr;
   D3D11_RASTERIZER_DESC1 rd = {};
   rd.FillMode = D3D11_FILL_SOLID;
   rd.CullMode = D3D11_CULL_NONE;
   rd.DepthClipEnable = TRUE;
   rd.ForcedSampleCount = samples;
   ID3D11RasterizerState1 *rs = nullptr;
   dev1->CreateRasterizerState1(&rd, &rs);
   dev1->Release();
   return rs;
}

// A triangle with edges crossing pixels at odd angles (vertices from SV_VertexID)
static const char *const TRIANGLE_VS = "float4 main(uint id : SV_VertexID) : SV_Position { "
                                        "float2 v[3] = {float2(-0.9, -0.8), float2(0.7, 0.95), float2(0.95, -0.6)}; "
                                        "return float4(v[id], 0.5, 1); }";

// Target-independent rasterization: the triangle with a forced sample count, each pixel's covered samples as red
static bool tirTest(Dev &d, std::vector<uint32_t> &o, std::string &e, UINT samples)
{
   if (d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
      e = "needs feature level 11_1 (optional)";
      return false;
   }
   ID3D11RasterizerState1 *rs = forcedRaster11(d, samples);
   ID3D11VertexShader *vs = vs11(d, TRIANGLE_VS, e);
   char src[160];
   snprintf(src, sizeof(src), "float4 main(float4 p : SV_Position, uint c : SV_Coverage) : SV_Target "
                              "{ return float4(countbits(c) / %u.0, 0, 0, 1); }", samples);
   ID3D11PixelShader *ps = ps11(d, src, e);
   RT11 rt;
   const float clear[4] = {0, 0, 0, 0};
   bool ok = rs && vs && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
   if (ok) {
      d.ctx->RSSetState(rs);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.ctx->VSSetShader(vs, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      d.ctx->Draw(3, 0);
      ok = readTexture(d, rt.tex, 0, o);
   }
   rt.release();
   release(rs);
   release(vs);
   release(ps);
   return ok;
}

// Deterministic "random" bytes
static void fillPattern(std::vector<uint8_t> &v, uint32_t seed)
{
   for (auto &b : v) {
      seed = seed * 1664525u + 1013904223u;
      b = (uint8_t)(seed >> 24);
   }
}

static void resetCtx(Dev &d)
{
   d.ctx->ClearState();
}

typedef bool (*RenderFn)(Dev &d, std::vector<uint32_t> &out, std::string &detail);

// A texture of the given format with pattern data, read by Load at 4x4 pixels per texel into an RGBA8 target
static bool sampleFormat(Dev &d, std::vector<uint32_t> &out, std::string &detail, DXGI_FORMAT format, UINT texelBytes,
                         bool blocks, uint32_t seed, const std::function<void(std::vector<uint8_t> &)> &fix = nullptr)
{
   const UINT size = 8;
   UINT pitch = blocks ? (size / 4) * texelBytes : size * texelBytes;
   UINT rows = blocks ? size / 4 : size;
   std::vector<uint8_t> data(pitch * rows);
   fillPattern(data, seed);
   if (fix) fix(data);
   ID3D11Texture2D *t = tex11(d, size, size, format, data.data(), pitch);
   ID3D11ShaderResourceView *srv = nullptr;
   if (!t || FAILED(d.dev->CreateShaderResourceView(t, nullptr, &srv))) {
      detail = "creating the texture failed";
      release(t);
      return false;
   }
   RT11 rt;
   const float black[4] = {0, 0, 0, 0};
   ID3D11PixelShader *ps = ps11(d, "Texture2D t : register(t0); float4 main(float4 p : SV_Position) : SV_Target "
                                   "{ return saturate(t.Load(int3(p.xy / 4, 0))); }", detail);
   bool ok = ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black);
   if (ok) {
      d.ctx->PSSetShaderResources(0, 1, &srv);
      fullscreen11(d, ps);
      ok = readTexture(d, rt.tex, 0, out);
   }
   rt.release();
   release(ps);
   srv->Release();
   t->Release();
   return ok;
}

static bool renderTargetFormat(Dev &d, std::vector<uint32_t> &out, std::string &detail, DXGI_FORMAT format, UINT bytes)
{
   UINT support = 0;
   d.dev->CheckFormatSupport(format, &support);
   if (!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
      detail = "not a render target format here";
      return false;
   }
   RT11 rt;
   const float clear[4] = {0.125f, 0.25f, 0.5f, 1};
   ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target "
                                   "{ return float4(p.x / 32, p.y / 32, 0.75, p.x < 16 ? 0.25 : 1); }", detail);
   bool ok = ps && makeRT(d, rt, format, bytes, clear);
   if (ok) {
      // the left quarter stays at the clear color
      D3D11_VIEWPORT vp = {8, 0, RW - 8, RH, 0, 1};
      d.ctx->RSSetViewports(1, &vp);
      fullscreen11(d, ps);
      ok = readTexture(d, rt.tex, 0, out);
   } else if (detail.empty()) {
      detail = "creating the render target failed";
   }
   rt.release();
   release(ps);
   return ok;
}

static bool blendTest(Dev &d, std::vector<uint32_t> &out, std::string &detail, const D3D11_BLEND_DESC &bd,
                      const char *ps, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, int targets = 1)
{
   RT11 rt[8];
   const float clear[4] = {0.2f, 0.4f, 0.6f, 0.8f};
   ID3D11PixelShader *p = ps11(d, ps, detail);
   ID3D11BlendState *bs = nullptr;
   HRESULT h = d.dev->CreateBlendState(&bd, &bs);
   if (FAILED(h)) detail = "CreateBlendState: " + hrs(h);
   bool ok = p && bs;
   ID3D11RenderTargetView *rtvs[8] = {};
   for (int i = 0; ok && i < targets; i++) {
      ok = makeRT(d, rt[i], format, 4, clear);
      rtvs[i] = rt[i].rtv;
   }
   if (ok) {
      d.ctx->OMSetRenderTargets(targets, rtvs, nullptr);
      d.ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
      fullscreen11(d, p);
      out.clear();
      for (int i = 0; ok && i < targets; i++) {
         std::vector<uint32_t> one;
         ok = readTexture(d, rt[i].tex, 0, one);
         out.insert(out.end(), one.begin(), one.end());
      }
   }
   for (auto &r : rt) r.release();
   release(p);
   release(bs);
   return ok;
}

static D3D11_BLEND_DESC blendDesc(D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD)
{
   D3D11_BLEND_DESC bd = {};
   for (auto &r : bd.RenderTarget) {
      r = {TRUE, src, dst, op, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
   }
   return bd;
}

static const char *TRI_VS = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
   float2 uv = float2((id << 1) & 2, id & 2);
   return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
)";

// 11.3: SV_RenderTargetArrayIndex and SV_ViewportArrayIndex written by the vertex shader (two instances) or, with
// tess, by the domain shader (two patches): each lands in its own slice and viewport
// Clip distances written by the domain shader, as Direct2D's curve tessellation on Windows 10 does: a tessellated
// triangle with SV_ClipDistance0 (four of them) cutting off a corner and a stripe
static bool clipFromDomainShader(Dev &d, std::vector<uint32_t> &o, std::string &e, UINT forcedSamples = 0)
{
   if (forcedSamples && d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
      e = "needs feature level 11_1 (optional)";
      return false;
   }
   const std::string pos = "float2 uv = float2((id << 1) & 2, id & 2); float4 p = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);\n";
   ID3D11VertexShader *vs = vs11(d, "float4 main(uint v : SV_VertexID) : POS { uint id = v % 3; " + pos + "return p; }", e);
   const char *pc = "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; }; struct V { float4 p : POS; };\n";
   ID3DBlob *hb = compileHlsl(std::string(pc) + "PC pc() { PC c; c.e[0] = c.e[1] = c.e[2] = 4; c.i = 4; return c; }\n"
                              "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] "
                              "[outputcontrolpoints(3)] [patchconstantfunc(\"pc\")]\n"
                              "V main(InputPatch<V, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }",
                              "hs_5_0", &e);
   ID3DBlob *db = compileHlsl(std::string(pc) +
                              "struct O { float4 p : SV_Position; float4 clip : SV_ClipDistance0; float4 c : COLOR; };\n"
                              "[domain(\"tri\")] O main(PC pc, float3 b : SV_DomainLocation, const OutputPatch<V, 3> v) "
                              "{ O o; o.p = v[0].p * b.x + v[1].p * b.y + v[2].p * b.z; "
                              "o.clip = float4(b.x - 0.1, 0.8 - b.y, 1, abs(b.z - 0.5) - 0.1); o.c = float4(b, 1); return o; }",
                              "ds_5_0", &e);
   ID3D11HullShader *hs = nullptr;
   ID3D11DomainShader *ds = nullptr;
   HRESULT h = S_OK;
   if (hb && FAILED(h = d.dev->CreateHullShader(hb->GetBufferPointer(), hb->GetBufferSize(), nullptr, &hs))) e = "CreateHullShader: " + hrs(h);
   if (db && FAILED(h = d.dev->CreateDomainShader(db->GetBufferPointer(), db->GetBufferSize(), nullptr, &ds))) e = "CreateDomainShader: " + hrs(h);
   release(hb);
   release(db);
   // with a forced sample count: each pixel's covered samples, as Direct2D accumulates coverage
   ID3D11PixelShader *ps = ps11(d, forcedSamples ? "float4 main(float4 p : SV_Position, uint c : SV_Coverage) : SV_Target "
                                                   "{ return float4(countbits(c) / 16.0, 0, 0, 1); }"
                                                 : "float4 main(float4 p : SV_Position, float4 clip : SV_ClipDistance0, float4 c : COLOR) : SV_Target { return c; }", e);
   ID3D11RasterizerState1 *rs = forcedSamples ? forcedRaster11(d, forcedSamples) : nullptr;
   // added up: a pixel the tessellated triangles share gets all its samples whatever order they come in
   ID3D11BlendState *bs = nullptr;
   if (forcedSamples) {
      D3D11_BLEND_DESC bd = blendDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE);
      d.dev->CreateBlendState(&bd, &bs);
   }
   RT11 rt;
   const float clear[4] = {0, 0.2f, 0.3f, 1};
   bool ok = vs && hs && ds && ps && (!forcedSamples || rs) && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
   if (ok) {
      d.ctx->RSSetState(rs);
      d.ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
      d.ctx->VSSetShader(vs, nullptr, 0);
      d.ctx->HSSetShader(hs, nullptr, 0);
      d.ctx->DSSetShader(ds, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      d.ctx->Draw(3, 0);
      d.ctx->HSSetShader(nullptr, nullptr, 0);
      d.ctx->DSSetShader(nullptr, nullptr, 0);
      d.ctx->RSSetState(nullptr);
      d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
      ok = readTexture(d, rt.tex, 0, o);
   }
   rt.release();
   release(bs);
   release(rs);
   release(ps);
   release(ds);
   release(hs);
   release(vs);
   return ok;
}

// Direct2D's curve patches on Windows 10, rebuilt: 5 control points (a cubic Bezier's 4 and the fan's center), the
// hull shader's edge factors 1, 1 (a forked loop writing an indexed output) and one from the curve's size, the domain
// shader fanning from the center to points on the curve
static bool d2dCurvePatch(Dev &d, std::vector<uint32_t> &o, std::string &e, bool coverage = false)
{
   // coverage: as Direct2D's coverage pass draws it, each pixel's sample mask XORed into R16_UINT, ForcedSampleCount 16
   D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
   d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts);
   if (coverage && (!opts.OutputMergerLogicOp || d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)) {
      e = "not supported (optional)";
      return false;
   }
   // the hull shader's inputs are the vertex shader's SV_Position and SV_ClipDistance0, as Direct2D's are
   const char *common = "struct VI { float4 p : SV_Position; float4 c : SV_ClipDistance0; };\n"
                        "struct V { float2 p : BEZIERPOS; float4 c : TEXCOORD; };\n"
                        "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n";
   ID3D11VertexShader *vs = vs11(d, std::string(common) +
                                 "VI main(uint id : SV_VertexID) { float2 cp[5] = {float2(-0.8, -0.7), float2(-0.6, 0.95), "
                                 "float2(0.6, 0.95), float2(0.8, -0.7), float2(0, -0.7)}; VI v; v.p = float4(cp[id], 0, 1); "
                                 "v.c = float4(1, 2, 3, 4); return v; }", e);
   ID3DBlob *hb = compileHlsl(std::string(common) +
                              "PC pc(InputPatch<VI, 5> ip) { PC c; c.e[0] = 1; c.e[1] = 1;\n"
                              "float2 mn = min(min(min(ip[1].p.xy, ip[0].p.xy), ip[2].p.xy), ip[3].p.xy), "
                              "mx = max(max(max(ip[1].p.xy, ip[0].p.xy), ip[2].p.xy), ip[3].p.xy);\n"
                              "float2 ext = abs(mx - mn) * 256; float m = max(ext.x, ext.y);\n"
                              "c.e[2] = m < 0.7 ? 2 : m < 2.3 ? 4 : m < 8 ? 8 : m < 30 ? 16 : m < 120 ? 32 : 64; c.i = 1; return c; }\n"
                              "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] "
                              "[outputcontrolpoints(5)] [patchconstantfunc(\"pc\")]\n"
                              "V main(InputPatch<VI, 5> ip, uint i : SV_OutputControlPointID) { V v; v.p = ip[i].p.xy; "
                              "v.c = ip[i].c; return v; }",
                              "hs_5_0", &e);
   ID3DBlob *db = compileHlsl(std::string(common) +
                              "struct O { float4 p : SV_Position; float4 clip : SV_ClipDistance0; };\n"
                              "[domain(\"tri\")] O main(PC pc, float3 b : SV_DomainLocation, const OutputPatch<V, 5> v) {\n"
                              "float t = b.x, s = 1 - t; float2 q = v[0].p * s * s * s + v[1].p * 3 * t * s * s + "
                              "v[2].p * 3 * t * t * s + v[3].p * t * t * t;\n"
                              "float4 c = v[0].c * s * s * s + v[1].c * 3 * t * s * s + v[2].c * 3 * t * t * s + v[3].c * t * t * t;\n"
                              "if (b.z > 0.1) { q = v[0].p; c = v[0].c; } if (b.z > 0.9) { q = v[4].p; c = v[4].c; }\n"
                              "O o; o.p = float4(q, 0, 1); o.clip = c; return o; }",
                              "ds_5_0", &e);
   ID3D11HullShader *hs = nullptr;
   ID3D11DomainShader *ds = nullptr;
   HRESULT h = S_OK;
   if (hb && FAILED(h = d.dev->CreateHullShader(hb->GetBufferPointer(), hb->GetBufferSize(), nullptr, &hs))) e = "CreateHullShader: " + hrs(h);
   if (db && FAILED(h = d.dev->CreateDomainShader(db->GetBufferPointer(), db->GetBufferSize(), nullptr, &ds))) e = "CreateDomainShader: " + hrs(h);
   release(hb);
   release(db);
   ID3D11PixelShader *ps = ps11(d, coverage ? "uint main(float4 p : SV_Position, uint c : SV_Coverage) : SV_Target { return c; }"
                                            : "float4 main(float4 p : SV_Position) : SV_Target { return float4(1, 0.5, 0, 1); }", e);
   D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE};
   rd.DepthClipEnable = TRUE;
   ID3D11RasterizerState *rs = nullptr;
   if (coverage) rs = forcedRaster11(d, 16);
   else d.dev->CreateRasterizerState(&rd, &rs);
   ID3D11BlendState1 *bs = nullptr;
   if (coverage) {
      ID3D11Device1 *dev1 = nullptr;
      d.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
      D3D11_BLEND_DESC1 bd = {};
      bd.RenderTarget[0].LogicOpEnable = TRUE;
      bd.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_XOR;
      bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      if (dev1) dev1->CreateBlendState1(&bd, &bs);
      release(dev1);
   }
   RT11 rt;
   const float clear[4] = {0, coverage ? 0 : 0.2f, coverage ? 0 : 0.3f, 1};
   bool ok = vs && hs && ds && ps && rs && (!coverage || bs) &&
             makeRT(d, rt, coverage ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8G8B8A8_UNORM, coverage ? 2 : 4, clear);
   if (ok) {
      d.ctx->RSSetState(rs);
      d.ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
      d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST);
      d.ctx->VSSetShader(vs, nullptr, 0);
      d.ctx->HSSetShader(hs, nullptr, 0);
      d.ctx->DSSetShader(ds, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      d.ctx->Draw(5, 0);
      d.ctx->HSSetShader(nullptr, nullptr, 0);
      d.ctx->DSSetShader(nullptr, nullptr, 0);
      d.ctx->RSSetState(nullptr);
      d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
      ok = readTexture(d, rt.tex, 0, o);
   }
   rt.release();
   release(bs);
   release(rs);
   release(ps);
   release(ds);
   release(hs);
   release(vs);
   return ok;
}

static bool layerFromAnyShader(Dev &d, std::vector<uint32_t> &o, std::string &e, bool tess)
{
   D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3 = {};
   d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof o3);
   if (!o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer) {
      e = "not supported (optional)";
      return false;
   }
   const char *out = "struct O { float4 p : SV_Position; float4 c : COLOR; uint rt : SV_RenderTargetArrayIndex; "
                     "uint vp : SV_ViewportArrayIndex; };\n";
   const std::string pos = "float2 uv = float2((id << 1) & 2, id & 2); float4 p = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);\n";
   ID3D11VertexShader *vs = nullptr;
   ID3D11HullShader *hs = nullptr;
   ID3D11DomainShader *ds = nullptr;
   if (!tess) {
      vs = vs11(d, std::string(out) + "O main(uint id : SV_VertexID, uint k : SV_InstanceID) { " + pos +
                   "O o; o.p = p; o.c = float4(k * 0.5, uv * 0.25, 1); o.rt = k * 2; o.vp = k; return o; }", e);
   } else {
      vs = vs11(d, "float4 main(uint v : SV_VertexID) : POS { uint id = v % 3; " + pos + "return p; }", e);
      const char *pc = "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; }; struct V { float4 p : POS; };\n";
      ID3DBlob *hb = compileHlsl(std::string(pc) + "PC pc() { PC c; c.e[0] = c.e[1] = c.e[2] = 1; c.i = 1; return c; }\n"
                                 "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] "
                                 "[outputcontrolpoints(3)] [patchconstantfunc(\"pc\")]\n"
                                 "V main(InputPatch<V, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }",
                                 "hs_5_0", &e);
      ID3DBlob *db = compileHlsl(std::string(pc) + out +
                                 "[domain(\"tri\")] O main(PC pc, float3 b : SV_DomainLocation, const OutputPatch<V, 3> v, "
                                 "uint k : SV_PrimitiveID) { O o; o.p = v[0].p * b.x + v[1].p * b.y + v[2].p * b.z; "
                                 "o.c = float4(k * 0.5, b.xy * 0.25, 1); o.rt = k * 2; o.vp = k; return o; }",
                                 "ds_5_0", &e);
      HRESULT h = S_OK;
      if (hb && FAILED(h = d.dev->CreateHullShader(hb->GetBufferPointer(), hb->GetBufferSize(), nullptr, &hs))) e = "CreateHullShader: " + hrs(h);
      if (db && FAILED(h = d.dev->CreateDomainShader(db->GetBufferPointer(), db->GetBufferSize(), nullptr, &ds))) e = "CreateDomainShader: " + hrs(h);
      release(hb);
      release(db);
   }
   ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }", e);
   ID3D11Texture2D *arr = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D11_BIND_RENDER_TARGET, 3);
   ID3D11RenderTargetView *rtv = nullptr;
   if (arr) d.dev->CreateRenderTargetView(arr, nullptr, &rtv);
   D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE};
   rd.DepthClipEnable = TRUE;
   ID3D11RasterizerState *rs = nullptr;
   d.dev->CreateRasterizerState(&rd, &rs);
   bool ok = vs && ps && rtv && rs && (!tess || (hs && ds));
   if (ok) {
      const float clear[4] = {0.1f, 0.2f, 0.3f, 1};
      d.ctx->ClearRenderTargetView(rtv, clear);
      d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
      D3D11_VIEWPORT vps[2] = {{0, 0, RW, RH, 0, 1}, {8, 4, 16, 24, 0, 1}};
      d.ctx->RSSetViewports(2, vps);
      d.ctx->RSSetState(rs);
      d.ctx->IASetPrimitiveTopology(tess ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      d.ctx->VSSetShader(vs, nullptr, 0);
      d.ctx->HSSetShader(hs, nullptr, 0);
      d.ctx->DSSetShader(ds, nullptr, 0);
      d.ctx->PSSetShader(ps, nullptr, 0);
      if (tess) d.ctx->Draw(6, 0);
      else d.ctx->DrawInstanced(3, 2, 0, 0);
      d.ctx->HSSetShader(nullptr, nullptr, 0);
      d.ctx->DSSetShader(nullptr, nullptr, 0);
      d.ctx->RSSetState(nullptr);
      std::vector<uint32_t> s0, s2;
      ok = readTexture(d, arr, 0, s0) && readTexture(d, arr, 2, s2);
      o = s0;
      o.insert(o.end(), s2.begin(), s2.end());
   }
   release(rs);
   release(rtv);
   release(arr);
   release(ps);
   release(ds);
   release(hs);
   release(vs);
   return ok;
}

// 11.2's tiled resources: the context's 11.2 interface where the driver has them (else an optional feature), a tile
// pool, and 64 KB of a tile's pattern
static bool tiled11(Dev &d, ID3D11DeviceContext2 **ctx2, std::string &e)
{
   D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
   d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
   if (o1.TiledResourcesTier == D3D11_TILED_RESOURCES_NOT_SUPPORTED ||
       FAILED(d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext2), (void **)ctx2))) {
      e = "not supported (optional)";
      return false;
   }
   return true;
}

static ID3D11Buffer *tilePool11(Dev &d, UINT tiles)
{
   D3D11_BUFFER_DESC bd = {tiles * 65536, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_TILE_POOL, 0};
   ID3D11Buffer *b = nullptr;
   d.dev->CreateBuffer(&bd, nullptr, &b);
   return b;
}

static ID3D11Texture2D *tiledTexture11(Dev &d, UINT w, UINT h, UINT mips, UINT bind)
{
   D3D11_TEXTURE2D_DESC td = {w, h, mips, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0,
                              D3D11_RESOURCE_MISC_TILED};
   ID3D11Texture2D *t = nullptr;
   d.dev->CreateTexture2D(&td, nullptr, &t);
   return t;
}

static std::vector<uint8_t> tilePattern(int seed)
{
   std::vector<uint8_t> v(65536);
   for (size_t i = 0; i < v.size(); i++) v[i] = (uint8_t)(i * 7 + seed * 31 + (i >> 9) * seed);
   return v;
}

struct RenderTest {
   const char *name;
   RenderFn fn;
   int tolerance;   // per byte (per 10-bit channel with rgb10a2)
   bool optionalFeature;
   bool rgb10a2;    // values are R10G10B10A2 pixels
};

static const RenderTest RENDER_TESTS[] = {
   {"Block-compressed textures: BC1", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_BC1_UNORM, 8, true, 1); }, 4},
   {"Block-compressed textures: BC2", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_BC2_UNORM, 16, true, 2); }, 4},
   {"Block-compressed textures: BC3", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_BC3_UNORM, 16, true, 3); }, 4},
   {"Block-compressed textures: BC4", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_BC4_UNORM, 8, true, 4); }, 8},
   {"Block-compressed textures: BC5", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_BC5_UNORM, 16, true, 5); }, 8},
   {"Block-compressed textures: BC6H (HDR)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       // mode 1 (2-bit mode 00) in every block: no reserved modes
       return sampleFormat(d, o, e, DXGI_FORMAT_BC6H_UF16, 16, true, 6, [](std::vector<uint8_t> &v) { for (size_t i = 0; i < v.size(); i += 16) v[i] &= ~3; });
    }, 2},
   {"Block-compressed textures: BC7", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       // mode 6 in every block
       return sampleFormat(d, o, e, DXGI_FORMAT_BC7_UNORM, 16, true, 7, [](std::vector<uint8_t> &v) { for (size_t i = 0; i < v.size(); i += 16) v[i] = 0x40; });
    }, 1},
   {"Packed formats: R11G11B10_FLOAT", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return sampleFormat(d, o, e, DXGI_FORMAT_R11G11B10_FLOAT, 4, false, 8, [](std::vector<uint8_t> &v) {
          for (size_t i = 0; i < v.size(); i += 4) { uint32_t x; memcpy(&x, &v[i], 4); x &= ~((1u << 10) | (1u << 21) | (1u << 31)); memcpy(&v[i], &x, 4); }
       });
    }, 1},
   {"Packed formats: R9G9B9E5_SHAREDEXP", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 4, false, 9); }, 1},
   {"Packed formats: R10G10B10A2_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_R10G10B10A2_UNORM, 4, false, 10); }, 1},
   {"Packed formats: B5G6R5_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_B5G6R5_UNORM, 2, false, 11); }, 1},
   {"Packed formats: B5G5R5A1_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_B5G5R5A1_UNORM, 2, false, 12); }, 1},
   {"Packed formats: B4G4R4A4_UNORM (11.1)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_B4G4R4A4_UNORM, 2, false, 13); }, 1, true},
   {"sRGB textures (R8G8B8A8_UNORM_SRGB)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return sampleFormat(d, o, e, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4, false, 14); }, 1},
   {"Render target format R8G8B8A8_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R8G8B8A8_UNORM, 4); }, 1},
   {"Render target format B8G8R8A8_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_B8G8R8A8_UNORM, 4); }, 1},
   {"Render target format R8G8B8A8_UNORM_SRGB", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4); }, 1},
   {"Render target format R10G10B10A2_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R10G10B10A2_UNORM, 4); }, 2, false, true},
   {"Render target format R11G11B10_FLOAT", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R11G11B10_FLOAT, 4); }, 4},
   {"Render target format R16G16B16A16_FLOAT (HDR)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R16G16B16A16_FLOAT, 8); }, 1},
   {"Render target format R32G32B32A32_FLOAT", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R32G32B32A32_FLOAT, 16); }, 1},
   {"Render target format R16G16_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R16G16_UNORM, 4); }, 2},
   {"Render target format R8_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_R8_UNORM, 1); }, 1},
   {"Render target format B5G6R5_UNORM", [](Dev &d, std::vector<uint32_t> &o, std::string &e) { return renderTargetFormat(d, o, e, DXGI_FORMAT_B5G6R5_UNORM, 2); }, 1, true},
   {"Alpha blending", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return blendTest(d, o, e, blendDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA),
                        "float4 main(float4 p : SV_Position) : SV_Target { return float4(0.9, p.x / 32, 0.1, p.y / 32); }");
    }, 1},
   {"Blend operations (subtract, min, max)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       std::vector<uint32_t> all;
       for (D3D11_BLEND_OP op : {D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX}) {
          std::vector<uint32_t> one;
          resetCtx(d);
          if (!blendTest(d, one, e, blendDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE, op),
                         "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.x / 32, 0.5, p.y / 32, 0.5); }")) return false;
          all.insert(all.end(), one.begin(), one.end());
       }
       o = all;
       return true;
    }, 1},
   {"Blending on an sRGB render target", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return blendTest(d, o, e, blendDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA),
                        "float4 main(float4 p : SV_Position) : SV_Target { return float4(0.8, p.x / 32, 0.1, 0.5); }",
                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    }, 2},
   {"Dual-source blending", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_BLEND_DESC bd = blendDesc(D3D11_BLEND_ONE, D3D11_BLEND_SRC1_COLOR);
       return blendTest(d, o, e, bd, "struct O { float4 a : SV_Target0; float4 b : SV_Target1; };"
                                     "O main(float4 p : SV_Position) { O o; o.a = float4(0.25, 0.125, 0, 0); "
                                     "o.b = float4(p.x / 32, 0.5, p.y / 32, 1); return o; }");
    }, 1},
   {"Eight render targets at once", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_BLEND_DESC bd = {};
       bd.IndependentBlendEnable = FALSE;
       bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
       return blendTest(d, o, e, bd, "struct O { float4 c[8] : SV_Target0; }; O main(float4 p : SV_Position) "
                                     "{ O o; [unroll] for (int i = 0; i < 8; i++) o.c[i] = float4(i / 8.0, p.x / 32, p.y / 32, 1); return o; }",
                        DXGI_FORMAT_R8G8B8A8_UNORM, 8);
    }, 1},
   {"Independent blend states per render target", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_BLEND_DESC bd = blendDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE);
       bd.IndependentBlendEnable = TRUE;
       bd.RenderTarget[1].BlendEnable = FALSE;
       bd.RenderTarget[2].BlendOp = D3D11_BLEND_OP_MAX;
       bd.RenderTarget[3].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_ALPHA;
       return blendTest(d, o, e, bd, "struct O { float4 c[4] : SV_Target0; }; O main(float4 p : SV_Position) "
                                     "{ O o; [unroll] for (int i = 0; i < 4; i++) o.c[i] = float4(0.3, p.x / 64, 0.1 * i, 0.5); return o; }",
                        DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    }, 1},
   {"Doubles: arithmetic, comparisons, conversions", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_DOUBLES dbl = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof dbl);
       if (!dbl.DoublePrecisionFloatShaderOps) {
          e = "not supported (optional)";
          return false;
       }
       RT11 rt;
       const float clear[4] = {0, 0, 0, 0};
       // ftod, dmul, dadd, dmax, dmin, negation, dlt/dge/deq/dne, dmovc, dtof; 64-bit immediates
       ID3D11PixelShader *ps = ps11(d, R"(
float4 main(float4 p : SV_Position) : SV_Target
{
   double x = p.x, y = p.y;
   double s = x * 1.5 + y * 0.25;
   double m = max(x, y) - min(x, y);
   bool lt = x < y, ge = x >= y + 3.0, eq = x == y, ne = x != y;
   double c = lt ? s : -m;
   return float4((float)(s * (1.0 / 64.0)), (float)(m * (1.0 / 32.0)),
                 (lt ? 0.25 : 0) + (ge ? 0.5 : 0) + (eq ? 0.125 : 0) + (ne ? 0.0625 : 0),
                 (float)(abs(c) * (1.0 / 64.0)));
})", e);
       bool ok = ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       return ok;
    }, 1, true},
   {"Doubles: 11.1 extended instructions (ddiv, dfma, drcp, conversions)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts);
       if (!opts.ExtendedDoublesShaderInstructions) {
          e = "not supported (optional)";
          return false;
       }
       if (d.warp) {
          // The reference computed here rather than by WARP: Windows 8.1's WARP gets dfma wrong (fma(1.5, 0.5, 1.5)
          // as 1.57 instead of 2.25), which made the GPU's right results a failure there. The shader's math in
          // doubles, then the UNORM8 render target's rounding (the tolerance covers drcp's 1 ULP)
          auto unorm = [](double v) { return (uint32_t)lround(std::min(1.0, std::max(0.0, (double)(float)v)) * 255.0); };
          o.clear();
          for (int py = 0; py < RH; py++) {
             for (int px = 0; px < RW; px++) {
                double x = px + 0.5 + 1, y = py + 0.5 + 1;
                int i = (int)(x * -3.7);
                unsigned u = (unsigned)(y * 2.2);
                double back = (double)i + (double)u;
                o.push_back(unorm(x / y / 33.0) | unorm((x * 0.5 + y) / 50.0) << 8 | unorm(1.0 / y * 2.0) << 16 |
                            unorm((back + 120.0) / 240.0) << 24);
             }
          }
          return true;
       }
       RT11 rt;
       const float clear[4] = {0, 0, 0, 0};
       ID3D11PixelShader *ps = ps11(d, R"(
float4 main(float4 p : SV_Position) : SV_Target
{
   double x = p.x + 1, y = p.y + 1;
   double q = x / y;
   double f = fma(x, 0.5, y);
   double r = rcp(y);
   int i = (int)(x * -3.7);
   uint u = (uint)(y * 2.2);
   double back = (double)i + (double)u;
   return float4((float)(q / 33.0), (float)(f / 50.0), (float)(r * 2.0), (float)((back + 120.0) / 240.0));
})", e);
       bool ok = ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       return ok;
    }, 1, true},
   {"Target-independent rasterization (11.1, ForcedSampleCount 8)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return tirTest(d, o, e, 8);
    }, 33, true},
   {"Target-independent rasterization (11.1, ForcedSampleCount 16, as Direct2D draws)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return tirTest(d, o, e, 16);
    }, 33, true},
   {"UAV-only rendering with a forced sample count (11.1)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       if (d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
          e = "needs feature level 11_1 (optional)";
          return false;
       }
       ID3D11RasterizerState1 *rs = forcedRaster11(d, 4);
       ID3D11VertexShader *vs = vs11(d, TRIANGLE_VS, e);
       ID3D11PixelShader *ps = ps11(d, "RWByteAddressBuffer u : register(u0); "
                                       "void main(float4 p : SV_Position, uint c : SV_Coverage) "
                                       "{ u.InterlockedAdd(0, countbits(c)); u.InterlockedAdd(4, 1); }", e);
       ID3D11Buffer *b = buf11(d, D3D11_BIND_UNORDERED_ACCESS, nullptr, 16, D3D11_USAGE_DEFAULT, 0,
                               D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
       ID3D11UnorderedAccessView *uav = nullptr;
       if (b) {
          D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
          ud.Format = DXGI_FORMAT_R32_TYPELESS;
          ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
          ud.Buffer.NumElements = 4;
          ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
          d.dev->CreateUnorderedAccessView(b, &ud, &uav);
       }
       bool ok = rs && vs && ps && uav;
       if (ok) {
          const UINT zero[4] = {};
          d.ctx->ClearUnorderedAccessViewUint(uav, zero);
          d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &uav, nullptr);
          D3D11_VIEWPORT vp = {0, 0, (float)RW, (float)RH, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          d.ctx->RSSetState(rs);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(3, 0);
          ok = readBuffer11(d, b, 16, o);
          if (ok) e = fmt("%u samples in %u pixels", o[0], o[1]);
       }
       release(uav);
       release(b);
       release(rs);
       release(vs);
       release(ps);
       return ok;
    }, 0, true},
   {"UAVs in vertex shaders (11.1)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       if (d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
          e = "needs feature level 11_1 (optional)";
          return false;
       }
       ID3D11VertexShader *vs = vs11(d, "RWBuffer<uint> u : register(u1); "
                                        "float4 main(uint id : SV_VertexID) : SV_Position "
                                        "{ InterlockedAdd(u[id % 4], id + 1); return float4(0, 0, 0.5, 1); }", e);
       ID3D11PixelShader *ps = ps11(d, "float4 main() : SV_Target { return 1; }", e);
       ID3D11Buffer *b = buf11(d, D3D11_BIND_UNORDERED_ACCESS, nullptr, 16);
       ID3D11UnorderedAccessView *uav = nullptr;
       if (b) {
          D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
          ud.Format = DXGI_FORMAT_R32_UINT;
          ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
          ud.Buffer.NumElements = 4;
          d.dev->CreateUnorderedAccessView(b, &ud, &uav);
       }
       RT11 rt;
       const float clear[4] = {0, 0, 0, 0};
       bool ok = vs && ps && uav && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          const UINT zero[4] = {};
          d.ctx->ClearUnorderedAccessViewUint(uav, zero);
          d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rt.rtv, nullptr, 1, 1, &uav, nullptr);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(20, 0);
          ok = readBuffer11(d, b, 16, o);
       }
       rt.release();
       release(uav);
       release(b);
       release(vs);
       release(ps);
       return ok;
    }, 0, true},
   {"ClearView on UAVs (11.1)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11DeviceContext1 *ctx1 = nullptr;
       d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
       if (!ctx1) {
          e = "no ID3D11DeviceContext1 (optional)";
          return false;
       }
       // (Windows 7's 11.1 runtime over an 11.0 driver: no ClearView, the call does nothing)
       D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
       if (FAILED(d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts)) || !opts.ClearView) {
          ctx1->Release();
          e = "the driver has no ClearView (optional)";
          return false;
       }
       std::vector<uint8_t> init(RW * RH * 4);
       fillPattern(init, 7);
       ID3D11Texture2D *t = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, init.data(), RW * 4,
                                  D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
       ID3D11UnorderedAccessView *uav = nullptr;
       if (t) d.dev->CreateUnorderedAccessView(t, nullptr, &uav);
       bool ok = uav != nullptr;
       if (ok) {
          const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
          const D3D11_RECT rects[2] = {{2, 3, 12, 9}, {20, 16, 31, 30}};
          ctx1->ClearView(uav, color, rects, 2);
          ok = readTexture(d, t, 0, o);
       }
       release(uav);
       release(t);
       ctx1->Release();
       return ok;
    }, 1, true},
   {"Clip and cull distances (four clip planes in one register, a cull distance)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11VertexShader *vs = vs11(d, R"(
struct O { float4 p : SV_Position; float4 clip : SV_ClipDistance0; float cull : SV_CullDistance0; float4 c : COLOR; };
O main(uint id : SV_VertexID)
{
   O o; uint tri = id / 3, v = id % 3;
   float2 uv = float2((v << 1) & 2, v & 2);
   o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
   if (tri == 1) o.p.xy = o.p.xy * 0.5 + 0.2;
   // four planes: a window from x -0.6..0.7 and y -0.5..0.8
   o.clip = float4(o.p.x + 0.6, 0.7 - o.p.x, o.p.y + 0.5, 0.8 - o.p.y);
   o.cull = tri == 1 ? -1 : 1;   // the second triangle is culled
   o.c = float4(uv * 0.5, 0.8, 1);
   return o;
})", e);
       // the pixel shader's inputs match the vertex shader's outputs register by register (and read a distance)
       ID3D11PixelShader *ps = ps11(d, "struct O { float4 p : SV_Position; float4 clip : SV_ClipDistance0; float cull : SV_CullDistance0; float4 c : COLOR; }; "
                                       "float4 main(O i) : SV_Target { return float4(i.c.rgb, saturate(i.clip.x)); }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = vs && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(6, 0);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(vs);
       release(ps);
       return ok;
    }, 1},
   {"Coverage masks XORed into R16_UINT (ForcedSampleCount 16, as Direct2D fills paths)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts);
       ID3D11Device1 *dev1 = nullptr;
       d.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
       if (!opts.OutputMergerLogicOp || !dev1 || d.dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1) {
          e = "not supported (optional)";
          release(dev1);
          return false;
       }
       D3D11_BLEND_DESC1 bd = {};
       bd.RenderTarget[0].LogicOpEnable = TRUE;
       bd.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_XOR;
       bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
       ID3D11BlendState1 *bs = nullptr;
       dev1->CreateBlendState1(&bd, &bs);
       dev1->Release();
       ID3D11RasterizerState1 *rs = forcedRaster11(d, 16);
       // two overlapping triangles: where both cover a sample it ends up clear
       ID3D11VertexShader *vs = vs11(d, "float4 main(uint id : SV_VertexID) : SV_Position { "
                                        "float2 v[6] = {float2(-0.9, -0.8), float2(0.7, 0.95), float2(0.95, -0.6), "
                                        "float2(-0.95, 0.7), float2(0.85, 0.9), float2(0.2, -0.97)}; "
                                        "return float4(v[id], 0.5, 1); }", e);
       ID3D11PixelShader *ps = ps11(d, "uint main(float4 p : SV_Position, uint c : SV_Coverage) : SV_Target { return c; }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 0};
       bool ok = bs && rs && vs && ps && makeRT(d, rt, DXGI_FORMAT_R16_UINT, 2, clear);
       if (ok) {
          d.ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
          d.ctx->RSSetState(rs);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(6, 0);
          d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
          d.ctx->RSSetState(nullptr);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       release(vs);
       release(rs);
       release(bs);
       return ok;
    }, 0, true},
   {"Logic operations (11.1)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts);
       ID3D11Device1 *dev1 = nullptr;
       d.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
       if (!opts.OutputMergerLogicOp || !dev1) {
          e = "not supported (optional)";
          release(dev1);
          return false;
       }
       D3D11_BLEND_DESC1 bd = {};
       bd.RenderTarget[0].LogicOpEnable = TRUE;
       bd.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_XOR;
       bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
       ID3D11BlendState1 *bs = nullptr;
       dev1->CreateBlendState1(&bd, &bs);
       RT11 rt;
       const float clear[4] = {0x55, 0xaa, 0x0f, 0xf0};
       ID3D11PixelShader *ps = ps11(d, "uint4 main(float4 p : SV_Position) : SV_Target { return uint4(p.x, p.y, 0xff, 0x33); }", e);
       bool ok = bs && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UINT, 4, clear);
       if (ok) {
          d.ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       release(bs);
       dev1->Release();
       return ok;
    }, 0, true},
   {"Viewport arrays (SV_ViewportArrayIndex)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11GeometryShader *gs = gs11(d, R"(
struct G { float4 p : SV_Position; float4 c : COLOR; uint vp : SV_ViewportArrayIndex; };
[maxvertexcount(6)] void main(triangle float4 v[3] : SV_Position, inout TriangleStream<G> s)
{
   for (uint k = 0; k < 2; k++) {
      for (uint i = 0; i < 3; i++) { G g; g.p = v[i]; g.c = float4(k, 1 - k, 0.5, 1); g.vp = k; s.Append(g); }
      s.RestartStrip();
   }
})", e);
       ID3D11VertexShader *vs = vs11(d, TRI_VS, e);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c * float4(p.xy / 32, 1, 1); }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = gs && vs && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          D3D11_VIEWPORT vps[2] = {{0, 0, 16, 16, 0, 1}, {16, 8, 16, 24, 0, 1}};
          d.ctx->RSSetViewports(2, vps);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->GSSetShader(gs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(3, 0);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(gs);
       release(vs);
       release(ps);
       return ok;
    }, 1},
   {"Render target arrays (SV_RenderTargetArrayIndex)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11Texture2D *arr = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, D3D11_BIND_RENDER_TARGET, 3);
       ID3D11RenderTargetView *rtv = nullptr;
       if (arr) d.dev->CreateRenderTargetView(arr, nullptr, &rtv);
       ID3D11GeometryShader *gs = gs11(d, R"(
struct G { float4 p : SV_Position; uint rt : SV_RenderTargetArrayIndex; };
[maxvertexcount(3)] void main(triangle float4 v[3] : SV_Position, inout TriangleStream<G> s)
{ for (uint i = 0; i < 3; i++) { G g; g.p = v[i]; g.rt = 2; s.Append(g); } })", e);
       ID3D11VertexShader *vs = vs11(d, TRI_VS, e);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.xy / 32, 0.75, 1); }", e);
       bool ok = rtv && gs && vs && ps;
       if (ok) {
          const float clear[4] = {0.1f, 0.2f, 0.3f, 1};
          d.ctx->ClearRenderTargetView(rtv, clear);
          d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
          D3D11_VIEWPORT vp = {0, 0, RW, RH, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->GSSetShader(gs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(3, 0);
          std::vector<uint32_t> s0, s2;
          ok = readTexture(d, arr, 0, s0) && readTexture(d, arr, 2, s2);
          o = s0;
          o.insert(o.end(), s2.begin(), s2.end());
       }
       release(rtv);
       release(arr);
       release(gs);
       release(vs);
       release(ps);
       return ok;
    }, 1},
   {"Instancing with per-instance data and step rates", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3DBlob *code = nullptr;
       ID3D11VertexShader *vs = vs11(d, R"(
struct O { float4 p : SV_Position; float4 c : COLOR; };
O main(float2 corner : CORNER, float4 color : COLOR, uint inst : SV_InstanceID)
{ O o; o.p = float4(corner.x * 0.5 - 1 + inst * 0.5, corner.y, 0.5, 1); o.c = color; return o; })", e, &code);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }", e);
       D3D11_INPUT_ELEMENT_DESC el[] = {{"CORNER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 2}};
       ID3D11InputLayout *layout = nullptr;
       if (code) d.dev->CreateInputLayout(el, 2, code->GetBufferPointer(), code->GetBufferSize(), &layout);
       const float corners[8] = {0, 1, 1, 1, 0, -1, 1, -1};
       const uint32_t colors[3] = {0xff0000ff, 0xff00ff00, 0xffff0000};
       ID3D11Buffer *vb0 = buf11(d, D3D11_BIND_VERTEX_BUFFER, corners, sizeof corners);
       ID3D11Buffer *vb1 = buf11(d, D3D11_BIND_VERTEX_BUFFER, colors, sizeof colors);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = vs && ps && layout && vb0 && vb1 && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          ID3D11Buffer *vbs[2] = {vb0, vb1};
          UINT strides[2] = {8, 4}, offsets[2] = {0, 0};
          d.ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
          d.ctx->IASetInputLayout(layout);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->DrawInstanced(4, 4, 0, 1);   // instances 0..3 read colors 1, 1, 2, 2 (step rate 2, from instance 1)
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(code);
       release(vs);
       release(ps);
       release(layout);
       release(vb0);
       release(vb1);
       return ok;
    }, 0},
   {"Stream output and DrawAuto", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3DBlob *code = nullptr;
       ID3D11VertexShader *vs = vs11(d, R"(
struct O { float4 p : SV_Position; float4 c : COLOR; };
O main(uint id : SV_VertexID) { O o; float2 uv = float2((id << 1) & 2, id & 2);
   o.p = float4(uv * float2(1, -1) + float2(-0.5, 0.5), 0.5, 1); o.c = float4(uv, 0.5, 1); return o; })", e, &code);
       D3D11_SO_DECLARATION_ENTRY so[] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0}};
       UINT stride = 32;
       ID3D11GeometryShader *gs = nullptr;
       if (code) d.dev->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), so, 2, &stride, 1,
                                                             D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs);
       ID3D11Buffer *sob = buf11(d, D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER, nullptr, 32 * 16);
       ID3DBlob *code2 = nullptr;
       ID3D11VertexShader *vs2 = vs11(d, "struct O { float4 p : SV_Position; float4 c : COLOR; };"
                                         "O main(float4 p : POSITION, float4 c : COLOR) { O o; o.p = p; o.c = c; return o; }", e, &code2);
       D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
       ID3D11InputLayout *layout = nullptr;
       if (code2) d.dev->CreateInputLayout(el, 2, code2->GetBufferPointer(), code2->GetBufferSize(), &layout);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = vs && gs && sob && vs2 && layout && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          UINT offset = 0;
          d.ctx->SOSetTargets(1, &sob, &offset);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->GSSetShader(gs, nullptr, 0);
          d.ctx->PSSetShader(nullptr, nullptr, 0);
          d.ctx->Draw(3, 0);
          ID3D11Buffer *none = nullptr;
          d.ctx->SOSetTargets(1, &none, &offset);
          d.ctx->IASetVertexBuffers(0, 1, &sob, &stride, &offset);
          d.ctx->IASetInputLayout(layout);
          d.ctx->VSSetShader(vs2, nullptr, 0);
          d.ctx->GSSetShader(nullptr, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->DrawAuto();
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(code);
       release(code2);
       release(vs);
       release(gs);
       release(sob);
       release(vs2);
       release(layout);
       release(ps);
       return ok;
    }, 1},
   {"Mipmap generation (GenerateMips)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       std::vector<uint8_t> data(RW * RH * 4);
       fillPattern(data, 77);
       D3D11_TEXTURE2D_DESC td = {RW, RH, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS};
       ID3D11Texture2D *t = nullptr;
       d.dev->CreateTexture2D(&td, nullptr, &t);
       ID3D11ShaderResourceView *srv = nullptr;
       if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
       if (!srv) {
          release(t);
          return false;
       }
       d.ctx->UpdateSubresource(t, 0, nullptr, data.data(), RW * 4, 0);
       d.ctx->GenerateMips(srv);
       std::vector<uint32_t> l2, l4;
       bool ok = readTexture(d, t, 2, l2) && readTexture(d, t, 4, l4);
       o = l2;
       o.insert(o.end(), l4.begin(), l4.end());
       srv->Release();
       t->Release();
       return ok;
    }, 3},
   {"Copies and updates (CopyResource, CopySubresourceRegion, UpdateSubresource)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       std::vector<uint8_t> a(RW * RH * 4), b(RW * RH * 4), c(8 * 8 * 4);
       fillPattern(a, 101);
       fillPattern(b, 202);
       fillPattern(c, 303);
       ID3D11Texture2D *ta = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, a.data(), RW * 4);
       ID3D11Texture2D *tb = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, b.data(), RW * 4);
       ID3D11Texture2D *tc = tex11(d, RW, RH, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0);
       if (!ta || !tb || !tc) return false;
       D3D11_BOX box = {4, 6, 0, 20, 30, 1};
       d.ctx->CopySubresourceRegion(tb, 0, 10, 1, 0, ta, 0, &box);
       D3D11_BOX upd = {20, 20, 0, 28, 28, 1};
       d.ctx->UpdateSubresource(tb, 0, &upd, c.data(), 8 * 4, 0);
       d.ctx->CopyResource(tc, tb);
       bool ok = readTexture(d, tc, 0, o);
       ta->Release();
       tb->Release();
       tc->Release();
       return ok;
    }, 0},
   {"Dynamic buffers (Map WRITE_DISCARD and NO_OVERWRITE)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3DBlob *code = nullptr;
       ID3D11VertexShader *vs = vs11(d, "struct O { float4 p : SV_Position; float4 c : COLOR; };"
                                        "O main(float2 p : POSITION, float4 c : COLOR) { O o; o.p = float4(p, 0.5, 1); o.c = c; return o; }", e, &code);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }", e);
       D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
       ID3D11InputLayout *layout = nullptr;
       if (code) d.dev->CreateInputLayout(el, 2, code->GetBufferPointer(), code->GetBufferSize(), &layout);
       ID3D11Buffer *vb = buf11(d, D3D11_BIND_VERTEX_BUFFER, nullptr, 12 * 64, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = vs && ps && layout && vb && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          UINT stride = 12, offset = 0;
          d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
          d.ctx->IASetInputLayout(layout);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          for (int q = 0; q < 4; q++) {
             struct V { float x, y; uint32_t c; } v[4];
             float x0 = -1 + q * 0.5f, x1 = x0 + 0.5f;
             uint32_t c = 0xff000000 | (q * 60) << 16 | (255 - q * 60) << 8 | 0x40;
             v[0] = {x0, 1, c};
             v[1] = {x1, 1, c};
             v[2] = {x0, -1, c};
             v[3] = {x1, -1, c};
             D3D11_MAPPED_SUBRESOURCE m;
             if (FAILED(d.ctx->Map(vb, 0, q ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
             memcpy((char *)m.pData + q * sizeof v, v, sizeof v);
             d.ctx->Unmap(vb, 0);
             d.ctx->Draw(4, q * 4);
          }
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(code);
       release(vs);
       release(ps);
       release(layout);
       release(vb);
       return ok;
    }, 0},
   {"Predicated rendering (SetPredication)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
       ID3D11Predicate *hidden = nullptr, *visible = nullptr;
       d.dev->CreatePredicate(&qd, &hidden);
       d.dev->CreatePredicate(&qd, &visible);
       ID3D11PixelShader *none = ps11(d, "float4 main() : SV_Target { discard; return 0; }", e);
       ID3D11PixelShader *red = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { return p.x < 16 ? float4(1, 0, 0, 1) : float4(0, 0, 0, 0); }", e);
       ID3D11PixelShader *green = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }", e);
       ID3D11PixelShader *blue = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { return float4(0, 0, 1, 1); }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = hidden && visible && none && red && green && blue && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          d.ctx->Begin(hidden);
          fullscreen11(d, none);
          d.ctx->End(hidden);
          d.ctx->SetPredication(hidden, FALSE);
          D3D11_VIEWPORT left = {0, 0, 16, RH, 0, 1};
          d.ctx->RSSetViewports(1, &left);
          fullscreen11(d, green);   // skipped: nothing passed
          d.ctx->SetPredication(nullptr, FALSE);
          D3D11_VIEWPORT all = {0, 0, RW, RH, 0, 1};
          d.ctx->RSSetViewports(1, &all);
          d.ctx->Begin(visible);
          fullscreen11(d, red);
          d.ctx->End(visible);
          d.ctx->SetPredication(visible, FALSE);
          D3D11_VIEWPORT right = {16, 0, 16, RH, 0, 1};
          d.ctx->RSSetViewports(1, &right);
          fullscreen11(d, blue);    // drawn
          d.ctx->SetPredication(nullptr, FALSE);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(hidden);
       release(visible);
       release(none);
       release(red);
       release(green);
       release(blue);
       return ok;
    }, 0},
   {"Occlusion and pipeline statistics queries", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_QUERY_DESC oq = {D3D11_QUERY_OCCLUSION, 0}, sq = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
       ID3D11Query *occ = nullptr, *stats = nullptr;
       d.dev->CreateQuery(&oq, &occ);
       d.dev->CreateQuery(&sq, &stats);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { if (p.y >= 24) discard; return 1; }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = occ && stats && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (ok) {
          d.ctx->Begin(occ);
          d.ctx->Begin(stats);
          fullscreen11(d, ps);
          d.ctx->End(stats);
          d.ctx->End(occ);
          UINT64 n = 0;
          D3D11_QUERY_DATA_PIPELINE_STATISTICS st = {};
          for (int i = 0; i < 5000 && d.ctx->GetData(occ, &n, sizeof n, 0) == S_FALSE; i++) Sleep(1);
          for (int i = 0; i < 5000 && d.ctx->GetData(stats, &st, sizeof st, 0) == S_FALSE; i++) Sleep(1);
          // vertex shader invocations vary with caching, pixel shader ones with helper pixels, and the primitives
          // clipping outputs with how it splits them (WARP: 3 for the clipped triangle, GPUs: 1): not compared
          o = {(uint32_t)n, (uint32_t)st.IAVertices, (uint32_t)st.IAPrimitives, (uint32_t)st.CInvocations};
          e = fmt("%llu pixels, %llu vertices, %llu primitives, %llu clipped", (unsigned long long)n,
                  (unsigned long long)st.IAVertices, (unsigned long long)st.IAPrimitives, (unsigned long long)st.CInvocations);
       }
       rt.release();
       release(occ);
       release(stats);
       release(ps);
       return ok;
    }, 0},
   {"Deferred context: record and execute a command list", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11DeviceContext *def = nullptr;
       HRESULT h = d.dev->CreateDeferredContext(0, &def);
       ID3D11PixelShader *ps = ps11(d, "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.x / 32, 0.55, p.y / 32, 1); }", e);
       RT11 rt;
       const float clear[4] = {0, 0, 0, 1};
       bool ok = SUCCEEDED(h) && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, clear);
       if (!ok && FAILED(h)) e = "CreateDeferredContext: " + hrs(h);
       if (ok) {
          const float c2[4] = {0.6f, 0, 0, 1};
          def->ClearRenderTargetView(rt.rtv, c2);
          def->OMSetRenderTargets(1, &rt.rtv, nullptr);
          D3D11_VIEWPORT vp = {8, 8, 16, 16, 0, 1};
          def->RSSetViewports(1, &vp);
          def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          def->VSSetShader(d.vs, nullptr, 0);
          def->PSSetShader(ps, nullptr, 0);
          def->Draw(3, 0);
          ID3D11CommandList *list = nullptr;
          ok = SUCCEEDED(def->FinishCommandList(FALSE, &list));
          if (ok) {
             d.ctx->ExecuteCommandList(list, FALSE);
             list->Release();
             ok = readTexture(d, rt.tex, 0, o);
          }
       }
       rt.release();
       release(ps);
       release(def);
       return ok;
    }, 1},
   {"Stencil reference from the pixel shader (SV_StencilRef, 11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
       if (!o2.PSSpecifiedStencilRefSupported) {
          e = "not supported (optional)";
          return false;
       }
       ID3D11Texture2D *t = tex11(d, RW, RH, DXGI_FORMAT_D24_UNORM_S8_UINT, nullptr, 0, D3D11_BIND_DEPTH_STENCIL);
       ID3D11DepthStencilView *dsv = nullptr;
       if (t) d.dev->CreateDepthStencilView(t, nullptr, &dsv);
       D3D11_DEPTH_STENCIL_DESC dd = {};
       dd.StencilEnable = TRUE;
       dd.StencilReadMask = dd.StencilWriteMask = 0xff;
       dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
       dd.BackFace = dd.FrontFace;
       ID3D11DepthStencilState *dss = nullptr;
       d.dev->CreateDepthStencilState(&dd, &dss);
       ID3D11PixelShader *ps = ps11(d, "uint main(float4 p : SV_Position) : SV_StencilRef "
                                       "{ return ((uint)p.x * 7 + (uint)p.y * 3) & 255; }", e);
       bool ok = dsv && dss && ps;
       if (ok) {
          d.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0x5a);
          d.ctx->OMSetRenderTargets(0, nullptr, dsv);
          d.ctx->OMSetDepthStencilState(dss, 0x11);
          D3D11_VIEWPORT vp = {4, 2, RW - 8, RH - 4, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          fullscreen11(d, ps);
          d.ctx->OMSetDepthStencilState(nullptr, 0);
          ok = readTexture(d, t, 0, o);
       }
       release(ps);
       release(dss);
       release(dsv);
       release(t);
       return ok;
    }, 0, true},
   {"Typed UAV loads of R8G8B8A8_UNORM and R16G16B16A16_FLOAT (11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
       if (!o2.TypedUAVLoadAdditionalFormats) {
          e = "not supported (optional)";
          return false;
       }
       ID3DBlob *b = compileHlsl("RWTexture2D<float4> u : register(u0); [numthreads(8, 8, 1)] void main(uint3 i : SV_DispatchThreadID) "
                                 "{ float4 v = u[i.xy]; u[i.xy] = float4(v.y, v.z * 0.5, v.x * 2, v.w * 0.25); }", "cs_5_0", &e);
       ID3D11ComputeShader *cs = nullptr;
       if (b) d.dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cs);
       release(b);
       bool ok = cs != nullptr;
       for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
          if (!ok) break;
          D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs = {f};
          d.dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof fs);
          if (!(fs.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
             e = std::string(f == DXGI_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8_UNORM" : "R16G16B16A16_FLOAT") + " has no typed UAV loads";
             ok = false;
             break;
          }
          UINT bpp = f == DXGI_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
          std::vector<uint8_t> init(RW * RH * bpp);
          fillPattern(init, 21);
          if (bpp == 8) {
             // halves in [0.125, 0.25): every operation above is exact
             for (size_t i = 0; i < init.size(); i += 2) { init[i + 1] = 0x30 | (init[i + 1] & 3); }
          }
          ID3D11Texture2D *t = tex11(d, RW, RH, f, init.data(), RW * bpp, D3D11_BIND_UNORDERED_ACCESS);
          ID3D11UnorderedAccessView *uav = nullptr;
          if (t) d.dev->CreateUnorderedAccessView(t, nullptr, &uav);
          std::vector<uint32_t> one;
          ok = uav != nullptr;
          if (ok) {
             d.ctx->CSSetShader(cs, nullptr, 0);
             d.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
             d.ctx->Dispatch(RW / 8, RH / 8, 1);
             ID3D11UnorderedAccessView *none = nullptr;
             d.ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
             d.ctx->CSSetShader(nullptr, nullptr, 0);
             ok = readTexture(d, t, 0, one);
             o.insert(o.end(), one.begin(), one.end());
          }
          release(uav);
          release(t);
       }
       release(cs);
       return ok;
    }, 1, true},
   {"Conservative rasterization (11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
       ID3D11Device3 *dev3 = nullptr;
       d.dev->QueryInterface(__uuidof(ID3D11Device3), (void **)&dev3);
       if (o2.ConservativeRasterizationTier == D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED || !dev3) {
          e = "not supported (optional)";
          release(dev3);
          return false;
       }
       // a small triangle in every fourth pixel, away from its center: drawn with conservative rasterization only.
       // Per triangle whether its pixel was drawn, and whether anything was more than a pixel away from them (tier
       // 1 may draw the neighbors, within its half a pixel of uncertainty)
       ID3D11VertexShader *vs = vs11(d, "float4 main(uint v : SV_VertexID) : SV_Position { uint c = v / 3, k = v % 3; "
                                        "float2 p = float2(c % 8, c / 8) * 4 + 1 + float2(k == 1 ? 0.4 : 0.1, k == 2 ? 0.4 : 0.1); "
                                        "return float4(p.x / 16 - 1, 1 - p.y / 16, 0, 1); }", e);
       ID3D11PixelShader *ps = ps11(d, "float4 main() : SV_Target { return 1; }", e);
       RT11 rt;
       const float black[4] = {};
       bool ok = vs && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black);
       for (int on = 0; ok && on < 2; on++) {
          D3D11_RASTERIZER_DESC2 rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE, 0,
                                       on ? D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF};
          ID3D11RasterizerState2 *rs = nullptr;
          HRESULT h = dev3->CreateRasterizerState2(&rd, &rs);
          if (FAILED(h)) {
             e = "CreateRasterizerState2: " + hrs(h);
             ok = false;
             break;
          }
          d.ctx->ClearRenderTargetView(rt.rtv, black);
          d.ctx->RSSetState(rs);
          d.ctx->IASetInputLayout(nullptr);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->Draw(64 * 3, 0);
          d.ctx->RSSetState(nullptr);
          rs->Release();
          std::vector<uint32_t> px;
          ok = readTexture(d, rt.tex, 0, px);
          uint32_t away = 0;
          for (int y = 0; ok && y < RH; y++)
             for (int x = 0; x < RW; x++)
                if (px[y * RW + x] && (x % 4 == 3 || y % 4 == 3)) away = 1;
          for (int c = 0; ok && c < 64; c++) o.push_back(px[(c / 8 * 4 + 1) * RW + c % 8 * 4 + 1] ? 1 : 0);
          o.push_back(away);
       }
       rt.release();
       release(ps);
       release(vs);
       release(dev3);
       return ok;
    }, 0, true},
   {"Rasterizer ordered views (11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
       if (!o2.ROVsSupported) {
          e = "not supported (optional)";
          return false;
       }
       // 64 full-screen triangles in one draw, each pixel shader invocation a read-modify-write that depends on the
       // order (v * 31 + instance): right only when they happen in draw order, pixel by pixel
       ID3D11VertexShader *vs = vs11(d, "void main(uint v : SV_VertexID, uint i : SV_InstanceID, out float4 p : SV_Position, "
                                        "out nointerpolation uint o : I) { float2 t = float2((v << 1) & 2, v & 2); "
                                        "p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o = i; }", e);
       ID3D11PixelShader *ps = ps11(d, "RasterizerOrderedTexture2D<uint> r : register(u0); "
                                       "void main(float4 p : SV_Position, nointerpolation uint i : I) "
                                       "{ uint2 c = (uint2)p.xy; uint v = r[c]; r[c] = v * 31 + i + 1 + c.x; }", e);
       std::vector<uint32_t> zero(RW * RH);
       ID3D11Texture2D *t = tex11(d, RW, RH, DXGI_FORMAT_R32_UINT, zero.data(), RW * 4, D3D11_BIND_UNORDERED_ACCESS);
       ID3D11UnorderedAccessView *uav = nullptr;
       if (t) d.dev->CreateUnorderedAccessView(t, nullptr, &uav);
       bool ok = vs && ps && uav;
       if (ok) {
          d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &uav, nullptr);
          D3D11_VIEWPORT vp = {0, 0, RW, RH, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          d.ctx->IASetInputLayout(nullptr);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(ps, nullptr, 0);
          d.ctx->DrawInstanced(3, 64, 0, 0);
          d.ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
          ok = readTexture(d, t, 0, o);
       }
       release(uav);
       release(t);
       release(ps);
       release(vs);
       return ok;
    }, 0, true},
   {"Tiled resources: tile mappings, UpdateTiles, aliasing and rendering (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11DeviceContext2 *ctx2 = nullptr;
       if (!tiled11(d, &ctx2, e)) return false;
       // a 2x2-tile texture on two pool tiles: tile (0,0) on the first, (1,0) and (0,1) both on the second (aliased),
       // (1,1) unmapped. Tile (0,0) and (1,0) get data, (0,1) a solid color drawn: (1,0) shows it too
       ID3D11Buffer *pool = tilePool11(d, 2);
       ID3D11Texture2D *t = tiledTexture11(d, 256, 256, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
       ID3D11RenderTargetView *rtv = nullptr;
       if (t) d.dev->CreateRenderTargetView(t, nullptr, &rtv);
       ID3D11PixelShader *ps = ps11(d, "float4 main() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }", e);
       std::vector<uint8_t> zero(256 * 256 * 4);
       ID3D11Texture2D *out = tex11(d, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, zero.data(), 256 * 4);
       bool ok = pool && t && rtv && ps && out;
       HRESULT h = S_OK;
       if (ok) {
          D3D11_TILED_RESOURCE_COORDINATE at = {};
          D3D11_TILE_REGION_SIZE all = {4, TRUE, 2, 2, 1};
          UINT flags[] = {0, 0, 0, D3D11_TILE_RANGE_NULL}, offsets[] = {0, 1, 1, 0}, counts[] = {1, 1, 1, 1};
          h = ctx2->UpdateTileMappings(t, 1, &at, &all, pool, 4, flags, offsets, counts, 0);
          D3D11_TILE_REGION_SIZE one = {1, FALSE};
          std::vector<uint8_t> a = tilePattern(3), b = tilePattern(7);
          ctx2->UpdateTiles(t, &at, &one, a.data(), 0);
          at.X = 1;
          ctx2->UpdateTiles(t, &at, &one, b.data(), 0);
          d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
          D3D11_VIEWPORT vp = {0, 128, 128, 128, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          fullscreen11(d, ps);
          d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
          for (UINT tile = 0; tile < 3; tile++) {
             UINT x = tile == 1 ? 128 : 0, y = tile == 2 ? 128 : 0;
             D3D11_BOX box = {x, y, 0, x + 128, y + 128, 1};
             d.ctx->CopySubresourceRegion(out, 0, x, y, 0, t, 0, &box);
          }
          ok = SUCCEEDED(h) && readTexture(d, out, 0, o);
          if (FAILED(h)) e = "UpdateTileMappings: " + hrs(h);
       }
       release(out);
       release(ps);
       release(rtv);
       release(t);
       release(pool);
       release(ctx2);
       return ok;
    }, 1, true},
   {"Tiled resources: CopyTileMappings, CopyTiles and tiled buffers (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11DeviceContext2 *ctx2 = nullptr;
       if (!tiled11(d, &ctx2, e)) return false;
       // texture A's two tiles get data; B takes A's mappings swapped (CopyTileMappings) and CopyTiles writes B's
       // two tiles to a buffer. A tiled buffer of three tiles: the first two on one pool tile, data in the first
       ID3D11Buffer *pool = tilePool11(d, 3);
       ID3D11Texture2D *ta = tiledTexture11(d, 256, 128, 1, D3D11_BIND_SHADER_RESOURCE);
       ID3D11Texture2D *tb = tiledTexture11(d, 256, 128, 1, D3D11_BIND_SHADER_RESOURCE);
       D3D11_BUFFER_DESC bd = {2 * 65536, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
       ID3D11Buffer *linear = nullptr, *tiledBuf = nullptr, *plain = nullptr, *staging = nullptr;
       d.dev->CreateBuffer(&bd, nullptr, &linear);
       D3D11_BUFFER_DESC tbd = {3 * 65536, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_TILED, 0};
       d.dev->CreateBuffer(&tbd, nullptr, &tiledBuf);
       D3D11_BUFFER_DESC pbd = {4 * 65536, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
       d.dev->CreateBuffer(&pbd, nullptr, &plain);
       D3D11_BUFFER_DESC sbd = {4 * 65536, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
       d.dev->CreateBuffer(&sbd, nullptr, &staging);
       bool ok = pool && ta && tb && linear && tiledBuf && plain && staging;
       if (ok) {
          D3D11_TILED_RESOURCE_COORDINATE at = {};
          D3D11_TILE_REGION_SIZE two = {2, FALSE}, one = {1, FALSE};
          UINT start = 0;
          ctx2->UpdateTileMappings(ta, 1, &at, &two, pool, 1, nullptr, &start, nullptr, 0);
          std::vector<uint8_t> a = tilePattern(5), b = tilePattern(9), c = tilePattern(11);
          ctx2->UpdateTiles(ta, &at, &one, a.data(), 0);
          D3D11_TILED_RESOURCE_COORDINATE second = {1, 0, 0, 0};
          ctx2->UpdateTiles(ta, &second, &one, b.data(), 0);
          ctx2->CopyTileMappings(tb, &at, ta, &second, &one, 0);
          ctx2->CopyTileMappings(tb, &second, ta, &at, &one, 0);
          ctx2->CopyTiles(tb, &at, &two, linear, 0, D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
          UINT flags[] = {D3D11_TILE_RANGE_REUSE_SINGLE_TILE, D3D11_TILE_RANGE_NULL}, offsets[] = {2, 0}, counts[] = {2, 1};
          D3D11_TILE_REGION_SIZE three = {3, FALSE};
          ctx2->UpdateTileMappings(tiledBuf, 1, &at, &three, pool, 2, flags, offsets, counts, 0);
          ctx2->UpdateTiles(tiledBuf, &at, &one, c.data(), 0);
          D3D11_BOX lb = {0, 0, 0, 2 * 65536, 1, 1}, tb2 = {0, 0, 0, 2 * 65536, 1, 1};
          d.ctx->CopySubresourceRegion(plain, 0, 0, 0, 0, linear, 0, &lb);
          d.ctx->CopySubresourceRegion(plain, 0, 2 * 65536, 0, 0, tiledBuf, 0, &tb2);
          d.ctx->CopyResource(staging, plain);
          D3D11_MAPPED_SUBRESOURCE m;
          ok = SUCCEEDED(d.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
          if (ok) {
             o.assign((const uint32_t *)m.pData, (const uint32_t *)m.pData + 4 * 65536 / 4);
             d.ctx->Unmap(staging, 0);
          }
       }
       release(staging);
       release(plain);
       release(tiledBuf);
       release(linear);
       release(tb);
       release(ta);
       release(pool);
       release(ctx2);
       return ok;
    }, 0, true},
   {"Tiled resources: packed mips (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       ID3D11DeviceContext2 *ctx2 = nullptr;
       if (!tiled11(d, &ctx2, e)) return false;
       ID3D11Device2 *dev2 = nullptr;
       d.dev->QueryInterface(__uuidof(ID3D11Device2), (void **)&dev2);
       // a 256x256 texture with all 9 mips, every tile mapped (standard and packed ones), each mip cleared to a color
       // of its own and read back
       ID3D11Texture2D *t = tiledTexture11(d, 256, 256, 9, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
       UINT total = 0;
       D3D11_PACKED_MIP_DESC packed = {};
       D3D11_TILE_SHAPE shape = {};
       UINT subs = 9;
       D3D11_SUBRESOURCE_TILING tilings[9] = {};
       if (t && dev2) dev2->GetResourceTiling(t, &total, &packed, &shape, &subs, 0, tilings);
       ID3D11Buffer *pool = total ? tilePool11(d, total) : nullptr;
       D3D11_TEXTURE2D_DESC od = {256, 256, 9, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
       ID3D11Texture2D *out = nullptr;
       d.dev->CreateTexture2D(&od, nullptr, &out);
       bool ok = t && dev2 && pool && out;
       if (ok) {
          D3D11_TILED_RESOURCE_COORDINATE at = {};
          D3D11_TILE_REGION_SIZE all = {total, FALSE};
          UINT start = 0;
          ctx2->UpdateTileMappings(t, 1, &at, &all, pool, 1, nullptr, &start, nullptr, 0);
          for (UINT m = 0; m < 9 && ok; m++) {
             D3D11_RENDER_TARGET_VIEW_DESC rd = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D};
             rd.Texture2D.MipSlice = m;
             ID3D11RenderTargetView *rtv = nullptr;
             ok = SUCCEEDED(d.dev->CreateRenderTargetView(t, &rd, &rtv));
             float color[4] = {m / 8.0f, 1 - m / 8.0f, 0.5f, 1};
             if (ok) d.ctx->ClearRenderTargetView(rtv, color);
             release(rtv);
          }
          d.ctx->CopyResource(out, t);
          for (UINT m = 0; m < 9 && ok; m++) {
             std::vector<uint32_t> level;
             ok = readTexture(d, out, m, level);
             o.push_back(level.empty() ? 0 : level[0]);
             o.push_back(level.empty() ? 0 : level.back());
          }
          e = fmt("%u tiles, %u packed mips in %u tiles", total, packed.NumPackedMips, packed.NumTilesForPackedMips);
       }
       release(out);
       release(pool);
       release(t);
       release(dev2);
       release(ctx2);
       return ok;
    }, 1, true},
   {"Min/max filtering (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
       if (!o1.MinMaxFiltering) {
          e = "not supported (optional)";
          return false;
       }
       // a 4x4 texture filtered bilinearly at texel corners (four equal weights: no sub-texel precision in it), with
       // the minimum (left half) and maximum (right half) reduction
       std::vector<uint8_t> init(4 * 4 * 4);
       fillPattern(init, 41);
       ID3D11Texture2D *t = tex11(d, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, init.data(), 16);
       ID3D11ShaderResourceView *srv = nullptr;
       if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
       ID3D11SamplerState *smp[2] = {};
       for (int i = 0; i < 2; i++) {
          D3D11_SAMPLER_DESC sd = {i ? D3D11_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT,
                                   D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
          sd.MaxLOD = D3D11_FLOAT32_MAX;
          d.dev->CreateSamplerState(&sd, &smp[i]);
       }
       ID3D11PixelShader *ps = ps11(d, "Texture2D t; SamplerState s0 : register(s0); SamplerState s1 : register(s1); "
                                       "float4 main(float4 p : SV_Position) : SV_Target { float2 uv = (fmod(floor(p.xy / 4), 3) + 1) / 4; "
                                       "return p.x < 16 ? t.Sample(s0, uv) : t.Sample(s1, uv); }", e);
       RT11 rt;
       const float black[4] = {};
       bool ok = srv && smp[0] && smp[1] && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black);
       if (ok) {
          d.ctx->PSSetShaderResources(0, 1, &srv);
          d.ctx->PSSetSamplers(0, 2, smp);
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       release(smp[1]);
       release(smp[0]);
       release(srv);
       release(t);
       return ok;
    }, 1, true},
   {"Tiled resources tier 2: residency and unmapped tiles (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
       ID3D11DeviceContext2 *ctx2 = nullptr;
       if (o1.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2 ||
           FAILED(d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext2), (void **)&ctx2))) {
          e = "not supported (optional)";
          return false;
       }
       // a 2x2-tile texture with tiles (0,0) and (1,1) mapped: each pixel point-samples one texel and its status;
       // unmapped ones read 0 and report it (magenta added)
       ID3D11Buffer *pool = tilePool11(d, 2);
       ID3D11Texture2D *t = tiledTexture11(d, 256, 256, 1, D3D11_BIND_SHADER_RESOURCE);
       ID3D11ShaderResourceView *srv = nullptr;
       if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
       D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                D3D11_TEXTURE_ADDRESS_CLAMP};
       sd.MaxLOD = D3D11_FLOAT32_MAX;
       ID3D11SamplerState *smp = nullptr;
       d.dev->CreateSamplerState(&sd, &smp);
       ID3D11PixelShader *ps = ps11(d, "Texture2D t; SamplerState s; float4 main(float4 p : SV_Position) : SV_Target { "
                                       "float2 uv = (floor(p.xy) * 8 + 4) / 256; uint status; "
                                       "float4 c = t.Sample(s, uv, int2(0, 0), 0.0f, status); "
                                       "return CheckAccessFullyMapped(status) ? c : float4(1, 0, 1, 1) + c; }", e);
       RT11 rt;
       const float black[4] = {};
       bool ok = pool && srv && smp && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black);
       if (ok) {
          D3D11_TILED_RESOURCE_COORDINATE at[2] = {{0, 0, 0, 0}, {1, 1, 0, 0}};
          UINT start[2] = {0, 1};
          D3D11_TILE_REGION_SIZE one = {1, FALSE};
          D3D11_TILE_REGION_SIZE sizes[2] = {one, one};
          UINT counts[2] = {1, 1};
          ctx2->UpdateTileMappings(t, 2, at, sizes, pool, 2, nullptr, start, counts, 0);
          std::vector<uint8_t> a = tilePattern(13), b = tilePattern(17);
          ctx2->UpdateTiles(t, &at[0], &one, a.data(), 0);
          ctx2->UpdateTiles(t, &at[1], &one, b.data(), 0);
          d.ctx->PSSetShaderResources(0, 1, &srv);
          d.ctx->PSSetSamplers(0, 1, &smp);
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       release(smp);
       release(srv);
       release(t);
       release(pool);
       release(ctx2);
       return ok;
    }, 0, true},
   {"LOD clamp (sample_cl, 11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
       if (o1.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2) {
          e = "not supported (optional)";
          return false;
       }
       // an 8x8 texture whose 4 mips have colors of their own, magnified: Sample clamped to mip 2 (left), SampleBias
       // with a bias below 0 clamped to mip 1 (right)
       D3D11_TEXTURE2D_DESC td = {8, 8, 4, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0};
       std::vector<uint32_t> mips[4];
       D3D11_SUBRESOURCE_DATA init[4];
       for (UINT m = 0; m < 4; m++) {
          UINT w = 8 >> m;
          mips[m].assign(w * w, 0xff000000u | (m * 60u) | ((255u - m * 60u) << 8) | (m * 20u << 16));
          init[m] = {mips[m].data(), w * 4, 0};
       }
       ID3D11Texture2D *t = nullptr;
       d.dev->CreateTexture2D(&td, init, &t);
       ID3D11ShaderResourceView *srv = nullptr;
       if (t) d.dev->CreateShaderResourceView(t, nullptr, &srv);
       D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                D3D11_TEXTURE_ADDRESS_CLAMP};
       sd.MaxLOD = D3D11_FLOAT32_MAX;
       ID3D11SamplerState *smp = nullptr;
       d.dev->CreateSamplerState(&sd, &smp);
       ID3D11PixelShader *ps = ps11(d, "Texture2D t; SamplerState s; float4 main(float4 p : SV_Position) : SV_Target { "
                                       "float2 uv = p.xy / 32; return p.x < 16 ? t.Sample(s, uv, int2(0, 0), 2.0f) "
                                       ": t.SampleBias(s, uv, -4.0f, int2(0, 0), 1.0f); }", e);
       RT11 rt;
       const float black[4] = {};
       bool ok = srv && smp && ps && makeRT(d, rt, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black);
       if (ok) {
          d.ctx->PSSetShaderResources(0, 1, &srv);
          d.ctx->PSSetSamplers(0, 1, &smp);
          fullscreen11(d, ps);
          ok = readTexture(d, rt.tex, 0, o);
       }
       rt.release();
       release(ps);
       release(smp);
       release(srv);
       release(t);
       return ok;
    }, 0, true},
   {"ClearView on depth-only formats (11.2)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1);
       ID3D11DeviceContext1 *ctx1 = nullptr;
       d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
       if (!o1.ClearViewAlsoSupportsDepthOnlyFormats || !ctx1) {
          e = "not supported (optional)";
          release(ctx1);
          return false;
       }
       ID3D11Texture2D *t = tex11(d, RW, RH, DXGI_FORMAT_D32_FLOAT, nullptr, 0, D3D11_BIND_DEPTH_STENCIL);
       ID3D11DepthStencilView *dsv = nullptr;
       if (t) d.dev->CreateDepthStencilView(t, nullptr, &dsv);
       bool ok = dsv != nullptr;
       if (ok) {
          d.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
          const float depth[4] = {0.25f, 0, 0, 0};
          D3D11_RECT r[] = {{4, 6, 20, 14}, {10, 18, 30, 28}};
          ctx1->ClearView(dsv, depth, r, 2);
          ok = readTexture(d, t, 0, o);
       }
       release(dsv);
       release(t);
       release(ctx1);
       return ok;
    }, 0, true},
   {"Depth bias (DepthBias on a 16-bit depth buffer)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       // a triangle at depth 0.25 with a bias of 100 units: each texel as 0 (cleared), 1 (the triangle, biased) or 2
       // (the triangle, not biased). Classes, not depths: GL leaves the unit to the implementation (NVIDIA's is twice
       // D3D's 1/65536)
       ID3D11Texture2D *t = tex11(d, RW, RH, DXGI_FORMAT_D16_UNORM, nullptr, 0, D3D11_BIND_DEPTH_STENCIL);
       ID3D11DepthStencilView *dsv = nullptr;
       if (t) d.dev->CreateDepthStencilView(t, nullptr, &dsv);
       D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 100, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
       ID3D11RasterizerState *rs = nullptr;
       d.dev->CreateRasterizerState(&rd, &rs);
       ID3D11VertexShader *vs = vs11(d, "float4 main(uint id : SV_VertexID) : SV_Position { "
                                        "float2 v[3] = {float2(-0.9, -0.8), float2(0.7, 0.95), float2(0.95, -0.6)}; "
                                        "return float4(v[id], 0.25, 1); }", e);
       bool ok = dsv && rs && vs;
       if (ok) {
          d.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
          d.ctx->OMSetRenderTargets(0, nullptr, dsv);
          D3D11_VIEWPORT vp = {0, 0, RW, RH, 0, 1};
          d.ctx->RSSetViewports(1, &vp);
          d.ctx->RSSetState(rs);
          d.ctx->IASetInputLayout(nullptr);
          d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          d.ctx->VSSetShader(vs, nullptr, 0);
          d.ctx->PSSetShader(nullptr, nullptr, 0);
          d.ctx->Draw(3, 0);
          std::vector<uint32_t> raw;
          ok = readTexture(d, t, 0, raw);
          o.clear();
          for (size_t i = 0; ok && i < (size_t)RW * RH; i++) {
             uint16_t v = ((const uint16_t *)raw.data())[i];
             o.push_back(v == 0xffff ? 0 : v >= 16384 + 50 ? 1 : 2);
          }
       }
       release(vs);
       release(rs);
       release(dsv);
       release(t);
       return ok;
    }, 0, false},
   {"Map on default textures (WriteToSubresource, ReadFromSubresource; 11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2 = {};
       d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2);
       ID3D11Device3 *dev3 = nullptr;
       ID3D11DeviceContext3 *ctx3 = nullptr;
       d.dev->QueryInterface(__uuidof(ID3D11Device3), (void **)&dev3);
       d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext3), (void **)&ctx3);
       if (!o2.MapOnDefaultTextures || !dev3 || !ctx3) {
          e = "not supported (optional)";
          release(dev3);
          release(ctx3);
          return false;
       }
       // a default texture the CPU writes (a pattern), the GPU changes (a rectangle cleared) and the CPU reads back
       D3D11_TEXTURE2D_DESC1 td = {RW, RH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                   D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0, D3D11_TEXTURE_LAYOUT_UNDEFINED};
       ID3D11Texture2D1 *t = nullptr;
       HRESULT h = dev3->CreateTexture2D1(&td, nullptr, &t);
       ID3D11RenderTargetView *rtv = nullptr;
       if (t) d.dev->CreateRenderTargetView(t, nullptr, &rtv);
       bool ok = SUCCEEDED(h) && rtv;
       if (!ok) e = "CreateTexture2D1: " + hrs(h);
       if (ok) {
          std::vector<uint8_t> init(RW * RH * 4);
          fillPattern(init, 33);
          h = ctx3->Map(t, 0, D3D11_MAP_WRITE, 0, nullptr);
          if (SUCCEEDED(h)) {
             dev3->WriteToSubresource(t, 0, nullptr, init.data(), RW * 4, 0);
             ctx3->Unmap(t, 0);
             ID3D11DeviceContext1 *ctx1 = nullptr;
             d.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
             const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
             const D3D11_RECT r = {5, 3, 20, 11};
             if (ctx1) {
                ctx1->ClearView(rtv, color, &r, 1);
                ctx1->Release();
             }
             h = ctx3->Map(t, 0, D3D11_MAP_READ, 0, nullptr);
          }
          if (SUCCEEDED(h)) {
             o.assign(RW * RH, 0);
             dev3->ReadFromSubresource(o.data(), RW * 4, 0, t, 0, nullptr);
             ctx3->Unmap(t, 0);
          } else {
             e = "Map: " + hrs(h);
             ok = false;
          }
       }
       release(rtv);
       release(t);
       release(dev3);
       release(ctx3);
       return ok;
    }, 1, true},
   {"Render target and viewport index from the vertex shader (11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return layerFromAnyShader(d, o, e, false);
    }, 1, true},
   {"Render target and viewport index from the domain shader (11.3)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return layerFromAnyShader(d, o, e, true);
    }, 1, true},
   {"Clip distances from the domain shader (as Direct2D draws curves)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return clipFromDomainShader(d, o, e);
    }, 2},
   {"Curve patches as Direct2D tessellates them (5 control points, forked edge factors)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return d2dCurvePatch(d, o, e);
    }, 2},
   {"Curve patches into coverage masks (R16_UINT, XOR, ForcedSampleCount 16), as Direct2D fills", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return d2dCurvePatch(d, o, e, true);
    }, 0, true},
   {"Tessellation with target-independent rasterization (ForcedSampleCount 16, clip distances)", [](Dev &d, std::vector<uint32_t> &o, std::string &e) {
       return clipFromDomainShader(d, o, e, 16);
    }, 2, true},
};

// Resources and shaders created from several threads at once
static bool multithreadedCreation(ID3D11Device *dev, std::string &detail)
{
   std::atomic<int> failures(0);
   std::vector<std::thread> threads;
   for (int t = 0; t < 4; t++) {
      threads.emplace_back([&, t] {
         for (int i = 0; i < 25; i++) {
            D3D11_TEXTURE2D_DESC td = {(UINT)(16 + t * 8 + i), 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                       D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
            ID3D11Texture2D *tex = nullptr;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) failures++;
            ID3D11RenderTargetView *rtv = nullptr;
            if (tex && FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) failures++;
            release(rtv);
            release(tex);
         }
      });
   }
   for (auto &th : threads) th.join();
   detail = fmt("4 threads x 25 textures and views, %d failures", failures.load());
   return failures == 0;
}

// A texture rendered on one device and read on another (shared handle)
static bool sharedTexture(ID3D11Device *a, ID3D11DeviceContext *actx, std::string &detail)
{
   ID3D11Device *b = nullptr;
   ID3D11DeviceContext *bctx = nullptr;
   const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
   createDevice11(&b, &bctx, &fl, 1);
   D3D11_TEXTURE2D_DESC td = {16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
   ID3D11Texture2D *tex = nullptr, *opened = nullptr, *staging = nullptr;
   ID3D11RenderTargetView *rtv = nullptr;
   IDXGIResource *res = nullptr;
   HANDLE handle = nullptr;
   bool ok = false;
   if (b && SUCCEEDED(a->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(a->CreateRenderTargetView(tex, nullptr, &rtv))) {
      const float color[4] = {0.75f, 0.25f, 0.5f, 1};
      actx->ClearRenderTargetView(rtv, color);
      actx->Flush();
      tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
      if (res && SUCCEEDED(res->GetSharedHandle(&handle)) &&
          SUCCEEDED(b->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&opened))) {
         td.BindFlags = 0;
         td.MiscFlags = 0;
         td.Usage = D3D11_USAGE_STAGING;
         td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         b->CreateTexture2D(&td, nullptr, &staging);
         bctx->CopyResource(staging, opened);
         D3D11_MAPPED_SUBRESOURCE m;
         if (staging && SUCCEEDED(bctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            uint32_t p = *(uint32_t *)m.pData;   // BGRA
            int r = (p >> 16) & 255, g = (p >> 8) & 255, bl = p & 255;
            ok = near8(r, 191, 2) && near8(g, 64, 2) && near8(bl, 128, 2);
            detail = fmt("the other device reads (%d,%d,%d), expected (191,64,128)", r, g, bl);
            bctx->Unmap(staging, 0);
         }
      } else {
         detail = "sharing failed";
      }
   } else {
      detail = "creating the shared texture failed";
   }
   release(staging);
   release(opened);
   release(res);
   release(rtv);
   release(tex);
   release(bctx);
   release(b);
   return ok;
}

// An NV12 texture (11.4's ExtendedNV12SharedTextureSupported) made on one device, read on another through a shared
// handle (NT or legacy): both planes as written
static bool sharedNV12(ID3D11Device *a, bool nt, std::string &detail)
{
   const UINT W = 64, H = 32;
   std::vector<uint8_t> data(W * (H + H / 2));
   for (UINT y = 0; y < H; y++)
      for (UINT x = 0; x < W; x++) data[y * W + x] = (uint8_t)(x * 7 + y * 3);
   for (UINT y = 0; y < H / 2; y++)
      for (UINT x = 0; x < W; x += 2) data[(H + y) * W + x] = (uint8_t)(64 + x), data[(H + y) * W + x + 1] = (uint8_t)(192 - y);
   ID3D11Device *b = nullptr;
   ID3D11DeviceContext *bctx = nullptr;
   const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   createDevice11(&b, &bctx, &fl, 1);
   D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                              (UINT)(nt ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : D3D11_RESOURCE_MISC_SHARED)};
   D3D11_SUBRESOURCE_DATA init = {data.data(), W, 0};
   ID3D11Texture2D *tex = nullptr, *opened = nullptr, *staging = nullptr;
   ID3D11Device1 *b1 = nullptr;
   IDXGIResource1 *res = nullptr;
   HANDLE handle = nullptr;
   HRESULT h = b ? a->CreateTexture2D(&td, &init, &tex) : E_FAIL;
   bool ok = false;
   if (FAILED(h)) {
      detail = "creating the shared NV12 texture: " + hrs(h);
   } else {
      ID3D11DeviceContext *actx = nullptr;
      a->GetImmediateContext(&actx);
      actx->Flush();   // (the initial data reaches the texture before the other device reads it, as D3D asks)
      release(actx);
      tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res);
      h = !res ? E_NOINTERFACE
          : nt ? res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)
               : res->GetSharedHandle(&handle);
      if (SUCCEEDED(h) && nt) {
         b->QueryInterface(__uuidof(ID3D11Device1), (void **)&b1);
         h = b1 ? b1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **)&opened) : E_NOINTERFACE;
      } else if (SUCCEEDED(h)) {
         h = b->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&opened);
      }
      if (FAILED(h)) {
         detail = "sharing it: " + hrs(h);
      } else {
         D3D11_TEXTURE2D_DESC od;
         opened->GetDesc(&od);
         td.BindFlags = 0;
         td.MiscFlags = 0;
         td.Usage = D3D11_USAGE_STAGING;
         td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
         b->CreateTexture2D(&td, nullptr, &staging);
         if (staging) bctx->CopyResource(staging, opened);
         D3D11_MAPPED_SUBRESOURCE m;
         if (od.Format != DXGI_FORMAT_NV12 || od.Width != W || od.Height != H) {
            detail = fmt("the other device opens a %ux%u texture of format %d", od.Width, od.Height, (int)od.Format);
         } else if (staging && SUCCEEDED(bctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            int bad = 0, first = -1;
            for (UINT y = 0; y < H + H / 2; y++)
               for (UINT x = 0; x < W; x++)
                  if (((const uint8_t *)m.pData)[y * m.RowPitch + x] != data[y * W + x] && bad++ == 0) first = (int)(y * W + x);
            ok = !bad;
            const uint8_t *p = (const uint8_t *)m.pData;
            detail = bad ? fmt("%d of %u bytes differ (first at row %d, column %d: %s plane; reads %02x %02x %02x %02x, "
                               "chroma %02x %02x)", bad, W * (H + H / 2), first / (int)W, first % (int)W,
                               first / (int)W < (int)H ? "luma" : "chroma", p[0], p[1], p[2], p[3], p[H * m.RowPitch],
                               p[H * m.RowPitch + 1])
                         : "both planes read back on the other device";
            bctx->Unmap(staging, 0);
         } else {
            detail = "reading it back failed";
         }
      }
   }
   if (nt && handle) CloseHandle(handle);
   release(staging);
   release(opened);
   release(b1);
   release(res);
   release(tex);
   release(bctx);
   release(b);
   return ok;
}

static void groupD3D11Render()
{
   Dev hw, warp;
   bool hw_ok = setupPair(hw, warp);
   if (!warp.dev) return;
   for (const RenderTest &t : RENDER_TESTS) {
      if (!startTest(t.name)) continue;
      std::vector<uint32_t> a, b;
      std::string detail, warpDetail;
      resetCtx(warp);
      bool refOk = t.fn(warp, b, warpDetail);
      resetCtx(hw);
      bool ok = hw_ok && t.fn(hw, a, detail);
      if (!refOk) {
         if (t.optionalFeature && !ok) optional(false, t.name, detail);
         else report(Status::Skip, t.name, "WARP failed: " + warpDetail);
         continue;
      }
      if (!ok) {
         if (t.optionalFeature) optional(false, t.name, detail.empty() ? "not supported (optional)" : detail);
         else check(false, t.name, detail.empty() ? "the GPU run failed" : detail);
         continue;
      }
      int bad = 0, firstBad = -1;
      hashOutputs(t.name, a, b);
      if (a.size() != b.size()) {
         check(false, t.name, fmt("%zu values, WARP %zu", a.size(), b.size()));
         continue;
      }
      for (size_t i = 0; i < a.size(); i++) {
         bool same = true;
         if (t.rgb10a2) {
            for (int c = 0; c < 32; c += 10) {
               int mask = c == 30 ? 3 : 1023;
               if (std::abs((int)((a[i] >> c) & mask) - (int)((b[i] >> c) & mask)) > t.tolerance) same = false;
            }
         } else {
            for (int c = 0; c < 32; c += 8) {
               if (std::abs((int)((a[i] >> c) & 255) - (int)((b[i] >> c) & 255)) > t.tolerance) same = false;
            }
         }
         if (!same && firstBad < 0) firstBad = (int)i;
         bad += !same;
      }
      std::string why = bad ? fmt("%d of %zu values differ from WARP's (first: #%d, 0x%08x vs 0x%08x)", bad, a.size(), firstBad,
                                  a[firstBad], b[firstBad])
                            : detail;
      check(!bad, t.name, why);
   }
   if (hw_ok && startTest("Resource creation from several threads")) {
      std::string detail;
      check(multithreadedCreation(hw.dev, detail), "Resource creation from several threads", detail);
      detail.clear();
      if (startTest("Textures shared between devices")) check(sharedTexture(hw.dev, hw.ctx, detail), "Textures shared between devices", detail);
      D3D11_FEATURE_DATA_D3D11_OPTIONS4 o4 = {};
      bool nv12 = SUCCEEDED(hw.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS4, &o4, sizeof o4)) &&
                  o4.ExtendedNV12SharedTextureSupported;
      // (single textures: Direct3D 11 refuses shared NV12 arrays itself, WARP's as well)
      for (bool nt : {false, true}) {
         const char *name = nt ? "NV12 textures shared between devices (NT handle)" : "NV12 textures shared between devices";
         if (!startTest(name)) continue;
         detail.clear();
         if (!nv12) optional(false, name, "the driver doesn't offer it (11.4 ExtendedNV12SharedTextureSupported)");
         else check(sharedNV12(hw.dev, nt, detail), name, detail);
      }
   }
}

// ---------------------------------------------------------------------------------------------------------------
// Presenting to a window: the swap chain models

static void groupD3D11Present()
{
   if (!interactiveDesktop()) {
      report(Status::Skip, "Presenting to a window", "no interactive desktop");
      return;
   }
   const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
   ID3D11Device *dev = nullptr;
   ID3D11DeviceContext *ctx = nullptr;
   HRESULT h = createDevice11(&dev, &ctx, &fl, 1, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
   if (!dev) {
      checkAtLevel(fl, false, "Direct3D 11 device", hrs(h));
      return;
   }
   IDXGIDevice *dxgiDev = nullptr;
   IDXGIAdapter *adapter = nullptr;
   IDXGIFactory2 *factory = nullptr;
   dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev);
   dxgiDev->GetAdapter(&adapter);
   adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
   if (!factory) {
      if (isWindows7()) optional(false, "DXGI 1.2 factory", "Windows 7: DXGI 1.2 comes with its Platform Update (KB2670838)");
      else check(false, "DXGI 1.2 factory");
      return;
   }
   struct Mode {
      const char *name;
      DXGI_SWAP_EFFECT effect;
      DXGI_FORMAT format;
      UINT buffers;
      UINT interval;
      bool resize;
      int green = 128;   // how 0.5 shows: half-float swap chains are linear (scRGB)
   } modes[] = {
      {"Bit-block transfer swap chain (DISCARD)", DXGI_SWAP_EFFECT_DISCARD, DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0, false},
      {"Bit-block transfer swap chain (SEQUENTIAL)", DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, false},
      {"Flip model swap chain (FLIP_SEQUENTIAL)", DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 0, false},
      {"Flip model swap chain (FLIP_DISCARD)", DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, false},
      {"HDR / 10-bit swap chain (R10G10B10A2, flip)", DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_FORMAT_R10G10B10A2_UNORM, 2, 0, false},
      {"Half-float swap chain (R16G16B16A16_FLOAT, flip)", DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 0, false, 188},
      {"Presenting with vsync (sync interval 1)", DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 1, false},
      {"Resizing the swap chain (ResizeBuffers)", DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 0, true},
   };
   const int S = 96;
   for (auto &m : modes) {
      if (!startTest(m.name)) continue;
      if (m.effect == DXGI_SWAP_EFFECT_FLIP_DISCARD && !windows10OrLater()) {
         optional(false, m.name, "FLIP_DISCARD needs Windows 10 or later");
         continue;
      }
      HWND w = createWindow(m.name, S, S, true);
      DXGI_SWAP_CHAIN_DESC1 sd = {};
      sd.Width = sd.Height = S;
      sd.Format = m.format;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount = m.buffers;
      sd.SwapEffect = m.effect;
      IDXGISwapChain1 *sc = nullptr;
      HRESULT ch = factory->CreateSwapChainForHwnd(dev, w, &sd, nullptr, nullptr, &sc);
      if (!sc) {
         check(false, m.name, "CreateSwapChainForHwnd: " + hrs(ch));
         DestroyWindow(w);
         continue;
      }
      if (m.resize) {
         RECT r = {0, 0, S + 32, S + 32};
         AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
         SetWindowPos(w, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
         sc->ResizeBuffers(0, S + 32, S + 32, DXGI_FORMAT_UNKNOWN, 0);
      }
      const float color[4] = {0.0f, 0.5f, 1.0f, 1};
      ULONGLONG start = GetTickCount64();
      const int frames = m.interval ? 30 : 12;
      bool presented = true;
      for (int i = 0; i < frames; i++) {
         ID3D11Texture2D *bb = nullptr;
         ID3D11RenderTargetView *rtv = nullptr;
         sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
         if (bb) dev->CreateRenderTargetView(bb, nullptr, &rtv);
         if (rtv) ctx->ClearRenderTargetView(rtv, color);
         release(rtv);
         release(bb);
         if (FAILED(sc->Present(m.interval, 0))) presented = false;
         pumpMessages();
         if (!m.interval) Sleep(20);
      }
      double seconds = (GetTickCount64() - start) / 1000.0;
      Sleep(300);
      pumpMessages();
      COLORREF c = 0;
      bool shown = windowPixel(w, S / 2, S / 2, c) && near8(GetRValue(c), 0, 10) && near8(GetGValue(c), m.green, 10) &&
                   near8(GetBValue(c), 255, 10);
      std::string detail = fmt("window shows (%d,%d,%d), expected (0,%d,255)", GetRValue(c), GetGValue(c), GetBValue(c), m.green);
      if (m.interval) {
         double fps = frames / seconds;
         detail += fmt("; %.0f presents per second", fps);
         shown = shown && fps > 20 && fps < 400;
      }
      check(presented && shown, m.name, presented ? detail : "Present failed");
      sc->Release();
      DestroyWindow(w);
   }
   factory->Release();
   adapter->Release();
   dxgiDev->Release();
   ctx->Release();
   dev->Release();
}
