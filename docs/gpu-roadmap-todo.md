# GPU driver roadmap: D3D11 optional features, Direct2D, Vulkan, Direct3D 12, standalone installer

Asked for on 2026-10-14. Decisions: a **real Direct3D 12 driver** (a D3D12 user-mode driver that Windows' own
`d3d12.dll` loads for every app, not per-app vkd3d-proton DLLs); **D3D11 optional features and Direct2D**, both
tested; the standalone installer for **plain QEMU VMs with Windows 8.1, 10 and 11** (8.0 not).

Order: A, A2, B, C, E, D (2026-10-15: C3, G, E, T, then D); each stage builds on the ones before (E needs B's Vulkan
and C's D3D12; the standalone installer D ships all of it). Every stage ends with tests in `tools/dx-test` (spike
first, then the WinBoat VM) before the next one starts.

Legend: `[ ]` open · `[~]` in progress · `[x]` done

## A. Direct3D 11 optional features and Direct2D (Mesa d3d10umd, OpenGL route)

- [x] Inventory (2026-10-14). The kernel driver is WDDM 1.3, so the user-mode driver speaks the D3D11.1/WDDM 1.3
      interface. Through it the runtime can only be told about: doubles (+ 11.1 extended doubles), logic ops,
      feature level 11_1, tiled resources tiers (and with tier 2, min/max filtering). Everything of 11.3 (typed UAV
      loads of more formats, stencil reference from pixel shaders, ROVs, conservative rasterization, VP/RT index
      from any shader, Map on default buffers, ClearView on depth) is reported through the WDDM 2.0 interface:
      it comes with the WDDM 2.x kernel driver of stage C (D3D12 needs it too). Windows 8.1 (stage D) keeps a
      WDDM 1.3 build. Host (NVIDIA GL through virglrenderer): doubles (ARB_gpu_shader_fp64), logic ops, stencil
      export are passed on; sparse textures (tiled resources), raster multisample (11.1 target-independent
      rasterization) and conservative rasterization are not (host patches needed).
- [x] Double precision in shaders (DXBC double instructions → TGSI; `D3D11_FEATURE_DOUBLES`, 11.1 extended doubles)
- [x] Logic operations in the output merger (11.1)
- [x] Feature level 11_1: UAVs in every stage, 64 UAV slots, target-independent rasterization and UAV-only
      rendering with a forced sample count (virglrenderer patch 0004, image 6.05-7), ClearView on UAVs; the
      desktop (DWM) runs at 11_1 fine
- [x] Tiled resources (tier 1/2) and min/max filtering: thought impossible on the OpenGL route (D3D tile pools alias
      memory between resources; ARB_sparse_texture only commits driver-owned pages) - NV_memory_object_sparse binds
      sparse pages to a memory object at offsets, which is D3D's tile pool exactly (done 2026-10-24, below)
- [x] 11.3 options the host allows (2026-10-21, gpu.73: Mesa 0029, virglrenderer 0016, image 6.05-24). The D3D11
      runtime reads them from no driver below the WDDM 2.0 DDI, so d3d10umd now offers it (WINBOAT_D3D_WDDM2=0 leaves
      it out): the 2.0 table is the 1.3 one with longer arguments in seven places (views' plane slices, conservative
      rasterization mode, a query's context type, Flush) and four more entries, plus DXGI 1.4's and the 2.0 video
      table (SubmitDecoderBuffers1, StreamColorSpace1, format conversion checks). The runtime refused every device
      until GPUVA_CAPS answered (MaxGPUVirtualAddressBitsPerResource 40; found with cdb on its journal strings).
      Now: typed UAV loads of more formats (UAV_READS where UAV writes are), RT/viewport index from vertex and domain
      shaders (domain: GL_ARB_shader_viewport_layer_array, new cap bit VIRGL_CAP_V2_TES_LAYER_VIEWPORT), Map on default
      textures (the runtime offers it on 2.0; GetResourceLayout reports the virgl guest storage row-major).
      Left: stencil reference from pixel shaders (translated, on when the host GL has ARB_shader_stencil_export;
      NVIDIA's GL has not; checked offline through the translator harness and vrend_glsl only), conservative
      rasterization and ROVs (the host has GL_NV_conservative_raster and fragment_shader_interlock, virgl has no
      protocol for them), standard swizzle. dx-test: 5 new 11.3 tests. Spike 663/0, user VM 661/0
      Found on the way: mesa patch 0028 (HEVC RExt) had been cut down to its last fix by a regeneration over
      itself; rebuilt in full (the series reproduces the spike tree again)
- [x] Conservative rasterization (tier 1) and ROVs (2026-10-24, spike: Mesa 0037, virglrenderer 0019/0020, image
      6.05-28). Conservative: the WDDM 2.0 rasterizer state's mode goes to gallium's conservative_raster_mode, virgl sends
      VIRGL_CCMD_SET_CONSERVATIVE_RASTER (new command, cap bit 28) and the host turns GL_NV_conservative_raster on around
      the guest's draws. ROVs: a pixel shader declaring one gets the TGSI property FS_INTERLOCK (cap bit 27); the host
      renames its main and calls it between beginInvocationInterlockARB/endInvocationInterlockARB (they may be neither
      in control flow nor after a return), GLSL 1.50 for the layout qualifier. The ROV UAVs must be coherent: without it
      every invocation read stale values (the interlock orders, coherence makes the writes visible). Found on the way:
      the UAV declarations' flags were decoded with DECODE_D3D11_SB_UAV_FLAGS (the counter bit only), so
      globally-coherent never reached the shaders. dx-test: draws a sliver triangle per pixel (conservative only) and
      64 instanced full-screen RMWs in order (ROV), both against WARP. Spike 694/0
- [x] What's left of 11.2-11.4, and why (2026-10-24, probed): min/max filtering and ClearView on depth formats have no
      DDI cap of their own - the runtime derives them from the tiled resources tier (reporting tier 1 turned on
      ClearView on depth, tier 2 min/max filtering), so they come only with tiled resources (stage C's note above;
      on this route it would need GL_NV_memory_object_sparse-style aliasing of tile pools). Shared NV12 textures
      (11.4): the UMD's DDI version (done below: the WDDM 2.1 DDI). Not the driver's: stencil
      reference from pixel shaders (NVIDIA's GL has no ARB_shader_stencil_export, and NVIDIA's D3D11 driver doesn't
      offer it either) and standard swizzle (NVIDIA offers it nowhere): dx-test counts them as optional, not failures
- [x] Tiled resources, tier 2, and min/max filtering (2026-10-24, spike: Mesa 0038, virglrenderer 0021-0023, image
      6.05-29). The host's sparse pages are D3D's standard tile shapes (NVIDIA: 256x256 at 8 bits ... 64x64 at 128, BC1
      512x256, checked at start); a tiled resource is sparse storage (ARB_sparse_texture2/sparse_buffer), a tile pool
      device-local Vulkan memory GL imports, and VIRGL_CCMD_TILE_MAP binds pages into it (NV_memory_object_sparse's
      TexturePageCommitmentMemNV: aliasing is real, no copies). The UMD (Tiled.cpp) keeps every tile's mapping, packs
      mips below a tile (the host's mip tail, a tile a slice), grows pools in chunks; tiled textures transfer through
      staging buffers, tiled buffers and pools take GPU copies only. Found: virgl skipped the readback of textures it
      thought clean - an aliased tile changes behind it, so sparse ones always read back. Tier 2: unmapped tiles read
      0; the feedback instructions (sample/gather/ld *_s, check_access_fully_mapped) and sample_*_cl through a TGSI
      SPARSE before the texture instruction (the host emits the lookup's Clamp form and the same lookup's sparse form
      for the residency); min/max filtering (sampler S0 bits 26-27 -> GL_TEXTURE_REDUCTION_MODE_ARB; MAXIMUM_* filters
      had been taken for comparison ones by their 0x80 bit). With tier 1 the runtime adds ClearView on depth-only
      formats (DSV clears in rectangles). dx-test: 7 new tests (mappings/aliasing/rendering, CopyTileMappings and
      CopyTiles and tiled buffers, packed mips, min/max, residency and unmapped reads, LOD clamp, ClearView on depth),
      against WARP. Spike 711/0. Left of 11.2-11.4: stencil reference and standard swizzle (NVIDIA has neither), shared
      NV12 (next entry)
- [x] Shared NV12 textures, 11.4's ExtendedNV12SharedTextureSupported (2026-10-24, spike: Mesa 0040, kvm 0032). Not
      WDDM 2.4 as thought: d3d11.dll's CheckFeatureSupport (OPTIONS4) reads the negotiated DDI version (>= 5, which
      the WDDM 2.1 DDI is; found with cdb on CDevice+0x41c), the kernel driver's WDDM version plays no part. The UMD
      offers D3DWDDM2_1 (2.0's device table plus the sync token entries, DXGI 1.5's base table, and the video table the
      runtime asks for under sub-ID 5 - the regression caught its absence: no D3D11 video device at all). The sharing itself:
      the UMD makes NV12/P010 of two host textures, and a shared resource is the allocations made with it in one call,
      so the opener found the luma plane only. The luma plane's allocation of a shared NV12/P010 texture now also makes
      the chroma plane (kernel-only flag VIOGPU_RESOURCE_FLAG_CHROMA_PLANE; attached to every context with the
      allocation, destroyed with it; VIOGPU_RES_INFO returns its id), and OpenResource rebuilds the surface from both.
      dx-test: "NV12 textures shared between devices", legacy and NT handles, both planes compared. One slice: the
      D3D11 runtime refuses shared NV12 arrays itself (E_INVALIDARG before the driver sees them; WARP the same).
      Decoder output (Mesa 0041): shared ones get the shared planes too and decode into a codec buffer per slice,
      copied into the planes after each frame (as AYUV output is); dx-test "H.264 (AVC) decoding into a shared
      texture" reads the frame on another device
- [x] Optional formats: B4G4R4A4 (11.1) added. Left: R32G32B32_FLOAT sampling/rendering, BGRA8 typed UAVs
      (GL images have no BGRA formats) — niche
- [x] Direct2D test group in dx-test (`d2d`, `d2d-32`: 16 scenes compared with WARP; mismatches saved as BMPs)
- [x] Direct2D: GDI drawing through GetDC on a Direct2D/DXGI surface was lost: DXGI's ResolveSharedResource (the
      runtime calls it instead of flushing) was a stub
- [x] Direct2D: a spike in the stroke scene: SV_ClipDistance was never applied (D3D10/11 in general); Direct2D
      clips each geometry of a batch to its rectangle with four clip distances
- [x] Rasterization tie-breaking: 4 pixels exactly on an aliased edge differ from WARP (vertex snapping on exact
      ties); tolerated and named in the test's result
- [x] Mesa patch 0013 (+ virglrenderer patch 0004, image 6.05-7); spike: 449 passed, 1 failed (no D3D12), desktop
      fine at feature level 11_1
- [x] Tests in the WinBoat VM (with A2, gpu.37, image 6.05-7): 476 passed, 1 failed (no D3D12)

## A2. Direct3D 9 and 10/10.1 as complete drivers (standalone use in any QEMU VM)

Asked for on 2026-10-14: D3D9 and D3D10 must be full implementations, not just pass today's tests.

- [x] D3D9 driver interface audit (2026-10-14): DepthFill, ResolveSharedResource, UpdateSubresourceUP, GetInfo,
      counter queries implemented; still stubs only what's never offered (overlays, content protection, N-/RT-patches,
      P8 palettes, hardware state blocks, ComposeRects on A1 surfaces) and decoding (stage E). Original list:
- [x] (was) D3D9 driver interface audit: every entry point the D3D9/9Ex runtimes can call is implemented (today stubs:
      the Windows 8+ "…1" variants TexBlt1/VolBlt1/BufBlt1/Present1/Flush1/Discard, BufBlt, DepthFill, StateSet
      (hardware state blocks), GetInfo, Rename, OpenResource/CreateResource2/ResolveSharedResource (shared surfaces:
      9Ex interop, WPF, browsers), OfferResources/ReclaimResources/TrimResidencySet/QueryResourceResidency,
      SetPalette/UpdatePalette (P8 textures), DrawRectPatch/DrawTriPatch (if offered), UpdateWInfo, SetDisplayMode,
      markers, counters, overlays/multiplane checks answered properly)
- [x] D3D9 flip model (D3DSWAPEFFECT_FLIPEX) presents crashed in the runtime (DdBltLH, NULL kernel handle): the
      driver listed no 16-bit display mode, so the runtime's DirectX 8 HAL check marked the adapter as having no
      hardware primary and made no desktop surface. R5G6B5 is a display mode now (only one 16-bit one is allowed)
- [x] D3D9 exclusive full screen: SetDisplayMode implemented; flip chains (one resource, front + back buffers as
      subresources) have one surface per subresource; only the first allocation joins the runtime's resource
      (a second AllocateCb on it bugchecks 0x113/7)
- [x] D3D9Ex shared surfaces between two D3D9 devices read black: nine cleared the opened render target at creation
- [x] Lock writes lost after GPU work (LockRect on render targets, GetDC drawing): virgl turned READ|WRITE maps of
      staged textures into read-only staging maps (upstream bug; D3D10/11 Map(READ_WRITE) too)
- [x] D3D9 DXVA2 video processing (VideoProcessBlt: YUY2/UYVY/NV12/RGB to RGB, scaling, progressive and bob
      processors), YUV surfaces (StretchRect to RGB, ColorFill, locks), the VCACHE query/GetInfo
- [x] DXVA2 decoding (2026-10-17, gpu.52): see stage E ("DXVA2 decoding in the D3D9 driver")
- [x] D3D9 caps review: SM3 caps, vertex textures, MSAA 2-16, INTZ/DF16/DF24/NULL/ATI1/ATI2, ATOC (alpha to
      coverage) added, RESZ when the host can; the DLLs carry the driver version (was 0.0.0.0 to applications)
- [x] D3D10/10.1 interface audit (nothing advertised is missing; command lists, tiled resources, markers not offered): no unsupported paths left in the D3D10 DDI (shared resources, all query/
      predicate types, all formats' caps, DXGI functions)
- [x] dx-test coverage (d3d9-ex group: shared surfaces, D3D9->D3D11 sharing, flip model, full screen, GetDC,
      VCACHE, DXVA2 video processing for YUY2/UYVY/NV12, StretchRect YUV->RGB) in 64- and 32-bit
- [x] Tests: spike and WinBoat VM (gpu.37): 476 passed, 1 failed (no D3D12), 17 optional missing

## B. Vulkan (Venus)

- [x] Stage 1, host: virglrenderer with Venus (`tools/gpu-spike/venus`, done 2026-10-12)
- [x] Host route (2026-10-14): Venus in WinBoat's GPU process. QEMU patch 0008: vhost-user-gpu gets `hostmem`
      (BAR 4 as a virtio shared memory region), blob resources mapped with vhost-user SHMEM_MAP, Venus contexts and
      per-ring fences (`--venus`, started by qemu-wrapper.sh when the device has `hostmem=`). Two QEMU fixes on the
      way (mappings deadlocked against vhost's memory table; the reply came before the mapping existed). Image 6.05-8
      (virglrenderer with Venus + render server, libvulkan1, libxext6 for NVIDIA's Vulkan driver). Checked with a
      Linux guest (Mesa Venus): RTX 3080 listed, GPU fill + copy into host-visible memory correct, 20/20 runs.
      Test VM: ~/.cache/claude-winboat/venus-linux (run.sh, sh.py, vktest.c)
- [x] Kernel driver (2026-10-14, kvm patch 0013): finds the host-visible window (virtio shared memory capability
      id 1) and acknowledges `RESOURCE_BLOB` + `CONTEXT_INIT`; escapes `VIOGPU_BLOB_CREATE/MAP/DESTROY` (blobs owned
      by the D3DKMT device, attached to its Venus context, mapped into the process with the host's caching, cleaned
      up with the device) and `VIOGPU_EXECBUFFER` (a stream on one of the context's 64 rings; an event set when the
      ring's fence signals; empty streams allowed). Found on the way: the control queue frees a command's data
      buffer itself, so the completion callback freeing it too corrupted the kernel heap (bugcheck 0x13A)
- [x] Mesa (2026-10-14, Mesa patch 0014): `vn_renderer_wddm.c` (Venus over those escapes; renderer syncs emulated
      with the driver's events), Windows stand-ins in Venus, Win32 WSI on its software path (GPU copy to
      host-visible memory, GDI into the window). Built with clang-cl (LLVM 18; Venus needs C11 atomics and typeof,
      which the EWDK's MSVC 14.38 lacks), 64- and 32-bit (`vkbuild`, `vkbuild32` in the VM)
- [x] Packaging (kvm patch 0014): `viogpu_vk[_x86].dll` + manifests, `VulkanDriverName`/`VulkanDriverNameWow`, and
      the Khronos loader (vulkan-1.dll from the Vulkan runtime 1.3.296, Apache-2.0/MIT) into System32/SysWOW64
      unless a newer one is there (Windows 11 24H2 ships its own, Microsoft-signed 1.3.300+). Signed files keep
      their signature when the package is test-signed. setup-build-env.ps1 installs LLVM and the runtime (pinned
      hashes); sync-sources.sh puts DirectX-Headers v1.613.1 into Mesa's subprojects (Win32 WSI needs them)
- [x] dx-test groups `vulkan` and `vulkan-32`: loader, instance, the GPU's device, memory types, features and
      extensions (optional ones as warnings), 64 MB GPU fill + copy into host-visible memory, compute shader,
      triangle read back, texture upload + sampling, timeline semaphores, swapchain presenting to a window (color
      checked on the desktop). Spike: 38 passed, 0 failed (ray tracing, mesh shaders, video decode/encode not
      offered by the host's Venus)
- [x] Spike regression: DirectX test 476 passed, 1 failed (no D3D12), as before
- [x] Faster present (Mesa patch 0015): the 63 fps ceiling was Windows' 15.6 ms timer tick (Mesa's os_time_sleep
      used Sleep(), and Venus waits by polling with short sleeps): a high-resolution waitable timer fixes every Venus
      wait. Win32 WSI's software path now draws straight from the mapped image (StretchDIBits) on a present thread
      of its own, FIFO/MAILBOX paced by DwmFlush, IMMEDIATE and MAILBOX offered; Venus skips its queue-idle wait
      before presents on Windows. 1200x781: 63 -> 1093 fps (IMMEDIATE), FIFO at 60 Hz; resize with queued frames OK
      Deployed as gpu.39 (driver 100.6.101.59284): user VM 514 passed, 1 failed (no D3D12)
- [x] Zero-copy present (2026-10-23, gpu.83: Mesa 0035, mesa-venus 0009; see the performance section). Before: the CPU path cost ~1 ms per megapixel on the present
      thread. Measured 2026-10-21 (spike, deploy/tools/src/vkpresentbench.cpp, IMMEDIATE): 1621/912/568 fps at
      720p/1080p/1440p, vkQueuePresentKHR 0.54/1.01/1.68 ms (D3D11 ~2400 fps, 0.25 ms flat): ~0.45 ms per
      megapixel, ~5 ms at 5120x2065 (a ~200 fps ceiling and a CPU core; latency, not frame rate, below that).
      Would need a D3D11 device and DXGI swap chain inside Venus' WSI plus cross-API waits. The host can already share a Venus blob with a virgl context (dma-buf, `PIPE_RESOURCE_SET_TYPE` +
      EGL import); a D3D11/DXGI swap chain fed that way would skip the CPU. Needs a host check on NVIDIA's EGL
- [x] Tests in the WinBoat VM (gpu.38, 2026-10-14: image 6.05-8, `hostmem=4G`, driver 100.6.101.59253): DirectX
      test 514 passed, 1 failed (no D3D12), Vulkan and Vulkan 32-bit all passing

## C. Direct3D 12 (a real user-mode driver, on Vulkan)

Prerequisite found on 2026-10-14: Windows' D3D12 runtime only accepts adapters whose kernel driver is WDDM 2.0
or newer (ours is WDDM 1.3, so `D3D12CreateDevice` fails with DXGI_ERROR_UNSUPPORTED). Reference: Microsoft's
graphics-driver-samples, compute-only sample (`coskmd`: WDDM 2.x with GpuMmu and CPU-updated page tables,
DdiRender/DdiPatch next to DdiSubmitCommandVirtual; `cosumd12`: a D3D12 user-mode driver).

- [x] C0. Kernel driver on WDDM 2.0: GpuMmu with page tables the CPU updates (`DXGK_PAGETABLEUPDATE_CPU_VIRTUAL`;
      our virtual GPU never reads them, its commands name host resources), processes, root page tables,
      SubmitCommandVirtual, GPU clock calibration, monitored fences. The D3D9/10/11/OpenGL drivers keep submitting
      the WDDM 1.x way (allocation lists) and must pass the DirectX test as before. The WDDM 1.3 build stays (for
      Windows 8.1, stage D): a build switch
- [x] C0 findings (2026-10-15): the WDDM 2.0 kernel driver runs the desktop, but WDDM 2.0 drops the WDDM 1.x model
      our D3D9/D3D11/OpenGL drivers use: residency is explicit (MakeResident; 'Allocation is not requested to be
      resident'), and legacy contexts/allocation-list submissions fail. The user chose the full migration
      (2026-10-15): new submission path (SubmitCommandVirtual) in the kernel driver, residency + WDDM 2.0
      contexts/submissions in all three user-mode drivers, then D3D12. The user's VM stays on gpu.39 until the
      DirectX test passes again on WDDM 2.0
- [x] C0 debugging (2026-10-15), causes found with DxgKrnl ETW traces and a live kernel dump
      (`Get-StorageDiagnosticInfo -IncludeLiveDump`):
  - [x] Random hangs where the whole scheduler stalls (Present, DestroyContext waiting in VidSchFlushContext). The
        kernel driver said `PreemptionAware` but its DxgkDdiPreemptCommand did nothing, and dxgmms2 waits for
        DXGK_INTERRUPT_DMA_PREEMPTED. Fix: acknowledge the request once the commands submitted before it are done
        (the host runs each one to its end, so nothing is resubmitted)
  - [x] Command buffers dropped after Render ("allocation has not been fully committed ... sync packet?"), which
        left their allocations busy (RES_BUSY waits of 10 s, black readbacks). The runtimes' pfnMakeResidentCb
        reports pending paging work as E_PENDING, not STATUS_PENDING, so the user-mode drivers never waited for
        the paging fence. Fix: wait whenever MakeResident returns a paging fence value
  - [x] DirectX test on the WDDM 2.0 kernel driver: 514 passed, 1 failed (D3D12, no user-mode driver yet), as on
        WDDM 1.3
  - [x] OpenGL (D3DKMT path) rendered black: the window's back buffer, a resource shared from another device and
        opened with D3DKMTOpenResource, was never made resident for the rendering device; the first command buffer
        naming it put that device in error (VidSchMarkDeviceAsError). Fix: MakeResident after opening too
  - [x] Driver stop/remove hung (pnputil 480: query-remove): gone with the preemption fix; `pnputil
        /restart-device` and an in-place driver update work, and the GPU passes its tests afterwards
  - [x] Patches: kvm-guest-drivers-windows 0015 (WDDM 2.0 kernel driver, preemption), Mesa 0016 (residency);
        diagnostic logging trimmed to rare events (VIOGPU_GET_LOG escape). Not deployed to the user's VM yet
        (still gpu.39, WDDM 1.3)
- [x] C1. D3D12 user-mode driver skeleton (OpenAdapter12, caps, device): `D3D12CreateDevice` succeeds
      (2026-10-15, spike; a Mesa target `src/gallium/targets/d3d12umd` = viogpu_d3d12.dll, since replaced by C2;
      kvm patch 0016 = the fourth UserModeDriverName entry). DDI 0052 (R5 build 2), feature level 11_0, binding tier 1, SM 5.1;
      every entry not implemented is a logging stub (env `VIOGPU_D3D12_LOG=<file>`), 64-bit only. The runtime
      refuses IOCoherent FALSE and an unanswered TEXTURE_LAYOUT_SETS (found with cdb on D3D12Core's C++
      exceptions: `CDevice::CacheHeapCaps` + `CJournal::ReportDriverError` strings). What the runtime calls during
      D3D12CreateDevice already: root signature, vertex/compute shaders, blend/depth/rasterizer states, pipeline
      states, MakeResident, descriptor sizes, command pool/recorder/list/queue, fences
- [ ] C2. The real implementation on vkd3d-proton (64- and 32-bit):
  - [x] Venus vs vkd3d-proton's Vulkan profile (`VP_D3D12_VKD3D_PROTON_profile.json` against `vulkaninfo --json`
        in the guest): everything up to FL 12_1 + SM 6.0 (12_2 lacks ray tracing and mesh shaders);
        VK_KHR_calibrated_timestamps missing (optional; Venus has the EXT one)
  - [x] Stock vkd3d-proton v3.0.1 built in the spike with clang-cl (new tools in setup-build-env.ps1: glslang
        16.5.0, widl from MSYS2's mingw-w64-tools, VC++ runtime 14.44 for glslang), its own d3d12.dll next to
        dx-test: D3D12 group 18/18 on Venus (FL 12_1, SM 6.0, tier 3, triangle, compute, copy queues, bundles,
        ExecuteIndirect, descriptor heaps)
  - [x] The driver moved into the vkd3d-proton tree (`libs/viogpu_umd`, Meson option `enable_viogpu_umd`;
        sync-sources.sh pins vkd3d-proton v3.0.1 with submodules, build-driver.ps1 builds viogpu_d3d12.dll; the Mesa
        target of C1 is gone): umd_ddi.cpp speaks the DDI with the SDK/WDK headers and calls vkd3d-proton's COM
        objects through the SDK interfaces, umd_bridge.c creates the vkd3d-proton device (no surfaces). Caps come
        from vkd3d-proton: D3D12CreateDevice succeeds at FL 12_1, SM 6.2 (vkd3d-proton patch: NVIDIA's denorm
        workaround also for Venus on an NVIDIA host), binding tier 3, heap tier 2. Runtime rules found: view
        instancing needs SM 6.1, native 16-bit ops SM 6.2 (D3D12Core `QueryShaderModel66AndLater`)
  - [x] DDI entries on vkd3d-proton (reference: docs/d3d12-ddi-mapping.md, 2026-10-15): umd_state.cpp (shaders
        get DXBC containers with made-up semantics: the runtime passes the bare SM4/5 token stream or DXIL program
        part and nameless signatures; states, root signatures, PSOs), umd_resource.cpp (heaps/resources, views,
        descriptor heaps, queries, command signatures), umd_command.cpp (pools, recorders, lists, queues with a
        kernel context each, fences). Patches vkd3d-proton 0001 (denorm workaround) and 0002 (the driver); kvm 0016
        also ships viogpu_d3d12-licenses.txt (vkd3d-proton is LGPL-2.1, statically linked). dx-test D3D12 group
        17/17 through the Windows runtime (FL 12_1, SM 6.2, tier 3); whole suite 531 passed, 0 failed
  - [x] Queues on the kernel context's timeline (2026-10-15): ExecuteCommandLists returns right away; the context
        signals a monitored fence that opens a vkd3d-proton fence the lists wait for (so the runtime's queue waits,
        cross-queue and CPU-signaled, come first), then waits for a fence signaled once the lists ran (so its
        signals follow the work). The runtime calls D3DKMT itself on the context (never pfnSignalFence; its thunks
        are ordinal delay imports), so each submission still waits for the one before on the GPU
  - [x] Venus waits (Mesa patch 0017): a sleep in the guest lasts ≥ 0.5 ms (the finest timer interrupt period,
        high-resolution waitable timers included), so every Venus fence/semaphore wait past vn_relax's few yields
        cost ~0.5 ms; vn_relax now yields for up to 0.5 ms first. Empty submit + wait: raw Vulkan 537 -> 70 us,
        stock vkd3d-proton 575 -> 94 us, our driver 648 -> 172 us; ordered submissions 600 -> 92 us each;
        presentprobe 1280x720 with 20 submissions per frame 324 fps (719 without). Helps every Vulkan app
  - [x] Overlapping submissions (2026-10-20, gpu.68: kvm 0023 crash fixes + 0024 markers, vkd3d-proton 0014).
        Measured: the same GPU work (40 4K copies) in 1/10/40 ExecuteCommandLists a frame: 348/192/67 fps, ~0.3 ms
        idle GPU per submission boundary (hops: kernel ~0.04 ms, the rest Venus: gate signal -> host start, host done
        -> guest sees it). Cause: each submission's kernel context waits for kmOut >= n (the runtime's own fence
        signals after it, unseen, must come once the lists ran), so submission n+1's gate opens only after n is done.
        Plan: a second engine (node 1) for D3D12 queue contexts; a submission is a marker DMA buffer there: the KMD
        opens its gate when dxgkrnl hands it over (after the runtime's waits before it; a semaphore the UMD's gate
        thread waits on) and completes the markers in order when the UMD reports their lists done (escape); other
        DMA buffers on node 1 (presents) wait for the markers before them. Several submissions in flight, runtime
        signals after a submission still come once it completed.
        Spike (2026-10-20, package 100.6.101.58478, not deployed): 40 copies in 1/10/40 submissions 399/357/150 fps
        (was 348/192/67), idle Present 5401 fps (was ~3500); dx-test D3D12 42/42; ~2.5 s submissions next to a D3D11
        renderer: no GPU timeout, D3D11 keeps its frame rate (2118 frames in 40 s, serialized path 2091).
        `VIOGPU_UMD_SERIALIZE=1` forces the old path. Found on the way:
        - dxgkrnl drops a zero-length SubmitCommandCb without handing it to the KMD and completes its fence: the
          marker carries a 16-byte dummy command (never read). Before that, no D3D12 work ran at all (benchmarks
          looked 5x faster: skipped work)
        - an application exiting with markers in flight: its gate/done threads die, Windows waits for the context's
          work before tearing the process down -> GPU timeout 10 s later. The registry keeps each queue's process; a
          queue whose process is exiting counts as done (PsGetProcessExitStatus), the Venus worker polls every 100 ms
          while markers are in flight
        - long lists aren't stacked: once the oldest marker in flight has run 20 ms the next waits for it (the host GPU
          runs them in order with D3D11/DWM)
        - DisableGpuTimeout on the context and PreemptionCaps (DMA buffer / dispatch boundary) changed nothing:
          reverted
        - two crashes the stress tests hit, both old code: the capset/EDID/display-info queries gave up after 1 s
          while the host was busy (BSOD 0x3B on an uninitialized vbuf; the late reply then wrote into a dead stack
          event) -> wait for the reply like QueueAndWait does; GetCTA861Data stepped `m_EDIDs + 128` rows of a
          BYTE[1][256] array, 32 KB past it (BSOD 0x50 at driver start when unmapped) -> `m_EDIDs[0] + 128`.
          Minidumps read on the host with deploy/tools/triage.py (driver list at TRIAGE_DUMP64 +
          0x30, 0x90-byte entries, DllBase at +0x38) and llvm-symbolizer against the build's PDB
        - tools: deploy/tools/src/kmlog.exe prints the KMD's VioGpuLog ring (escape VIOGPU_GET_LOG);
          VIOGPU_UMD_STDERR=1 copies the UMD's log to stderr
        10-minute soak (1080p, 40 copies in 10 submissions a frame, a D3D11 renderer alongside): 312.8 fps, D3D11
        59 fps throughout, no timeout or error. Full regression 74/74 as gpu.67, dx-test 642/0.
        User VM gpu.68 (driver 100.6.101.58478): dx-test 640/0; D3D12 3992/4082/3547 fps at 720p/1080p/1440p (gpu.67
        3534/3485/3069), BGRA 2203/2066/2160 (1794/1616/1711), split 1/10/40 302/256/106 fps; D3D11 1469/1489/1266
        there (gpu.67 1955/2044/1780) was the host's load: on the spike, same session, gpu.67 vs gpu.68 D3D11
        1470 vs 1418 fps (3 runs each, noise), D3D12 2634 vs 4221
  - [x] Present (2026-10-15): swap chain buffers (committed textures on a "primary" heap) also get a kernel
        allocation, a virgl texture flagged as a GDI surface (the KMD uploads it before every use; DWM opens it like a
        D3D10/11 surface); the command list's pfnPresent copies the image there (GPU copy to a readback buffer, CPU
        into the locked allocation) and reports it with the queue's kernel context. Found with a D3DKMT_PRESENT
        dump compared against a D3D11 flip-model present: BroadcastContextCount must be 0 (the queue's own context
        in BroadcastContext[] gets STATUS_INVALID_PARAMETER). dx-test D3D12 group has a present test now: 18/18
  - [x] 32-bit (2026-10-15): viogpu_d3d12_x86.dll (build-driver.ps1 builds vkd3d-proton per architecture with the
        Vulkan step's clang-cl machine files; kvm 0016 names it fourth in UserModeDriverNameWow). The logging stubs
        take each entry's own type (gen_tables.py emits an X-macro of the table members; `decltype`), so __stdcall
        pops the arguments. A changed UserModeDriverName* takes an adapter restart (dxgkrnl reads it at start).
        dx-test group `d3d12-32` 18/18; whole suite 550 passed, 0 failed (package 100.6.101.58575)
  - [x] Present cost (2026-10-20, gpu.65, image 6.05-19): zero-copy swap chain buffers - vkd3d-proton 0012,
        mesa-venus 0007, kvm-guest-drivers-windows 0022, virglrenderer 0012. Spike D3D12 clear + Present: 2522/2541/2265
        fps at 720p/1080p/1440p (was 526/290/184), Present 0.35 ms at every size; user VM 3212/3139/2568 fps (was
        600/337/213; D3D11 2312/2518/2170), dx-test 638/0. BGRA8 buffers too since gpu.66 (image
        6.05-20): GL keeps BGRA as RGBA, so a BGRA buffer stays vkd3d-proton's own and Present blits it into its shared
        RGBA image on the GPU (vkd3d-proton interop on the present queue): spike 1408/1100/1316 fps, user VM
        1087/972/1028 (was ~600/337/213); an import the host refuses is now an error the driver falls back from (vkr
        no longer marks the context fatal; Venus waits for such allocations), not a crash; since gpu.67 (vkd3d-proton 0013)
        Present no longer waits on the CPU (the kernel context already orders the present after each submission's
        kmOut wait; a BGRA conversion is a submission of the queue's own): a frame of 5 ms CPU + ~5 ms GPU went from
        102 to 168 fps on the spike (CPU and GPU overlap), user VM 191 fps (D3D11 163), plain present 3534/3485/3069
        fps (RGBA) and 1794/1616/1711 (BGRA) at 720p/1080p/1440p; the shared allocation keeps unused guest pages (the
        runtime refused size 0). Found on the way: Venus runs in virglrenderer's render server, which takes resources
        only as dma-bufs (NVIDIA exports an optimal image's memory as one though it advertises import only - the fallback
        covers a driver that won't), and an attach reaches it through the socket while the ring doesn't: a ring
        roundtrip (a submission) orders them, as Linux Venus does for dma-buf imports
        Measured 2026-10-20 (spike, tools/src/presentbench.cpp, clear + Present at sync interval 0): D3D11 1484/1378/1803
        fps at 720p/1080p/1440p (Present 0.4 ms), D3D12 526/290/184 fps (Present 1.8/3.4/5.3 ms). At 1440p a present
        is: wait for the queue 0.9 ms, GPU copy to the readback buffer 2.2 ms, CPU copy into the allocation 1.7 ms (and
        the KMD's upload per composition on top). How it went (host interop first proven by a probe: an optimal-tiled
        Vulkan image's memory shared by a second VkDevice and by GL as texture storage, bit-exact both ways on the 3080):
        1. virglrenderer 0012: textures with VIRGL_BIND_VK_SHARED get their storage from a vrend_vk image (exportable,
           dedicated, fixed parameters), GL imports its opaque fd, the resource exports it as a dma-buf for Venus
        2. kvm 0022: VIOGPU_RES_ATTACH attaches such a resource (only those) to the calling device's context
        3. mesa-venus 0007: an app's VkImportMemoryResourceInfoMESA attaches the resource first, then a roundtrip
        4. vkd3d-proton 0012: swap chain buffers as Venus images of the host's parameters on that memory, wrapped by
           CreateResourceFromBorrowedHandle; Present only waits for the queue
  - [x] Venus: report the adapter's LUID (vkd3d-proton couldn't match the DXGI adapter and fell back): mesa-venus 0001
- [~] C3. Newer D3D12 (asked 2026-10-15, after gpu.41): shader model 6.6 and up as far as vkd3d-proton and the
      host allow (our DDI 0052 caps it at 6.2; the DDI names 6.3 with 0054, 6.5 with 0071, 6.6 with 0082, 6.8 with
      0108), double-precision shaders (vkd3d-proton's NVIDIA denorm check looks at the driver ID, Venus has its own)
  - [x] Double precision: vkd3d-proton patch 0001 also treats Venus on an NVIDIA host as NVIDIA for FP64 denorms
  - [x] Venus from Mesa 26.2.4 (2026-10-16): the Mesa of the other drivers (Aug 2024) lacks the Venus extensions
        for SM 6.6+ (compute shader derivatives, maximal reconvergence, quad control), ray tracing and mesh shaders;
        the host's virglrenderer 1.3.0 has them all. Venus is now built from a tree of its own, `mesa-venus`
        (sync-sources.sh, build-driver.ps1's Vulkan step, DirectX-Headers 1.619.1), patches/mesa-venus 0001-0003
        (the WDDM renderer ported, Win32 present thread, Windows waits; Mesa's own Windows groundwork made the
        port smaller). Vulkan 1.4, 164 extensions, LUID reported (vkd3d-proton matches the DXGI adapter now).
        Old Mesa patches 0014/0015/0017 are no longer built (cleanup: drop them, rebase 0016's header context)
  - [x] Found on the way: (1) Venus 26 signals every VkFence through a renderer sync on the queue's ring, which
        retired only when virgl's GL fences woke vhost-user-gpu (an idle desktop: never; else ~10 ms): QEMU patch
        0009 polls each Venus context's own fence eventfd. (2) virglrenderer's queue-sync VkFences were SYNC_FD
        exportable, and NVIDIA (615.78) signals those up to 10 ms late (reproduced natively on the host):
        virglrenderer patch 0005 makes them plain (nothing exports them). Image 6.05-9. (3) The WDDM renderer's
        syncs held one pending submission and didn't wait for unsubmitted values (Venus' present thread waits
        first): real timelines now. Results: copy + fence 10 ms -> 0.4 ms, empty fence 0.06 ms, vkwin 1200x781
        IMMEDIATE 947 fps, FIFO 59.4 fps
  - [x] DDI 0082 (shader model 6.6): tables of 0080/0074 (viogpu_device_funcs / viogpu_command_list_funcs
        typedefs), PSO and resource arguments of 0075, shader caps 0082 (64-bit atomics), options 0081; 0052
        still offered to older runtimes. (Feature level 12_2: see below.)
        Spike: D3D12 SM 6.6, doubles, 64-bit atomics; dx-test Vulkan + D3D12 groups 80/0
  - [x] Shader models 6.7/6.8 (2026-10-18, gpu.58, vkd3d-proton patch 0008): DDI 0100 (0082 kept for older
        runtimes), the changed arguments by the build the runtime chose: resources with an initial barrier layout
        (0088), per-face stencil masks (0095), samplers with integer border colors/non-normalized coordinates
        (0096, CreateSampler2), float depth bias and PSO flags (0099: dynamic depth bias, strip cut value), root
        signatures 1.2 (0100); graphics PSOs are one stream now (view instancing and the depth bounds test were
        dropped by the plain desc before); OMSetFrontAndBackStencilRef/RSSetDepthBias/IASetIndexBufferStripCutValue.
        Video: a DDI 0100 runtime only enables video feature 0094_0 (0083_0 silently not: D3D12 encoding broke
        until then; the regression's venc cases caught it), its video list tables end in pfnBarrier.
        dx-test: SV_StartVertexLocation/SV_StartInstanceLocation in a vs_6_8 (DXC v1.8.2505.1 for the 6.8 shaders;
        the EWDK's stops at 6.7). Not DDI 0102 on: the runtime then wants sampler heaps of >= 4096 and UAV-only
        rendering at 1-16 samples, vkd3d-proton's shader-visible sampler heaps stop at 2048 (found by sweeping the
        OPTIONS_0102 caps: the runtime says nothing)
  - [x] DDI 0102 (2026-10-18, gpu.61, vkd3d-proton patch 0009): vkd3d-proton's shader-visible sampler heaps of 4096
        where descriptor sets take them (only the pipeline layouts' sampler arrays grow; descriptor buffers keep 2048),
        SupportedSampleCountsWithNoOutputs from framebufferNoAttachmentsSampleCounts (a draw without render targets
        already takes the pipeline's sample count); the UMD offers 0102 only where the device has both (0100/0082
        otherwise), OPTIONS_0102 from OPTIONS19, the rasterizer's own line mode from 0102. dx-test: a sampler in slot
        4095 of a 4096-sampler heap (its border color), a UAV-only draw at 4 samples (SV_Coverage 0xf), 64/32-bit.
        Spike 634/0 (regression unchanged), user VM 632/0
  - [x] Enhanced barriers and relaxed format casting (2026-10-18, gpu.62, vkd3d-proton patch 0010): the runtime enables
        enhanced barriers only with relaxed format casting (found by reporting it), then sends every barrier through
        pfnBarrier, legacy ones too. pfnBarrier (0094) into ID3D12GraphicsCommandList7::Barrier, one group per type, the
        runtime's legacy layouts mapped, its ranged barriers a full global barrier; the video lists' pfnBarrier does
        nothing (as their ResourceBarrier); castable format lists through ID3D12Device10 (initial layout) and Device12's
        allocation info. dx-test: a texture made in UNORDERED_ACCESS with texture and buffer barriers to copies; an
        R32_UINT texture written through a castable R8G8B8A8_UNORM view. Spike 638/0 (regression unchanged), user VM 636/0.
        Found on the way: the spike's clock ran 7 hours ahead after each restart (the RTC is UTC, Windows read it as
        Pacific time), so builds left future-dated files that meson later refused and ninja skipped: RealTimeIsUniversal
        set on the spike
  - [x] DDI 0110 (2026-10-20, gpu.63, vkd3d-proton patch 0011): offered where 0102 was (same conditions); the tables
        of 0109/0108 (work graph entries stay stubs), 0103 work graphs and wave matrix ops off, 0106's generic-device
        caps on, 0109 resource descs/CreateDevice only grow (no RecreateAt tier), 0110's ExecuteIndirect tier and
        SM 6.8 options from vkd3d-proton's OPTIONS21. dx-test: SampleCmpGrad in a cs_6_8 (sm68cmp.hlsl) and the
        ExecuteIndirect tier, both optional - on Venus tier 1.0 and no SampleCmpGrad/Bias (below). Spike 638/0,
        regression all ok, user VM 636/0
  - [x] Work graphs (2026-10-20, gpu.69, vkd3d-proton patch 0015): WorkGraphsTier 1.0 (0103 caps from vkd3d-proton's
        OPTIONS21; its experimental-features gate dropped - Venus has maximal reconvergence, the memory model and
        wave32). DDI 0108: the resolved work graph subobject becomes the API's (all available nodes, the entry points
        in the runtime's order, which answers the app's entry point indices itself), only work graph/library/root
        signature subobjects passed on (vkd3d-proton refuses others); GetProgramIdentifier, GetWorkGraphMemory-
        Requirements, SetProgram, DispatchGraph as they are. ponytail: node overrides (renames, shared inputs, local
        root argument indices, dispatch grids set outside the shader) and per-export root signatures are lost -
        vkd3d-proton takes neither. dx-test: a broadcasting node feeding a thread node plus a second entry point, CPU
        records (dxil/wg.hlsl, lib_6_8), 64/32-bit. Spike 644/0, regression identical to gpu.68; deployed as gpu.69
        (driver 100.6.101.58769, image 6.05-20): user VM 642/0
  - [x] VK_KHR_maintenance8 through Venus (2026-10-20, gpu.64, image 6.05-18; Mesa venus 0006, virglrenderer 0011):
        its features struct in the Venus protocol on both sides, generated-style (maintenance7's struct functions and
        pNext cases, extension 575), passed through; vkd3d-proton then reports SM 6.7's AdvancedTextureOps and SM 6.8's
        SampleCmpGrad/Bias (and takes its relaxed depth/color copies). dx-test's SampleCmpGrad passes 64/32-bit;
        spike 640/0, regression identical to gpu.63, user VM 638/0
  - [x] ExecuteIndirect tier 1.1 and state-changing ExecuteIndirect (2026-10-20, gpu.70, image 6.05-21; mesa-venus
        0008, virglrenderer 0013): VK_EXT_device_generated_commands through Venus. Found on the way: without it
        vkd3d-proton accepted every ExecuteIndirect that changes state per command (root constants, vertex/index
        buffers) and recorded nothing. The protocol from the venus-protocol generator (src-video clone, branches
        video-guest/video-host: maintenance8 - identical to the hand-written 0006/0011 - and the extension added,
        commands 1100-1108; dispatch/draw tokens' union data selects no member, so the tag alone goes on the wire).
        Guest: indirect commands layouts, memory requirements, preprocess/execute; no indirect execution sets (no
        stage reported bindable). Renderer: layouts as objects (vkr_device_object.json), dispatch. vkd3d-proton
        preprocesses in another command buffer with the list's as state: Venus batches each command buffer, so
        the host recorded the preprocessing against a stale state and the GPU hung (Xid 109 CTX SWITCH TIMEOUT,
        surfacing as device lost -> "vkQueueSubmit2 resulted in CS error"); the state command buffer's commands now
        go out first and the preprocessing right after. dgctest (deploy/tools/src/dgctest: the same plain-Vulkan
        DGC test on the host and in the guest) told Venus from vkd3d-proton. dx-test: compute EI with root
        constants + count buffer (incrementing constants at tier 1.1), graphics EI with vertex/index buffer and
        root constant changes. Spike 650/0, regression identical to gpu.69; deployed as gpu.70 (driver
        100.6.101.58857, image 6.05-21): user VM 648/0
  - [x] Feature level 12_2 (DirectX 12 Ultimate), vkd3d-proton patches 0006/0007, kvm-guest-drivers-windows 0021:
        VRS tier 2 (RSSetShadingRate[Image]); mesh/amplification shaders (OSG1/PSG1 containers, PSO streams,
        DispatchMesh); DXR 1.1 (umd_raytracing.cpp: state objects with the runtime's export summary turned into
        explicit associations, AddToStateObject, acceleration structures, SetPipelineState1, DispatchRays); sampler
        feedback tier 0.9 (feedback maps with their mip region, feedback UAVs). What else 12_2 took:
        - programmable sample positions tier 1 in vkd3d-proton (VK_EXT_sample_locations, a pipeline variant while
          positions are set): Microsoft's runtime allows ResolveSubresourceRegion - so sampler feedback's decode -
          only with them
        - WriteBufferImmediate's queue flags (it worked, but was reported unsupported)
        - 40-bit process GPU VA: the KMD's page tables went from 3 to 4 levels (39 -> 48 bits); with 39 the runtime
          refused the 12_2 device (DXGI_ERROR_DRIVER_INTERNAL_ERROR, nothing in the debug layer)
        - MSAA, broken in D3D12 until now: CheckMultisampleQualityLevels said 0 above 1 sample. Real levels need
          matching format support bits, and none for formats that only view multisampled ones (the runtime refuses
          the device otherwise; typeless formats keep their family's)
        dx-test D3D12: DXR (hit + miss + local root constant), sampler feedback decode, sample positions on 4x MSAA,
        WriteBufferImmediate, mesh/VRS - spike 34/0, feature level 12_2. dx-test has DXTEST_D3D12_DEBUG=1 (debug
        layer messages in failing tests)
- [x] Design: the D3D12 DDI (d3d12umddi.h) the runtime calls, mapped onto vkd3d-proton's Vulkan implementation;
      DXIL → SPIR-V with dxil-spirv; root signatures, descriptor heaps, command lists/queues, fences, resources,
      PSOs, present through DXGI
- [x] Adapter and device creation (feature level 11_0+ reported to `D3D12CreateDevice`), queues, fences
- [x] Resources and heaps, descriptor heaps, copies
- [x] Root signatures, pipeline states, DXIL shaders; draws and dispatches
- [x] Present (DXGI swap chains with D3D12)
- [x] Bundles, ExecuteIndirect, queries, further features as tests demand
- [x] dx-test D3D12 group passing (it exists: device, queues, root signatures/PSOs, triangle, compute, copy queue,
      bundles, ExecuteIndirect, descriptor heaps)
- [x] Tests: spike and WinBoat VM
      (The list above was done with C2/C3 and left unticked; checked 2026-10-17: dx-test's Direct3D 12 groups pass on
      the spike and in the WinBoat VM, DirectML and D3D12 video run on it)

## E. GPU compute and video for applications (ffmpeg, Topaz Video AI, players, browsers)

Asked for on 2026-10-14: applications like Topaz Video AI and ffmpeg must be able to use the GPU through the
driver. What they call, and what a virtual GPU can offer:

- Topaz Video AI: CUDA/TensorRT on NVIDIA's own driver; on any other DirectX 12 GPU, DirectML (D3D12 compute,
  ONNX Runtime). Encoding through NVENC/QSV/AMF, else the CPU.
- ffmpeg on Windows: decoding through `dxva2`, `d3d11va`, `d3d12va`, `vulkan` (Vulkan Video); encoding through
  `*_nvenc`/`*_qsv`/`*_amf` (vendor drivers), `*_mf` (Media Foundation encoders), `*_vulkan` and `*_d3d12va`
  (newer ffmpeg); filters through Vulkan (`libplacebo`, `scale_vulkan`) and OpenCL.
- Not through the graphics driver: CUDA, NVENC/NVDEC, TensorRT. They are NVIDIA's own closed libraries (`nvcuda.dll`,
  `nvEncodeAPI64.dll`, TensorRT on top of CUDA); open drivers (nouveau/NVK, NVIDIA's open kernel modules) don't
  provide them. Applications fall back to the routes below; Topaz to DirectML. Forwarding the CUDA/NVENC calls
  themselves to the host is a separate project: stage F.

- [x] Vulkan compute in the guest: ffmpeg's Vulkan filters and libplacebo (2026-10-17, gpu.55; mesa-venus 0005).
      They all hung on uploaded frames: ffmpeg's hwupload fills images with VK_EXT_host_image_copy, and a 3-plane
      yuv420p image filled that way hung the host GPU at its first queue use (NVIDIA Xid 109, context switch
      timeout); GPU-decoded frames, 2-plane NV12 (whose host-transfer bit the YCbCr mask already dropped) and
      single-plane images worked. Venus now offers host image copies for single-plane formats only (applications
      upload multi-planar ones through a buffer); the multi-planar host copy fault itself isn't found. Spike
      (deploy/tools/vkfilters-test.sh): hflip/vflip/transpose exact, scale 66.6 dB, gblur 42.4, avgblur 58.6, bwdif
      byte for byte the host driver's own output, nlmeans runs, libplacebo 42.3 dB. chromaber_vulkan crashes
      ffmpeg on the host's own driver too (not ours). dx-test gained "Timeline semaphores (signalled from the CPU)"
      and DXTEST_VK_FAMILY (run the Vulkan group on another queue family)
- [x] DirectML on the D3D12 driver (2026-10-16, spike): inbox DirectML 1.15.5 creates its device on ours (DML
      feature level 6.2); GEMM 64x64 FP32 and FP16, convolution FP32 match the CPU (2e-7 / 1.4e-3). dx-test group
      `directml` (its own declarations of the DirectML API: MinGW has no DirectML.h). Topaz Video AI's route on
      non-NVIDIA GPUs is DirectML (ONNX Runtime)
- [x] OpenCL (2026-10-16, spike): Microsoft's "OpenCL, OpenGL and Vulkan Compatibility Pack" (Store, winget id
      9NQPSL29BFFF) runs OpenCL 3.0 on our D3D12 driver (OpenCLOn12). Two D3D12 driver bugs found on the way:
      (1) the pack also installs Dozen (Vulkan on D3D12) as a Vulkan driver; vkd3d-proton's vkCreateInstance went
      through the loader, loaded Dozen inside D3D12CreateDevice and deadlocked on the runtime's lock - the D3D12
      driver now loads viogpu_vk.dll directly (no other drivers or layers in D3D12 programs); (2) creating a
      pipeline library failed, which the runtime takes as a driver error and removes the device (OpenCLOn12, and
      every engine with a PSO cache) - pass-through libraries now. dx-test group `opencl` (saxpy over 1M floats)
      and a pipeline library test in the D3D12 group. Windows 10 (2026-10-24): the pack alone isn't enough there -
      Windows 10 has no OpenCL.dll (Windows 11 has the ICD loader); Khronos' loader (OpenCL-SDK release, which finds the
      pack's OpenCLOn12 by its package family) and the VC++ 2015-2022 runtime it needs make it work: 3/3. Shipped since
      gpu.87: tools/opencl-loader builds the loader with MinGW (static runtime: only Windows 10's own UCRT), the
      driver payload carries it (drivers/opencl) and gpu-driver.ps1 puts it in System32/SysWOW64 where Windows has no
      OpenCL.dll (never over Windows 11's or a vendor's; removed on uninstall only if it put it there)
- [x] Video decoding on the host GPU (2026-10-16, spike, image 6.05-10): virgl's gallium video (host VA-API,
      nvidia-vaapi-driver on NVDEC; neither Mesa 26's nor virglrenderer 1.3.0's Venus protocol has Vulkan Video).
      Host fixes found by decoding for real (none of it had ever worked on NVIDIA):
      - virglrenderer 0007: slice parameters per NAL unit (virgl sent one with slice_data_size 0: NVIDIA's driver
        decoded nothing), the DRM modifier kept and passed to EGL, H.264 picture size and invalid references, HEVC
        scaling lists, one RT format per config (VP9 profile 2 decoded as 8-bit), and with NVIDIA's driver decoded
        pictures read with vaGetImage instead of the DMA-BUF/EGL import: NVIDIA's EGL reads nvidia-vaapi-driver's
        block-linear exports wrongly where pictures have detail (reproduced natively on the host, without a VM)
      - nvidia-vaapi-driver 0001 (planes of the decoded frame at the decoder's height: surfaces padded beyond the
        picture, as d3d11va's HEVC ones, came out without chroma), 0002 (VP9 loop filter deltas carry over from
        frame to frame: inter frames were deblocked wrong), built with gstreamer's codec parsers (no VP9 without)
- [x] D3D11 video decoding DDI (2026-10-16, spike, mesa 0019/0020): H.264, HEVC Main/Main 10, VP9 profiles 0/2
      (listed only as far as the host decodes them), NV12/P010 textures (plane views, maps, uploads, copies), the
      video processor (vl_compositor: color conversion, scaling). ffmpeg's d3d11va bit-exact against libavcodec for
      every test clip (H.264 High with B-frames and slices, 1080p; HEVC Main, with scaling lists, Main 10, 4K; VP9
      8/10-bit); 1080p H.264 at 225 fps, 4K HEVC at 112 fps with almost no guest CPU. dx-test `video`: a key frame
      per codec through the DXVA API (ffmpeg's buffers, the software decoder's hash), the video processor, plane
      views
- [x] AV1 and MPEG-2 (2026-10-16, spike, image 6.05-11): AV1 Main 8/10-bit (tiles from the DXVA tile control
      buffers) bit-exact against libdav1d for every clip incl. 1080p with tiles, ~260 fps at 1080p; MPEG-2 Main
      identical to native NVDEC on the host (720x576, 1280x720, custom quantizer matrices; MPEG-2's IDCT isn't
      bit-exact by spec, so compared with NVDEC, not libavcodec). Host fixes: virglrenderer 0007 (AV1 current
      frame, chroma subsampling, tile sizes; MPEG-2 quantizer matrices, picture size, slice headers parsed for
      VA-API's slice parameters), nvidia-vaapi-driver 0003 (AV1 reference sizes are the decoded size, not the
      padded surfaces'). The device and video contexts wait on each other's fences (Windows schedules each
      context by itself: an AV1 frame now and then read a reference before it was decoded). The GPU check has no
      MPEG-2 sample (its hash would be the host decoder's, vendor-specific)
- [x] D3D12 video decoding (`d3d12va`, 2026-10-17, spike, vkd3d-proton 0004): H.264, HEVC Main/Main 10, VP9
      profiles 0/2, AV1 Main 8/10-bit on Vulkan Video's decode queue (NVDEC), the encoder's way (interop lists on the
      decode family). ffmpeg -hwaccel d3d12va bit-exact against libavcodec for 11 of the 12 test clips
      (`deploy/tools/vdec-test.sh d3d12va`; vulkan and d3d11va as well), 1080p H.264 ~10x with the frames read back;
      decode and encode chain on the GPU (h264 d3d12va -> hevc_d3d12va, 720p 9.8x, 47.6 dB). How: the decoder keeps
      its own DPB (an image per slot, DPB and output coinciding) and copies each picture into the application's
      texture; parameter sets rebuilt from DXVA's picture parameters; AV1's reference state (order hints, sign biases,
      skip mode frames) derived per slot. Findings on the way:
      - the runtime checks its decode formats report decoder output support (CacheVideoDecodeFormatCaps throws
        otherwise: found with cdb); NV12/P010 report it
      - vkd3d-proton's initial layout transition of a texture comes with its first D3D12 use: an output texture
        first written by interop Vulkan commands lost its luma to it; the interop list now tracks the texture
      - ffmpeg sizes AV1 bitstream buffers to the byte, decoders read 256-byte aligned: decode buffers' Vulkan
        buffers are padded to 4 KiB
      - AV1: DXVA's tile starts are superblocks, Vulkan's MiColStarts 4x4 units; an unset AV1 color config means
        identity matrix coefficients (now unspecified)
      - (vkd3d-proton 0005) one multi-tile 8-bit AV1 clip had off-by-one U samples: NVIDIA's decoder takes a sequence
        header from a session parameters object differently from the same header passed inline
        (VK_KHR_video_maintenance2, as ffmpeg's Vulkan decoder does): AV1 sessions now pass it inline. Found by
        dumping every Vulkan input of both ffmpeg routes in the guest's Venus (parameters, tile layout, bitstream
        repacked, images, session, refresh flags: none mattered) until only the parameters object was left
      - (0005) AV1 film grain: a picture that applies grain decodes out of place into an extra image, which is
        copied to the application's texture; its DPB picture stays without grain (ffmpeg's way on NVIDIA's
        coinciding DPB/output). Bit-exact 8/10-bit on a strong-grain clip (43 dB between grained and not)
      - (0005) interlaced H.264: the interleaved layout for streams that aren't frames-only, both fields of a frame
        in one slot, field flags on the setup and field-coded references. Bit-exact on the conformance clips CVPA1,
        CVFI1 (field pictures), CVMA1, CAMA1 (MBAFF), CAPAMA3 (mixed)
      - ffmpeg's hevc_d3d12va encodes at 16-aligned heights: with an 8x8 minimum CU in our caps, 1080p frames
        straight from the decoder (1088-line references) failed the runtime's size check; caps now report 16x16
        (0005): h264/hevc 1080p and 4K d3d12va -> hevc/h264_d3d12va work (1080p 49.8 dB)
      Not offered: decode-time conversion. Deployed 2026-10-17 as gpu.48 (driver 100.6.101.58619, image 6.05-13):
      user VM dx-test 586/0, HEVC Main/Main 10 d3d12va 90/90 frames identical to software with Topaz's ffmpeg.
      0005 (all 19 test clips bit-exact on d3d12va and vulkan) in gpu.49
- [x] D3D11 decoding with the frames read back to the CPU ran at ~1x: the kernel's EscapeResourceBusy slept a
      clock tick (up to 15.6 ms) per check even when the resource was idle; it now yields only while still busy
      (kvm-guest-drivers-windows 0019). d3d11va with readback 4-8x (2026-10-17)
- [x] D3D11 AV1 film grain (2026-10-17, spike): the D3D11 route (Mesa -> virglrenderer -> VA-API ->
      nvidia-vaapi-driver -> NVDEC) dropped the grain: NVDEC writes the grained picture to CurrPicIdx and the
      grain-free reference to decodePicIdx, and the driver set both to the render target. nvidia-vaapi-driver 0004
      (image 6.05-15) keeps the grain-free picture on a decode surface of its own while a reference slot holds it
      (an AV1 decoder takes NVDEC's maximum of 32; at most 9 are in use as references), so nothing changes in
      virglrenderer, whose current_display_picture isn't a surface. With grain applied it was still wrong: Mesa cast
      DXVA's ar_coeffs_*_plus_128 to int8_t instead of subtracting 128 (mesa 0022). d3d11va av1-grain 8/10-bit
      bit-exact
- [x] DXVA2 decoding in the D3D9 driver (`dxva2`, 2026-10-17, gpu.52; mesa 0023): the Direct3D 11 driver's DXVA
      decoding moved into d3d10umd/DxvaDecode.{h,cpp}, which both drivers build; d3d9umd/decode.cpp has the decoder
      caps (NV12: H.264, HEVC Main, VP9 profile 0, AV1, MPEG-2), the compressed buffers and the Decode* entry points.
      The codec writes R8/R8G8 planes per surface (the host's reference); a draw on the video context copies each
      picture into the surface's L8 texture (luma rows, then chroma rows) that locks, StretchRect and VideoProcessBlt
      read; DXVA indices name the surface last decoded with that index. ffmpeg -hwaccel dxva2 (pix_fmt dxva2_vld):
      H.264 incl. the field/MBAFF conformance clips, HEVC to 4K, VP9, AV1 with film grain, bit-exact; 10-bit falls
      back to software (no P010 in D3D9). D3D11 19/19 unchanged; dx-test 590/0 (5 DXVA2 profiles, 64/32-bit)
- [x] Bob deinterlacing in the D3D11 video processor (2026-10-17, gpu.53; mesa 0024): offered in the rate conversion
      caps, the field order from VideoProcessorSetStreamFrameFormat; the field the output index names is copied (every
      other row of both planes) into a half-height picture on the video context, which the compositor scales.
      dx-test "Video processor (bob deinterlacing)": a frame with a light top and dark bottom field gives 214/214 and
      40/40 (no combing); 591/0. Found on the way: the spike's and the guest installer's driver updates removed the
      old package (with /uninstall) before adding the new one, so a GPU still waiting for its restart could stay on
      the 2D VioGpuDod driver; both now add the new package first (and a re-install skips signing what's signed)
- [x] Motion-adaptive deinterlacing in the D3D11 video processor (2026-10-18, gpu.60; mesa 0026): the rate conversion
      caps offer adaptive with one past and one future frame (bob when they're missing). A full-height picture: the
      field's rows kept, each missing row the mean of the opposite fields just before and after it where they match,
      else the rows above and below (motion: those fields' difference and the field's own rows between the frames;
      vl_deint_filter's 6/255 and 14/255 thresholds). vl_deint_filter isn't used: in frame mode it weaves from the
      frame before. dx-test "Video processor (motion-adaptive deinterlacing)": the same frames around it keep the
      alternating rows (214/40, woven), grey frames around it give bob's 214/214 and 40/40. Found on the way: virgl's
      TGSI to GLSL drops ranged sampler declarations (DCL SAMP[0..3]: "undefined variable fssamp0"), declare each.
      Spike 630/0 (regression unchanged), user VM 628/0
- [x] VC-1 decoding (2026-10-21, gpu.71, image 6.05-22; mesa 0027, virglrenderer 0014, nvidia-vaapi-driver 0005):
      not a software decoder's job after all - NVDEC decodes VC-1 and nvidia-vaapi-driver exposes it. DXVA VC1_D2010
      and VC1_D (simple, main, advanced; WMV9) in the D3D11 and D3D9 drivers: DXVA's MPEG-2-era picture parameters
      unpacked as ffmpeg's dxva2_vc1.c packs them, VA-API's picture types (PTYPE/FPTYPE), the advanced profile's real
      start codes put back per slice. Every layer had a gap: virgl never sent the frame coding mode or POSTPROCFLAG
      and the host gave VA-API one empty slice parameter (fields appended to the VC-1 picture description: profile,
      field order and parity, RANGEREDFRM); nvidia-vaapi-driver dropped the start codes NVDEC needs and the
      codec-specific intra/reference and pan-scan flags, and a field pair's first-field resolve raced with the
      second field (a sync returned half a frame; now BeginPicture waits for it, bounded - any codec). Tested
      natively on the host first (ffmpeg's VA-API vs its cuda path vs libavcodec), then in the guest. ffmpeg
      -hwaccel d3d11va/dxva2: the FATE suite's VC-1 clips bit-exact (simple/main, advanced progressive, interlaced
      with field pairs, 1080i; SMM0005 ends in a damaged packet where NVDEC itself differs), 2-14x; regress.sh
      runs them. Deployed as gpu.71 (driver 100.6.101.58945, image 6.05-22): user VM 648/0, VC-1 identical to
      software through d3d11va and dxva2 (deploy/uservm-vc1.ps1, Topaz's ffmpeg). dx-test: a group's child process
      sent stderr into the result pipe, and driver lines from other threads now and then split a result line
      (one test vanished from a report); stderr now goes to NUL
- [ ] Intermittent: dx-test's 64-bit OpenGL group crashed once (0xC000041D, an exception in a user callback: a
      window procedure) after all its tests passed, in the WinBoat VM on gpu.55; 3 reruns clean, never on the spike.
      2026-10-22 on the spike (package 58819) in 2 of 2 whole-suite runs, never with `--only opengl` (3 runs) nor in a
      whole-suite run under cdb (-o: children too, first-chance AVs logged: none). The group doesn't present (FBO and
      compute only): its teardown (wglDeleteContext/DestroyWindow) is where to look
      Watch for it; next time run the group under cdb (-g -G, first-chance exceptions) for the faulting module AMD/Intel hosts: Mesa's VA drivers need per-slice HEVC reference lists
      and VP9 segment values derived (not in DXVA's short format: ponytail notes in Video.cpp); Intel's iHD driver
      needs slice headers parsed
- [ ] Video encoding on the host GPU (NVENC behind it): Vulkan Video encode through Venus (`h264_vulkan`,
      `hevc_vulkan`), D3D12 video encoding (`*_d3d12va`), a Media Foundation encoder (MFT) for `*_mf` and Windows apps
- [x] Media Foundation hardware encoders (2026-10-17, gpu.51): Windows had none on our GPU (MFTEnumEx: Microsoft's
      software H.264/HEVC only), so Windows' capture/recording and `*_mf` fell back to the CPU. tools/mf-encoder:
      winboat_mft64.dll, asynchronous hardware H.264 and HEVC MFTs (NV12/IYUV, system memory or D3D11 textures;
      ICodecAPI rate control CBR/VBR/quality/QP, GOP, forced key frames; parameter sets on the output type) on the
      AMF runtime's Vulkan Video encoder; the INF copies it to System32 and writes its COM/MF registration as AddReg
      (kvm 0020; InfVerif refuses RegisterDlls). Spike: the Sink Writer with a D3D11 device manager picks it and feeds
      it D3D11 textures (H.264 59.4 dB, HEVC 59.6 dB); ffmpeg h264_mf/hevc_mf -hw_encoding 1: VBR/CBR/quality/QP,
      odd sizes, 48.6-58.6 dB. Not done: B-pictures, 10-bit (P010), AV1. AppRecordingManager.GetStatus (Game Bar's
      API) fails with "element not found" in the spike (no Game Bar there)
- [x] OpenCL: through the compatibility pack (above); the check app (stage T) points to it when OpenCL is missing
- [x] Topaz Video AI 7.1.5 (trial, 2026-10-16): it picks its AI backend by DXGI's dedicated video memory; with 0
      (our aperture-only segments) it called the GPU unusable and ran OpenVINO on the CPU. The kernel driver now
      reports a local memory segment over the host-visible window (4 GB in WinBoat; no allocation is placed there,
      and it must not be CPU-visible: Windows touching the unbacked window hung every D3D program), so Topaz uses
      ONNX Runtime + DirectML (`onnx16`) on our GPU: 2x upscale of a 320x240 clip in 10.7 s against 27 s on the CPU.
      Its export encoders (models/video-encoders.json): H.264/H.265/AV1 only through NVENC, AMF or QSV, offered by
      the GPU's name (nvidia/amd/intel); no Media Foundation or D3D12 entries. On our VirtIO GPU its UI offers CPU
      encoders only (ProRes, FFV1, VP9, image sequences), whatever the driver can do; its bundled ffmpeg does have
      hevc_d3d12va (works, below). An entry of our own in that file would offer it (Topaz may replace the file)
- [x] AMD AMF encoders on our GPU, Topaz unchanged (2026-10-17, gpu.49): an AMF runtime of our own
      (`tools/amf-runtime`: amfrt64.dll, AMF's API from AMD's MIT-licensed headers, shipped with that license; H.264
      and HEVC encoders on Vulkan Video: CQP/CBR/VBR, IDR period, profiles/levels, HEVC Main/Main 10/High tier,
      extradata) in System32 from the INF, and the adapter named "Red Hat VirtIO GPU 3D controller (AMD
      AMF-compatible)" (the INF's DeviceDesc; DriverDesc/FriendlyName don't reach DXGI) so Topaz's video-encoders.json
      ("amd|radeon") offers its AMF H.264/H.265 entries. Topaz's ffmpeg with Topaz's own AMF settings: H.264 CQP
      53 dB, CBR 60 dB; HEVC CQP 47.5, CBR 60, 4K High tier 47.9, odd 1278x718 57.5 (`deploy/tools/amf-test.sh`).
      Found on the way: a Venus generator bug (a NULL output pointer before a blob decoded in place lost its marker's
      room: vkGetEncodedVideoSessionParametersKHR returned the size in the header bytes; image 6.05-14) and NVIDIA's
      encoder ignoring bitrates without the codec's rate control layer info. User VM (gpu.49): Topaz's first start
      after the rename still showed its cached encoder list (HKCU\Software\Topaz Labs LLC\Topaz Video AI\
      exportEncoderList), from the second on the AMF entries; Proteus 2x on DirectML into Topaz's hevc_amf settings
      works. RGB input (BGRA, RGBA, ARGB, R10G10B10A2: ffmpeg's *_amf fed RGB without -pix_fmt) is converted to
      NV12/P010 on the CPU with the output color profile's matrix (unknown: BT.601 below 720 lines, else BT.709, as
      AMF's "auto by size"), studio range unless the profile says full: 1080p BGR0 46-49 dB, X2BGR10 into Main 10
      46.9 dB. Not RGBA_F16 (scRGB/HDR input needs a transfer function too)
- [x] GPU-timeout recovery (TDR, 2026-10-17, spike): the one-off hang in the user's VM was the host running out of
      memory (Topaz's pop-up, now bounded by the memory limit below), and it froze the VM for good because the INF set
      TdrDebugMode=1 and DxgkDdiResetFromTimeout was a stub. Now: the commander numbers its work by generation; a
      reset starts a new one, and everything older is abandoned (never sent on, never reported complete, its
      allocations no longer busy), late host replies included. The INF sets TdrDebugMode=2 (recover) and TdrDelay=10
      (host-side work can take seconds; gpu-driver.ps1 puts the previous value back on uninstall). Tested by freezing
      the spike's GPU process (SIGSTOP) under a rendering program: 15 s and 25 s freezes recover (one and two resets;
      the program gets DXGI_ERROR_DEVICE_RESET, makes a new device and renders on); a host stuck for about a minute
      ends in Windows' own limit (bugcheck 0x116 after 5 recoveries a minute) and a restart instead of a frozen VM.
      Not covered: Direct3D 12/Vulkan (Venus) work goes through driver escapes, not the scheduler, so a host hang
      there blocks the waiting program, not the desktop
- [x] GPU memory limit per VM (2026-10-16, spike): Topaz Video AI's upgrade pop-up creates a texture per animation
      frame and never releases them (seen with cdb in the app: 128 created, 0 destroyed); in the VM that took the
      host's memory until the kernel killed the GPU process. The kernel driver now charges every resource by its size
      (host-only textures included) against GpuMemoryLimitMB (driver key, default 8192) and answers out-of-memory
      over it, as a full graphics card does; the D3D10/11 driver reports that as E_OUTOFMEMORY (an upstream typo
      made it crash on any failed resource creation). Topaz keeps running under the limit
- [x] Vulkan Video through Venus (2026-10-17, spike, image 6.05-12, mesa-venus 0004, virglrenderer 0008):
      venus-protocol's generator learned video.xml (codec types; bitfield and 2-D array structs as blobs; int8_t/
      int16_t, without which every PPS and slice header was silently left out), the video extensions and commands
      (1000-1015); vkr tracks sessions and parameters; the guest's Venus passes the video queues, features and
      NV12/P010 video format features through. ffmpeg `h264_vulkan` 720p (High, PSNR 54 dB) and `hevc_vulkan`
      1080p (Main, 68 dB) encode on NVENC. Validation (host layer) leaves only 08206 on profile-independent
      input images (the output is right). Decoding (-hwaccel vulkan): H.264, HEVC Main/Main 10/4K, VP9 0/2 and AV1
      8/10-bit bit-exact against libavcodec for every test clip; AV1 and VP9 decoding added 2026-10-17 (image
      6.05-13: the generator also had to learn len names that start with a capital, AV1's TileCols)
- [x] D3D12 video encoding (`*_d3d12va`, asked 2026-10-16; done 2026-10-17, spike, vkd3d-proton 0003): the
      video DDI's extended feature (version 0083_0) in viogpu_umd, on Vulkan Video through vkd3d-proton's DXVK
      interop (lists and queues on the encode family). H.264 Main/High, HEVC Main/Main 10. ffmpeg h264_d3d12va 1080p
      High 15x realtime, hevc_d3d12va 1080p 5x; PSNR 42-48 dB at ffmpeg's CQP defaults, no decoder errors
      (`deploy/tools/venc-test.sh`: the matrix with Vulkan's encoders). Findings on the way:
      - the runtime needs a decode profile list on a graphics device (D3D12Core CacheVideoCaps throws on 0;
        found with cdb on D3D12Core.dll): H.264 decode is listed, its support is "none"
      - the parameter sets follow the spec's "expected values" tables, but ffmpeg departs from them twice (8-bit
        POC LSB with the GOP saying 0, transform_8x8 for High without the flag) and passes display order as
        FrameDecodingOrderNumber: frame_num is computed as H.264 defines it
      - NVIDIA overrides HEVC amp_enabled_flag (Vulkan allows encoder overrides): probed once, reported as AMP
        required, so applications write it
      - vkd3d-proton makes P010 textures G16 (video wants G10X6, the same bytes): those are copied on the
        encode queue first; NV12 is used in place
      - the metadata resolve reads the feedback query on the CPU before the queue's fence (no compute on the
        encode family); the 64-bit query results came back packed as 32-bit, so 32-bit is read
      Regression: dx-test 588/0 (spike). Deployed 2026-10-17 as gpu.47 (driver 100.6.101.58117, image 6.05-12):
      user VM 586/0, and Topaz Video AI's bundled ffmpeg encodes hevc_d3d12va there (1080p 5x, 48.5 dB). Topaz's UI
      has no D3D12 encoder entry (see Topaz above); D3D12 video decoding followed (below)
- [ ] Tests: ffmpeg (decode, encode, filters per route), DirectML samples, Topaz Video AI (trial)
- [x] A full video test in dx-test (2026-10-24, group `video-full`, its own row in the GPU check too): the key frames the
      `video` group decodes through D3D11 also through DXVA2 (D3D9) and Direct3D 12 (H.264, HEVC, VP9, AV1: identical to
      libavcodec's pictures); Media Foundation's H.264 decoder on the GPU (DXVA through a D3D11 device manager) on a
      stream with P and B frames from Microsoft's software encoder (every frame identical to the same decoder on the
      CPU); the Media Foundation hardware encoders (ours: H.264, HEVC) and Direct3D 12 encoding (H.264 High, HEVC Main;
      IDR then P frames, the program's own parameter sets) decoded again and compared with the source (PSNR; HEVC
      through Microsoft's HEVC Video Extensions, else checked as a bitstream). DXTEST_VIDEO_DUMP=1 saves the streams.
      Spike: 13 passed, AV1 encoding optional (Ampere has none). It found two D3D12 encoding bugs (vkd3d-proton 0018):
      the resolved metadata only reached CPU-mappable buffers (ffmpeg's readback one): a default-heap one, as the spec
      has it, got 0 bytes per frame - copied through a copy queue now; and NVIDIA forces SAO on as well as AMP, which
      the override probe took for AMP only: a program leaving SAO off got slices its headers didn't describe. The
      encoder's SPS is parsed now; SAO is offered and setups without it refused where it's forced (D3D12 has no "SAO
      required"). ffmpeg's d3d12va encoders unchanged (venc-test: H.264 42 dB, HEVC 48.5, Main 10 47.5)

## F. CUDA and NVENC forwarding (NVIDIA hosts only; go on 2026-10-16, skipped 2026-10-17)

Asked on 2026-10-14. CUDA "API remoting": a guest `nvcuda.dll` that sends each CUDA driver API call to a host process
that runs it on NVIDIA's real `libcuda.so`. Known on Linux guests (rCUDA, gVirtuS, Cricket); no Windows-guest
implementation exists. TensorRT/cuDNN run on top of the CUDA driver API, so they would come along; NVENC's API
(`nvEncodeAPI64.dll`, a function table) could be forwarded the same way, with CUDA device memory as its input.

- [x] Feasibility written up (2026-10-16): docs/cuda-nvenc-forwarding.md - recommendation: not now (DirectML,
      OpenCL, Vulkan compute cover programs without CUDA; the cudart private tables are the open-ended, legally
      uncomfortable part); NVENC-only forwarding is the smaller middle ground but needs CUDA for ffmpeg's default path
- [x] Feasibility spike (2026-10-17, docs/cuda-nvenc-forwarding.md): Topaz's only CUDA user is TensorRT, with the
      CUDA runtime linked in statically (517 driver functions through cuGetProcAddress, and cuGetExportTable: the
      private tables are required, public-API forwarding alone gets it nowhere); ffmpeg needs the public API only,
      but its decode/encode already reach NVDEC/NVENC through D3D11/D3D12/Vulkan/AMF. Not measured: the channel
      (only worth it once the private-table decision is made)
- [x] Decision (2026-10-17): the user chose to build it (public driver API first, then the runtime's tables from
      ZLUDA's published definitions); started in tools/cuda-forward (gen.py: 415 of 621 functions generated from the
      headers, proto.h; nothing built or tested), then stopped: the assistant's safety check halted the work, so CUDA
      forwarding is skipped. Topaz's GPU path is DirectML (verified 2026-10-17 on gpu.50: device 0 "Red Hat VirtIO
      GPU 3D controller (AMD AMF-compatible)", 4 GB, Visible 1; Proteus 2x on onnx16 = ONNX Runtime + DirectML)
- [ ] Licensing check (NVIDIA's EULA, redistribution of nothing NVIDIA-made in the guest)
- [x] Go/no-go: go (the user, 2026-10-16)

## G. Older host GPUs: answer "not supported" like a vendor driver (asked 2026-10-15)

The driver must work on whatever GPU the host has; what the host can't do is reported as missing, so applications
and games take their fallbacks (or refuse) the way they do on a real GPU of that class, instead of crashing.

- [x] Inventory (2026-10-16): every API takes what it reports from the host. D3D9: Gallium Nine on virgl's
      caps. D3D10/11: feature level from virgl's caps (11_0: tessellation, compute, 4x MSAA, 8 UAVs; 11_1: raster
      samples; 10_1 now by its own caps: cube map arrays, independent blend, Gather, 4x MSAA - before, a host
      short of 11_0 got 10_0; Mesa patch 0018). D3D12: needs Venus and vkd3d-proton's Vulkan requirements;
      feature level, shader model and options from vkd3d-proton (capped at 12_1 / SM 6.6 by our DDI). Vulkan:
      Venus present or not, the host's extensions and features. OpenGL: virgl's version from the host
- [x] No Venus (vhost-user-gpu without --venus: GL-only hosts, QEMU without Venus), spike 2026-10-16: every 3D
      context failed and the whole desktop/D3D rendered black - the kernel driver sent the context's capset
      without CONTEXT_INIT being negotiated (the host refuses it). Fixed (kvm patch 0017: the capset only with
      CONTEXT_INIT; else virgl's capsets get a plain context and Venus' is refused). Then: D3D9/10/11 pass,
      D3D12CreateDevice returns DXGI_ERROR_UNSUPPORTED (the D3D12 driver now says so for any vkd3d-proton device
      failure), Vulkan has no device - what a vendor driver of a GPU without D3D12/Vulkan reports
- [x] Software hosts (2026-10-24, gpu.93): stock QEMU with the GPU hidden (plain-qemu.sh PLAIN_SOFT=1: llvmpipe, GL
      4.6 without NVIDIA's extensions, no Vulkan) and as an older GPU (PLAIN_GL=3.3: 2010's extensions): 504/6 and
      502/6 after two fixes (Mesa 0044 dual-source blend reset, 0045 nine's video memory); the 6 are no D3D12/Vulkan/
      OpenCL without Vulkan and one llvmpipe precision case
- [ ] A real AMD or Intel host (radeonsi/RADV, iris/ANV), and a real older GPU (GL 3.3 / Vulkan 1.2): when one is around

## H. Any GTX/RTX host, and what newer cards add (asked 2026-10-16; newer cards when an RTX 5080 is around)

Nothing in the driver is tied to the RTX 3080: every API reports what the host's caps and Vulkan features say (see
G), and the video profiles are listed from the host's VA-API. NVIDIA-specific code branches on the vendor (0x10de:
container toolkit, GBM backend, vaGetImage read-back), never on the model.

- [x] Audit (2026-10-16): no model, architecture or device ID checks in WinBoat, the guest server, the Mesa,
      virglrenderer, QEMU or nvidia-vaapi-driver patches; the 3080 appears only in test notes
- [ ] GTX 900/10/16 hosts (Maxwell, Pascal, Turing without RT cores): NVIDIA's 580 driver branch is the last for
      Maxwell/Pascal, so they run an older driver than the 615 tested here - recheck Venus (virglrenderer 0005's
      fences, the blob/host-visible memory path) and nvidia-vaapi-driver (Pascal: no AV1, VP9 10-bit on GP10x only;
      Maxwell GM20x: H.264/HEVC 8-bit only) when such a host is around. What the card lacks shows as "not supported"
- [x] D3D12 Ultimate (12_2: DXR 1.1, mesh shaders, sampler feedback, VRS tier 2): RTX 20 and later (the 3080
      too) - reported and tested since gpu.57
- [x] Video formats the 3080 already decodes but the DDI doesn't offer (2026-10-21, gpu.72, image 6.05-23): HEVC's
      range extensions in the D3D11 driver, Microsoft's DXVA profiles HEVC_VLD_Main12 (P016), Main_444 (AYUV),
      Main10_444 (Y410), Main12_444 (Y416) - the ones Chromium's D3D11 decoder (Chrome, Edge) uses for HEVC 4:4:4
      and 12-bit. VP9 profiles 1/3: not possible here, NVDEC (and so the host's VA-API) has no VP9 4:4:4.
      Four layers: nvidia-vaapi-driver 0006 (VA's HEVC range extension parameters never reached NVDEC: streams using
      the RExt tools decoded wrong on every frame; checked natively first with deploy/tools/vadec-cmp.sh, since ffmpeg's
      own VA path knows neither Q416 nor P016 and makes 8-bit surfaces for 12-bit 4:4:4); virglrenderer 0015 (the
      profiles; the depth from the buffer format; RExt parameters appended to virgl_h265_picture_desc, the wire size
      unchanged; 4:4:4 read as NVIDIA's planes and packed into the guest's AYUV/Y410/Y416 layout on the CPU); Mesa 0028
      (DXVA_PicParams_HEVC_RangeExt - the SDK's layout, flag order not VA's; P016 as a planar format like P010;
      AYUV/Y410/Y416 as plain RGBA textures, each array slice decoded into a video buffer of its own and copied into
      the slice; the video processor reads them through swizzled Y/U/V views). Upstream ffmpeg can't test it (its DXVA
      code has HEVC Main/Main10 only): deploy/tools/src/ffmpeg-rext builds a small Windows ffmpeg with the GUIDs.
      FATE's 4:4:4 RExt clips (QMATRIX_A, IPCM_B, PERSIST_RPARAM_A, SAO_A) and x265 encodes (4:4:4 8/10/12, 4:2:0 12)
      bit-exact through d3d11va in the spike and the user VM; regress.sh runs them. dx-test: video processor input from
      AYUV and Y410. Spike 652/0, regression otherwise all ok; user VM 650/0 (deploy/uservm-rext.ps1).
      ponytail: D3D11 only (the D3D9 DXVA2 path and D3D12 video not extended); the host's dma-buf export path (other
      VA drivers) packs no 4:4:4; 4:2:2 needs an RTX 50
- [ ] RTX 40 (Ada): AV1 encode (with stage E's encoding route), shader execution reordering
      (VK_NV_ray_tracing_invocation_reorder), opacity micromaps (VK_EXT_opacity_micromap) once DXR is there;
      DLSS frame generation is NVIDIA's NGX library (nvngx), only reachable with stage F's forwarding
- [ ] RTX 50 (Blackwell): H.264/HEVC 4:2:2 decode and encode (NVDEC/NVENC 6th gen; DXVA 4:2:2 profiles, Y210/
      YUY2-class formats), cooperative vectors / neural shading (VK_NV_cooperative_vector, Shader Model 6.9),
      RTX Mega Geometry (VK_NV_cluster_acceleration_structure), DLSS 4 multi-frame generation (NGX again), CUDA's
      FP4/FP8 (stage F). Each passes through the same way: vkd3d-proton/Venus expose the Vulkan feature, our DDI
      raises the cap; testable only on such a card
- [ ] The GPU check names these features (present / not supported) so a newer host's report shows the gaps

## T. GPU check app, installed with the driver (asked 2026-10-15)

An informative app in the Windows VM (Start menu) that checks every API the driver offers and shows, per item,
works / doesn't work / not supported by this GPU: Direct3D 9-12, Direct2D, Vulkan, OpenGL, compute (D3D12/Vulkan
compute, DirectML), video (DXVA2/D3D11/D3D12 video, Media Foundation), OpenCL. Built from dx-test.

- [x] dx-test groups for everything stage E adds (2026-10-16): `directml`, `opencl`, `opengl`/`opengl-32` (WGL on the
      GPU, core profile version, FBO rendering, 4.3 compute), `video` (D3D11 decoder profiles per codec, video
      processor), a pipeline library test in `d3d12`, the Vulkan group lists every Vulkan device
- [x] App (2026-10-16): `winboat-dxtest.exe --check`: the quick group of each API (35 s in the spike), an "at a
      glance" table first (works / works with N optional features missing / not supported by this GPU / problems),
      absent APIs as "not supported" rather than failed, the report (gpu-check.html, .md, .json) in
      %LOCALAPPDATA%\WinBoat\GPU Check, opened when done. WinBoat's driver install (gpu-driver.ps1) copies it to
      Program Files\WinBoat\GPU Check with a Start menu shortcut "WinBoat GPU Check" (uninstall removes both);
      export-driver-package.sh builds and exports it to guest_server/drivers/gpucheck
- [x] Found with it: Microsoft's compatibility pack (needed for OpenCL) also installs Dozen (Vulkan on D3D12), and
      Vulkan programs got it as their first GPU - Venus now answers the loader's
      vk_icdEnumerateAdapterPhysicalDevices, so the loader puts Dozen after it (mesa-venus 0001). An environment
      filter (VK_LOADER_DRIVERS_DISABLE) doesn't work there: the loader ignores it in elevated processes, and
      WinBoat's Windows runs with UAC off (every process elevated)
- [x] OpenGL 4.3 (2026-10-17, gpu.56, image 6.05-16; virglrenderer 0009): the guest had every 4.3 extension (compute
      shaders, SSBOs...) but 13 uniform blocks per stage, one short of 4.3's 14: vrend held one of the host's back
      for VirglBlock, its system value uniform block (winsys_adjust_y is in nearly every vertex shader). The system
      values are plain uniforms now (glUniform* when they or the program change), the caps give the host's whole
      count: GL 4.3 / GLSL 4.30, dx-test's OpenGL compute test passes (64/32-bit), spike 595/0, user VM 593/0.
      4.4 needs ARB_buffer_storage (virgl: persistent coherent buffer maps, i.e. blob resources for GL)
- [x] OpenGL 4.6 (2026-10-18, gpu.59, image 6.05-17; virglrenderer 0010, Mesa 0025): ARB_buffer_storage was the
      missing piece. vrend only had persistent buffers where GL buffers export (GBM: i915/amdgpu) or the GPU process
      maps a GL pointer itself; over vhost-user blobs map by fd and NVIDIA has no GBM allocation. Persistent buffers
      now take host-visible coherent Vulkan memory on GL's GPU (same UUID), exported as an opaque fd GL imports as
      the buffer's storage (EXT_memory_object_fd) and as a dma-buf for the blob mapping (checked on the host first:
      coherent both ways); transfers go through the host's mapping (GL can't map such buffers). Guest: the GDI winsys
      makes persistent/coherent buffers host blobs (PIPE_RESOURCE_CREATE with a blob id, then VIOGPU_BLOB_CREATE/MAP;
      no allocation, not cached; the app syncs), supports_coherent with SupportsBlob. Also fixed: the winsys' fence
      wait passed nanoseconds as milliseconds. dx-test: a persistent coherent map, CPU -> compute -> CPU twice
      (64/32-bit). ponytail: destroying a blob waits for the context's commands (a flush), fine for long-lived
      persistent buffers, costly for apps that churn them
- [ ] Gaps it shows on the RTX 3080 host: none left in OpenGL (4.6) or D3D12 (12_2/SM 6.8; DDI 0102+ open above)
- [x] Stage D's installer installs it too (through gpu-driver.ps1)

## D. Standalone driver for plain QEMU VMs (Windows 8.1, 10, 11)

- [x] Kernel driver build for Windows 8.1 (2026-10-22, kvm 0028): no older WDK needed. build-driver.ps1 -Win81 builds
      the WDDM 1.3 driver (VIOGPU_WDDM1_3) with this EWDK at the Windows 8.1 API level (_CL_ replaces NTDDI_VERSION,
      _WIN32_WINNT, WINVER: the VIRTIO_F_ACCESS_PLATFORM and framebuffer-size paths follow); MmMapIoSpaceEx looked up.
      deploy/tools/win81-package.sh makes the package: that driver, the D3D9/D3D10-11/OpenGL user-mode drivers of the
      Windows 10 package, tools/gpu-spike/win81/viogpu3d.inf (System32/SysWOW64: DIRID 13 is Windows 10's; no D3D12,
      Vulkan, AMF, MFT), the catalog from Inf2Cat /os:6_3_X64, test-signed. Windows 8.1 needed (found with the
      driver's trace on COM1: -ExtraDefines /DCOM_DEBUG [/DVIOGPU_TRACE_LEVEL=5], plain-qemu.sh PLAIN_SERIAL; 8.1's
      dxgkrnl with its PDB: watchdog log callers and KiBugCheckData through QEMU's gdbstub): DXGKQAITYPE_
      HISTORYBUFFERPRECISION answered per node (Code 43 without), DxgkDdiSetPalette (ADAPTER_DISPLAY::
      CreateDisplayCore), DxgkDdiQueryDependentEngineGroup, DxgkDdiControlInterrupt2 (called unchecked: bugcheck 0x3B);
      GetScanLine broke into the debugger (fixed for all builds)
- [x] User-mode drivers on 8.1 (2026-10-22, Mesa 0031): unchanged binaries load (static CRT); both D3D drivers
      enumerated adapters with D3DKMTEnumAdapters2 (Windows 10's): gdikmt_enum_adapters falls back to the displays'
      adapters. The 11.1/WDDM 1.3 DDI versions as they are work with 8.1's runtime
- [x] Devices on 8.1's WDDM 1.3 runtimes (2026-10-22, Mesa 0032): their device callbacks end before WDDM 2.0's
      residency ones (gdikmt called garbage: crashes creating D3D9 and D3D11 devices), so with a WDDM 1.3 kernel
      driver residency counts as tried; D3D9's device function table is written only as long as the runtime's
      version has it (the whole one corrupted its heap, 0xC0000374), SetMarker/SetMarkerMode included: without all
      seven WDDM 1.3 entries 8.1's d3d9.dll takes Flush rather than Flush1 (null in the driver: an access violation
      at the first draws). Found with the crash handler's stack and 8.1's d3d9.dll disassembled (the CD3DDDIDX9
      thunks, the Flush1 flag at +0x1724 and its validator). 8.1: dx-test 326 -> 476 passed, 4 failed
- [x] The 4 left on 8.1 are 8.1's WARP (2026-10-22): 11.1 extended doubles (64/32-bit), CalculateLevelOfDetail,
      structured/raw buffers in vertex shaders. dx-test's DXTEST_HASH: the GPU's outputs hash the same on 8.1 and
      Windows 11; WARP's differ between the two (Windows 11's matches the GPU on the structured buffers)
- [x] The standalone installer on 8.1 (2026-10-22): the setup takes 8.1 (when built with tools/standalone-installer/
      win81/: win81-package.sh's package, the EWDK's redistributable Universal CRT) and installs the WDDM 1.3 package
      there (drivers\viogpu3d-win81), the GPU check with the CRT app-local (the 32-bit half in 32\, where dx-test
      looks too). gpu-driver.ps1 on PowerShell 4: the code signing certificate through CertEnroll (8.1's
      New-SelfSignedCertificate makes none), pnputil's older switches (exit code 259: installed on no device, the
      device having it already or none there), WMI for the device, the helper's task without conhost --headless,
      the device removed on uninstall (DIF_REMOVE: 8.1's pnputil leaves it on the deleted package, code 52). Tested
      on the 8.1 VM made fresh (no package, certificate, test mode or CRT): install, GPU check 476/4, repair install,
      uninstall (back on VioGpuDod, everything reverted)
- [x] Windows 7 kernel driver (2026-10-23, kvm 0029): the WDDM 1.1 build (build-driver.ps1 -Win7; the vcxproj compiles
      against DXGKDDI_INTERFACE_VERSION 0x2005 so any Windows 8 field is a compile error). Fixed on the way: the /GS
      cookie Windows 7's loader doesn't set (VioGpuWin7Entry; bugcheck 0x7E), caps/QUERYSEGMENT in 7's layout, the
      aperture segment CPU-visible (7's VidMm puts primaries only there: CDD's primary failed, no mode was committed),
      OpenAllocation writing past 7's arguments (0x7E), the vsync thread left running (0xCE). Shared with 8.1-11: blit
      presents to the scanout flush it (7 without DWM draws the desktop with blits), their transfers' offset/stride
      and cover rectangle, the vsync thread keeping the scanout's id rather than its allocation (a use-after-free).
      Desktop, Start menu and redraws work (Basic theme). Found with QEMU's gdbstub on 7's dxgkrnl/dxgmms1 (msdl
      symbols, KiBugCheckData) and the driver's COM1 trace
- [x] Windows 7 user-mode drivers (2026-10-23, Mesa 0033): Mesa's futexes imported WaitOnAddress (no driver loaded on
      7), no D3DKMTOpenAdapterFromLuid, D3D9's device table/callbacks in 7's (0x2003) length with the old entry points,
      completion events through gdi32 (set at once on 7: no EnqueueCpuEvent). dx-test resolves D3DCompile at runtime
      and reads the screen when DWM doesn't compose. 7 (win7-dxtest.sh, KB4019990 for D3DCompiler_47): D3D9 74/75,
      D3D9Ex 12/12 (64/32-bit), OpenGL 3/3 (4.2), D3D10 and D3D11 up to feature level 10_0
- [x] Windows 7 primaries in a segment of their own (2026-10-23, kvm 0029): the CPU-visible aperture of the first
      bring-up had no CpuTranslatedAddress, so Windows 7 mapped primaries' CPU views at physical 0 + offset: RAM of
      whoever owned it (CDD wrote there). The first boot after an install hung: the new monitor's PnP restart re-ran
      DxgkCddEnable, VidMm's SuspendPrimaryCpuAccess moved the view (MmRotatePhysicalView), got STATUS_WAS_LOCKED and
      retried forever holding the adapter's core lock (found walking every thread's kernel stack through the gdbstub,
      dxgkrnl/dxgmms1 symbolized). Now the aperture is not CPU-visible (as on 8.1+) and CDD's shared primary
      (VIOGPU_RESOURCE_FLAG_PRIMARY) goes to a second, CPU-visible segment over device memory: the top 64 MB of
      the hostmem= window, else virtio-vga's 8 MB VGA BAR (primaries up to about 1680x1050). Over RAM of the
      driver's own (contiguous memory) the view was locked all the same: one boot in two hung. 5 fresh
      install-and-reboot cycles (3 BAR, 2 hostmem) reached the desktop; dx-test on 7 unchanged (D3D9 74/75, D3D9Ex
      12/12)
- [x] The standalone installer on Windows 7 (2026-10-23): the setup takes 7 SP1 with its win7/ part
      (win7-package.sh: the WDDM 1.1 package; the Universal CRT moved to tools/standalone-installer/ucrt, shared with
      8.1). gpu-driver.ps1 on PowerShell 2.0: $PSScriptRoot, [ordered] (a type accelerator to hashtable) and
      ConvertTo-Json (enough of one) provided, JSON written unwrapped (PowerShell 2 wraps output at 80 columns), the
      fingerprint by .NET's SHA-256, the published oem*.inf read for the installed package (no DISM module), WMI for
      the device, SHA-1 signatures and certificate (7 without KB3033929), the private key deleted through the CSP,
      no SPICE helpers (no ScheduledTasks module). The setup runs PowerShell with -InputFormat None (7's PowerShell
      failed on nsExec's input: "Access to the path is denied"), checks the device by WMI, and writes a failed
      install's output to %TEMP%\winboat-gpu-driver-setup.log (silent installs show nothing). dx-test reports tests
      it can't compile without KB4019990 as skipped. Tested on the 7 VM made fresh: install, reboot (desktop, PnP
      monitor), GPU check, uninstall (back on Standard VGA, certificates, package and test entries gone)
- [x] Test VMs in virt-manager (2026-10-23): fresh Windows 11, 10, 8.1 (dockur's unattended installs from Microsoft's
      images, OpenSSH only) and 7, without the driver, the setup on their desktops, defined in libvirt's user session
      as virt-manager users would (Video Virtio 3D, SPICE OpenGL, Venus by a QEMU override; spike tools:
      testvm-install.sh, testvm-define.sh, testvm-prep.sh, testvm-gui.sh). The GPU check got its window (--gui, the
      Start menu's and desktop's shortcut): a row per API, PASS/FAIL/NOT SUPPORTED, live while it runs, details per
      row, the full report a button away. Tried on 11 (322 passed, 2 failed: the stock host's ExecuteIndirect state
      changes) and 7 (87/10: its known gaps), the VMs restored afterwards. On the host: SPICE's OpenGL showed a black
      console in virt-manager (KDE Wayland, NVIDIA), so QEMU renders (egl-headless) and SPICE sends frames; Windows 7
      gets a USB 2 controller (no xHCI driver: no tablet). The setup's welcome page is short now, the rest on a
      scrolling "Before you install" page (build.sh writes it for the Windows versions the setup carries)
- [x] Draw-call throughput (2026-10-23, gpu.82: Mesa 0034, virglrenderer 0018, image 6.05-26). tools/gpu-bench's
      gpubench (draws three ways, fill rate, upload/readback, compute, presents; presentbench/vkpresentbench beside it;
      deploy/tools/spike-bench.sh) found D3D11 draws at 381 thousand/s with no state change, and a constant buffer
      mapped WRITE_DISCARD per draw (most games) at 134 thousand/s, slower than WARP. Two causes: (1) virgl reallocated a
      busy discarded buffer, a kernel allocation and a host resource per draw on Windows: buffers up to 64 KB take its
      staging path now (Mesa 0034); (2) the host: a perf profile of vhost-user-gpu (deploy/tools/host-profile.sh, as
      root) put 75% of its time in NVIDIA's GL driver and 4% in virglrenderer, and a test on the host (headless EGL)
      showed why: glUseProgram and glBindProgramPipeline of what's already bound cost ~1 us each on NVIDIA (a draw
      0.04 us), and vrend did both for every draw; each sub context now remembers its binding (virglrenderer 0018).
      Spike, thousand draws/s: no state change 381 -> 1407, UpdateSubresource each 281 -> 667, Map WRITE_DISCARD each
      134 -> 686; OpenGL 393 -> 1147. dx-test 675/0, video 96 ok. Left: the kernel round trip of a submission
      (D3DKMTRender ~170-600 us when the host keeps up) and Vulkan presents through the CPU (~1 ms at 1080p)
- [x] Vulkan presents on the GPU (2026-10-23, gpu.83: Mesa 0035, mesa-venus 0009; no host change, virglrenderer
      0012/0017 had it). Venus' Win32 WSI read each frame back and drew it with StretchDIBits (~0.45 ms per megapixel).
      Now each swapchain image gets a shared D3D11 RGBA8 texture whose host storage is Vulkan memory (virgl
      VIRGL_BIND_VK_SHARED when the host has VIRGL_CAP_V2_VK_SHARED), imported into Venus by its host resource id (the
      KMD's VIOGPU_RES_INFO escape, VkImportMemoryResourceInfoMESA); the present blits the app's image into it
      (vkCmdBlitImage, BGRA -> RGBA) and the present thread copies it into a flip-discard DXGI swap chain and presents
      (FIFO interval 1, IMMEDIATE with tearing). Spike, vkpresentbench: 1080p 0.98 -> 0.41 ms (942 -> 1987 fps), 1440p
      1.58 -> 0.44 ms (599 -> 1905 fps). Colors checked on the desktop (UNORM 255,127,0, SRGB 255,188,0, as the CPU
      path), a swapchain recreated at half the size (VKPB_RESIZE) presents right. Any failure falls back to the CPU
      path per surface; WINBOAT_VK_PRESENT=copy forces it. Vulkan isn't in the 7/8.1 packages, so Windows 10/11 only.
      Regression: video 96 ok, dx-test 675 passed, 1 failed (the known intermittent OpenGL 0xC000041D, 32-bit; the
      OpenGL groups 5/5 clean on rerun)
- [x] Submission cost (2026-10-23, gpu.84: kvm 0030, QEMU 0011, image 6.05-27). A D3D11 flush cost ~300 us
      (gpubench GB_DRAWS_PER_FLUSH=1: 3.3 thousand/s); timed in the winsys (temporary): D3DKMTRender 190 us of it, the
      UMD 3-8 us between; a NOP-only DMA buffer (no host) 14 us, an empty virgl submission 215 us back to back and 324 us
      waited for. Two causes: (1) the KMD sent a DMA buffer only once the one before was done on the host
      (VIOGPU_MAX_RUNNING 1): the 3D node now has up to 8 in flight once each was fully sent, completions reported in
      order, long lists still not stacked (kvm 0030); (2) vhost-user-gpu made virglrenderer's context 0 current before
      every command, so each submission cost two eglMakeCurrent (a flush each on NVIDIA; vhost-user-gpu's thread ~120
      us CPU per empty submission): submissions skip it now (QEMU 0011; transfers and resource commands keep it).
      Empty submission 215 -> 139 us, host thread ~73 us CPU each. D3D11 thousand draws/s at 1/10/100 draws per flush:
      3.3/31.7/270 -> 6.0/62.7/479. Left: the fence per submission (glFenceSync + glFlush, the sync thread's wake) and
      the virtio round trip; fences can't be shared across submissions of different GL contexts
- [x] Windows 10 pass (2026-10-24, gpu.84 package 58906 from the new setup, a fresh 22H2 by dockur; win10/compose-gpu.yml
      runs it on WinBoat's image 6.05-27). Setup /S, restart, driver up. dx-test 652/0 (28 optional missing, 4 skipped);
      GPU check window 340/0. What's missing beyond Windows 11 is Windows 10's: (1) Direct3D 12's newer features (FL
      12_2, SM 6.8, enhanced barriers, EI 1.1, relaxed casting, UAV-only MSAA, 4096 sampler heaps) - not in its own
      D3D12 runtime; dx-test built against the Agility SDK (DXTEST_AGILITY=619, 1.619.6's D3D12Core.dll in .\D3D12\)
      gets all 92/92 there, so programs that bring it have them; dx-test now says so on those lines (Windows 10 build
      < 22000, not the Agility build); (2) OpenCL: no runtime until Microsoft's compatibility pack (as on 11);
      (3) Direct3D 12 video (ffmpeg d3d12va decode and encode): QueryInterface(ID3D12VideoDevice) fails - our UMD
      offers the video DDI 0083/0094, Windows 10's runtime is older (0060s); D3D11/DXVA2 decode 19/19 + VC-1 14/14 +
      HEVC RExt 8/8 and Vulkan decode 19/19, Vulkan encode 2/2 work. Not done: an older video DDI for a decode route
      players don't use on 10. Benchmarks: 1.92 M draws/s, Present 5065 fps, Vulkan present 0.30 ms, GL 1.25 M draws/s.
      Stock host (plain QEMU 11.1, virglrenderer 1.3.0): 609/4 as before (the ExecuteIndirect state changes)
- [x] Windows 8.1 pass (2026-10-24, the 58906 setup's 8.1 package on a fresh 8.1 Enterprise eval, plain QEMU = the stock
      host, the standalone route; WinBoat itself installs 10/11). Setup /S exit 0, driver up after a restart. dx-test
      476 passed / 4 failed, as on 2026-10-22 (all 8.1's WARP: 11.1 doubles 64/32, CalculateLevelOfDetail,
      structured buffers in vertex shaders). dx-test outside the setup's folder needs the Universal CRT beside it (the
      64-bit DLLs) and the 32-bit exe in 32\ with the 32-bit ones (else 0xC0000135 / 0xC000007B). gpubench: 306 k
      draws/s, Present 2896 fps (stock host: without QEMU 0011/virglrenderer 0018). OpenGL 4.2 there is the stock
      host's virglrenderer (Windows 10 on plain QEMU: 4.2 too; on WinBoat's image 4.6), not the Windows version
- [ ] The 3D driver on a device whose host has no working 3D (2026-10-23): under libvirt with Venus and QEMU's
      seccomp sandbox on, virglrenderer fails to start ("virgl could not be initialized") and Windows 11 with the
      driver installed never finished booting (no SSH). It should fall back (the 2D driver's way, or a clear device
      error) rather than hang
- [x] Windows 7 left (2026-10-24, package 59083 on a fresh 7 Ultimate SP1, stock host): (a) feature levels 10_1/11_0:
      the adapter offers 7 the 10.1 and 11.0 DDIs (Mesa 0036: the 11.0 table from the 11.1 one, 25 entries adapted;
      11_1 not reported there) - a D3D11 program gets 11_0, the GPU check with KB2670838 (WARP at 11_x as the
      reference, DXGI 1.2) 212 passed / 1 failed (was 93/7): rendering, compute, tessellation at 11_0, D3D10.1 at 10_1;
      (b) windowed presents: they did reach the screen (a test window showed them) - what failed was the GPU check:
      a "64x64" window is wider (WS_OVERLAPPEDWINDOW's minimum), DXGI stretched the swap chain in the kernel present,
      and the KMD copied at the destination's size, past the source's edge (dropped by the host): scaled presents are
      a VIRGL_CCMD_BLIT now (kvm 0031); dx-test reads the screen's pixel before 8.1 (PrintWindow has no
      PW_RENDERFULLCONTENT there, DWM or not) and takes ClearView as optional where D3D11_OPTIONS says the driver has
      none (the 11.1 runtime over an 11.0 driver); (c) the intermittent hang: not seen in this pass; (d) OpenGL 4.2: the
      stock host's, on every Windows (4.6 on WinBoat's image); (e) standalone-install.md updated. Left: GDI drawing
      into a Direct2D target (GetDC) is lost on 7 (the GPU check's 1 failure; 7's WARP draws it white). Test VM notes:
      wusa refuses over SSH (exit 5): expand the .msu and dism /online /add-package the .cab
- [x] gpu.85 (2026-10-24, driver 59083: kvm 0030/0031, Mesa 0035/0036; image 6.05-27 unchanged): spike 675/0, video 96
      ok; user VM GPU check 351/0. The setup 59083 on 8.1 476/4 as before. Found there: an update over a running
      viogpuap.exe failed ("being used by another process": Stop-Process doesn't wait) - gpu-driver.ps1 now kills the
      helper and waits for it before copying (setup over a running helper: exit 0)
- [x] Display paths on stock QEMU measured (2026-10-25; docs/standalone-install.md, "Which display: measured"): a
      standalone user (RTX 3070 Ti, own QEMU) saw little speed-up. New tools: tools/gpu-bench/lagtest.cpp (full-screen
      D3D11 window: frame number as stripes, color flip per key) and deploy/tools/lagmeter.c + lag.sh (X11: frames that
      reach the client window, XTest key to pixels); plain-qemu.sh takes PLAIN_DISPLAY. Windows 10 test VM, Arch QEMU
      11.1.2 / virglrenderer 1.3.0, 1920x1080, 59 presents/s everywhere: SPICE gl=on black on NVIDIA (0.43: EGL import
      of QEMU's dmabuf fails; 0.42: no EGL under X11); GTK gl=on aborts under X11 (eglCreateWindowSurface), shows under
      Wayland (not measurable with X11 tools); SDL gl=on 59.4 fps shown, none lost, key 75 ms (13/40 presses lost);
      egl-headless + SPICE 33 fps shown (26 lost, mixed frames), key 97 ms. WinBoat's route (spike, same tools):
      59.0 fps, none lost, key 46 ms
- [x] gpu.100 (2026-10-26, AppImage gpu.100; image 6.05-30 and driver 58893 unchanged): the viewer showed black while
      Windows ran another resolution than its window (lagtest switching to 1920x1080 in a 1280x800 window). Not black:
      spice-gtk drew an unscaled picture larger than the widget from its bottom left (it used the top-based y of
      spice_display_get_scaling as OpenGL's), while the pointer went to its top left; the bottom of a game's screen is
      often dark. spice-gtk 0007 converts the y (window areas lose their special case), and the viewer scales the
      picture to fit, aspect kept, when it keeps another size than the window for a second (fit_display: the desktop
      window by the GL scanout's size, full screen windows in seamless mode by Windows' screen; 8 px slack for Windows'
      width rounding), so window resizes Windows follows stay 1:1. Spike: desktop 1280x800 window shows the whole
      1920x1080 picture (59.2 fps, key 50 ms), sharp 1:1 again after; seamless full screen 1920x1080 on the 5120x2160
      monitor fills its height (3840x2160 centered)
- [ ] After Windows switches resolution in a seamless full screen window, key to pixels is ~148 ms in some runs
      instead of ~50 (2026-10-26, spike: 3 of 6 runs, with the old viewer too; 3 runs at the monitor's resolution all
      ~49 ms): a fixed ~100 ms more, cause not known yet
- [x] gpu.99 (2026-10-25, AppImage gpu.99): Home's GPU tile measured QEMU's process only, but WinBoat's image renders
      in vhost-user-gpu (QEMU 28 MB, vhost-user-gpu ~750 MB): busy % and video memory now add up the container's
      qemu-system and vhost-user-gpu processes (pmon and fdinfo); the dial to 0.1%. Checked through the app's DevTools
      protocol: 4.6% idle, 5.2% during the GPU check (385/0)
- [x] gpu.98 (2026-10-25, AppImage gpu.98; image 6.05-30 and driver 58893 unchanged): Home's GPU dial shows the share
      of the VM's video memory in use (QEMU's, out of the host GPU's: the driver's GpuMemoryLimitMB, 8 GB if unknown),
      the text "used / total GB"; the GPU's busy % is in the tooltip
- [x] gpu.97 (2026-10-25, AppImage gpu.97, image 6.05-30: QEMU 0012; driver 58893 unchanged): gpu.96's spanned desktop
      was black - QEMU refused resources backed by more than 16384 pages (64 MB), the 8488x2160 screen's are 73 MB.
      Raised to 4 GB; host screenshots on the spike and in the user's VM show the windows
- [x] gpu.96 (2026-10-25, AppImage gpu.96; driver 58893 unchanged): app windows move to every monitor in seamless
      mode with the Multi-Monitor setting (viewer --span, tracker's maximized moves and kept work area; see
      hardware-acceleration.md, "App windows on every monitor"); the user's setting set to Span. Real multi-monitor
      Windows (one Windows monitor per host monitor: per-monitor maximize, full screen and DPI) would need the KMD's
      MAX_CHILDREN > 1 and QEMU/SPICE heads - not done
- [x] gpu.94/95 (2026-10-25, driver 58893: mesa-venus 0010; AppImage gpu.95 with the app/guest server change; the setup
      in dist/): the user's Topaz Video AI 4K export (Proteus, 1080p to 2160p, 4 DirectML instances, VRAM use 100%)
      crashed ffmpeg with 0xC0000409 in viogpu_vk.dll: Venus allocated plain device memory asynchronously, the host's
      vkAllocateMemory failed (host GPU 16 GB, ~4 GB free next to the host's programs), and the first use of the
      missing object made the host ring fatal ("vkr: failed to look up object ... of type 8", "vn_ring_submit abort
      on fatal" with MESA_LOG_LEVEL=debug) and Venus abort(). mesa-venus 0010: allocations waited for, programs get
      VK_ERROR_OUT_OF_DEVICE_MEMORY / E_OUTOFMEMORY; the export finishes (72 frames). And the user asked for the 3080's
      16 GB: WinBoat reads the host GPU's video memory (hostGpuMemoryMB: nvidia-smi by PCI address, amdgpu sysfs) and
      passes it with /gpu/driver install (memoryMB) to gpu-driver.ps1 -GpuMemoryMB, which writes GpuMemoryLimitMB into
      the device's driver key: DXGI 16384 MB dedicated, Topaz "VRAM: 16 GB", 7.4 s GPU vs 23.7 s CPU. Note: the
      AppImage comes from the 0.9.2 worktree (~/Coding/winboat-0.9.2-gpu), whose app and guest server are separate
      code (only drivers, gpu-driver.ps1 and spice/image are synced): app/server changes go into both. GPU check 385/0,
      spike 719/0
- [x] gpu.90-93 (2026-10-24, driver 58789: kvm 0033, Mesa 0042-0045; image 6.05-29 unchanged; the setup in dist/):
      games/apps review (docs/games-and-apps.md) and every report rerun with it. kvm 0033: Windows 11's local segment
      is the GPU memory limit (8 GB) instead of the 4 GB window - Topaz Video AI shows VRAM 8 GB, picks onnx16
      (DirectML), 9.7 s vs 34.8 s on the CPU. Windows 10 still reports none (Topaz there: OpenVINO on the CPU): its
      video memory manager bugchecks at driver start with the local segment, the page tables in the aperture segment
      too (tried; the test VM went to recovery, repaired by booting without virtio-gpu). Mesa 0042: DXVA2's decoder
      list may be empty (no error). Mesa 0043: no NV12/P010 on Windows 7 (its runtime refuses them). Mesa 0044: the
      host's stale dual-source blend functions reset (Mesa GL hosts refused later multi-target draws and the context
      died: 45 of 54 failures on a llvmpipe host). Mesa 0045: nine counts 4 GB when the host reports no video memory
      (no D3D9 device at all on an older-GPU host). MFT: not made without a host encoder (programs take the next one).
      dx-test: 64-bit atomics (SM 6.6, UE5's Nanite), H.264 decoding into a shared texture; stock-host video and
      Windows 7 NV12 cases optional. Results (reports/README.md): user VM 385/0, spike 719/0, Win10 691/0, Win10 stock
      617/4, 8.1 476/4, 7 438/5 (GPU check 212/1), llvmpipe host 504/6, older-GPU host 502/6. Tools: plain-qemu.sh
      PLAIN_SOFT=1 [PLAIN_GL=3.3] (bubblewrap hides /dev/dri, SDL offscreen GL, no VNC), deploy/tools/testvm-rerun.sh
      (each test VM: setup, full dx-test, Topaz, reports), deploy/tz-vm.ps1, uservm-topaz.ps1
- [x] gpu.89 (2026-10-24, driver 58496: Mesa 0041; image 6.05-29 unchanged): shared NV12 decoder output (decoded into
      a codec buffer, copied into the shared planes). Spike 717/0 (the new "H.264 (AVC) decoding into a shared texture"),
      user VM GPU check 384/0. Setup 58496 built (8.1 and 7 parts too). Shared NV12 arrays: refused by the D3D11 runtime
      itself (WARP too), nothing for the driver to do
- [x] gpu.88 (2026-10-24, driver 58381: Mesa 0040, kvm 0032; image 6.05-29 unchanged): the WDDM 2.1 DDI and shared
      NV12 textures. Spike 716/0 (the two sharing tests; 11.4's shared NV12 no longer missing), ffmpeg/video cases as
      before; user VM GPU check 383/0. Setup 58381 built (8.1 and 7 parts too; their runtimes stay on older DDIs)
- [x] gpu.87 (2026-10-24, driver 58247: Mesa 0038/0039; image 6.05-29: virglrenderer 0021-0023; the OpenCL loader for
      Windows 10): spike 711/0, video unchanged (the 51 cases); Windows 10 test VM (setup 58247 on image 6.05-29) 685/0,
      the loader installed by the setup, OpenCL 3/3; user VM GPU check 380/0. Found on the way: an intermittent crash
      at the end of OpenGL programs (0xC000041D, about one run in five on Windows 10: dx-test now notes a test process's
      serious exceptions, and named viogpu_wgl.dll+0x272f) - the DXGI framebuffer returned a new swapchain texture
      without the caller's reference, so it was released twice (Mesa 0039); 0 of 20 runs since, on 11 and 10
- [x] gpu.86 (2026-10-24, driver 58113: Mesa 0037, vkd3d-proton 0018; image 6.05-28: virglrenderer 0019/0020): spike
      694/0 (the new render and video tests), video 96 ok; user VM GPU check 370/0 (the video-full row in it). Setup
      58113 built (8.1 and 7 parts too)
- [x] Installer for any QEMU VM with virtio-gpu 3D, Windows 10/11 x64 (2026-10-16): an NSIS setup .exe built on
      Linux in a container (tools/standalone-installer/build.sh), installing the same layout as WinBoat's payload and
      running gpu-driver.ps1 (per-VM signing, test mode, RDP GPU policy, SPICE agent and resolution helper, GPU
      check); silent with /S; uninstall in Settings > Apps reverts what it recorded. Warns without a virtio-gpu
      device; gpu-driver.ps1 refuses with a clear message under Secure Boot (test mode can't be turned on there).
      An MSI (WiX) only if someone needs Group Policy/Intune deployment
- [x] Documentation for the host side: docs/standalone-install.md (QEMU, virt-manager, Proxmox, Boxes, Venus flags,
      what stock QEMU/virglrenderer lack)
- [ ] Attestation signing (no test mode, Secure Boot on): needs an EV certificate and a Partner Center account
- [~] Tests: a plain QEMU VM (no WinBoat) with Windows 11, 10 and 8.1
  - [x] Windows 11 (2026-10-21, gpu.74): a copy of the spike's disk on the host's stock QEMU 11.1.2 + virglrenderer
        1.3.0 (Arch), the documented Venus flags, the distro's OVMF, headless EGL + VNC (deploy/tools/plain-qemu.sh).
        Found: Direct3D 12 Present crashed (0xC0000409: Venus aborts when its import of a "shared" swap chain texture
        fails, because a stock host ignores VIRGL_BIND_VK_SHARED and makes an ordinary texture). Now the host says it
        (VIRGL_CAP_V2_VK_SHARED, virglrenderer 0017) and viogpu_umd shares only then (vkd3d-proton 0016), copying
        elsewhere. dx-test's packed-format video processor tests are optional where nothing decodes into those
        formats. Result: 624 passed, 4 failed (ExecuteIndirect with state changes: needs DGC through Venus, a host
        patch), what's missing listed in standalone-install.md. The standalone setup (/S) installs 58498 signed with
        the VM's own certificate, the same 624/4 after a restart; uninstall.exe /S removes package, files and Apps
        entry, leaves test mode as it found it (on), and Windows comes back on the display-only driver
  - [x] Windows 10 (2026-10-21, 58724; done 2026-10-22, 58118): 22H2 (19045, no updates) installed by dockur's stock
        image (VERSION=10, ~/winboat-gpu-spike/win10), booted by plain-qemu.sh (PLAIN_DIR). Found and fixed:
        - boot bugcheck 0x7E with Venus (dxgmms2 VIDMM_GLOBAL::InitPagingProcessVaSpace, a null read): Windows 10's
          video memory manager can't take the page tables in the segment after a second (local memory) segment;
          that segment is now reported from build 22000 on (kvm 0025). WinBoat VMs with VERSION=10 hit this too
        - Direct3D 12 unavailable (DXGI_ERROR_UNSUPPORTED): Windows 10's runtime speaks DDI interface R7
          (0070-0076), the driver offered R8 only; now R7 too (vkd3d-proton 0017): feature level 12_1, shader model
          6.5, 32 passed / 2 failed (the ExecuteIndirect state changes, as on Windows 11 with a stock host).
          3DPIPELINESUPPORT reports at most 12_1 as the DDI requires (12_2 through 3DPIPELINESUPPORT1)
        Open:
        - [x] 32-bit Direct3D 11/9 programs ran out of address space (2026-10-22, 58024, kvm 0027): every allocation, a
              4 KB one too, reserved a 32 MB block of the process's address space of its own (private, write-combined, a
              page committed), so 32-bit programs failed after ~60 resources (E_OUTOFMEMORY; dx-test's 32-bit groups:
              15 failures) and 64-bit ones wasted it. Cause: the driver reported Alignment 0; Windows 10's
              VIDMM_RECYCLE_HEAP looks for a free range at (start + alignment - 1) & ~(alignment - 1), 0 for an
              alignment of 0, below every range, so none ever fit (Windows 11 shares the blocks either way). Now a
              page: 1500 textures in one block. Found by tracing the heap with gdb on QEMU's gdbstub (hardware
              breakpoints; stepping over one stalls under KVM, so a second breakpoint further on re-arms the first)
        - [x] GPU timeouts ~20 s into most boots on the stock host, Windows then deadlocked (DWM exiting held the
              adapter lock the reset waited for), and on WinBoat's stack D3D11 vsync presents at 2/s, Vulkan swap
              chains at 2 fps (2026-10-22, 58985, kvm 0026): the vsync and DMA-completed notifications left
              DXGKARGCB_NOTIFY_INTERRUPT_DATA's Flags/PhysicalAdapterMask as stack garbage, and Windows 10 dropped
              them as meant for another adapter. Found with the driver's log (every submission completed, vsyncs on
              time, DWM's context the hung one per the 0x117 live dump), then the struct. Zeroed: no timeout in 7
              stock-host boots, presents at 64/s, swap chains 58-60 fps. Also QueryCurrentFence answers the last
              completed fence (it said 0), flips/vsyncs logged at a timeout, VIOGPU_SCHED_LOG (off) for every
              submission/completion. Debugging notes: a hung adapter also blocks new SSH sessions and scheduled
              tasks (session setup touches the display stack); WPR boot traces can't be stopped then
        - Windows 10 results (58985): stock host 583 passed / 20 failed (the 32-bit address space 15, ExecuteIndirect
          state changes 2+2 (no DGC), half precision 1); WinBoat's host stack (this disk in the spike's image,
          win10/compose-winboat.yml) 609 / 20 (32-bit 15, half precision 1, Direct2D path geometry and layers 2+2);
          with kvm 0027 on WinBoat's stack 635 / 5 (half precision 1, Direct2D 2+2)
        - [x] Direct2D path geometry and layers lost every curved part on Windows 10 with WinBoat's host (2026-10-22,
              58118, Mesa 0030): with target-independent rasterization there, Windows 10's Direct2D fills paths by
              XORing SV_Coverage masks into an R16_UINT target (ForcedSampleCount 16, logic ops) and draws curves as
              tessellated patches (5 control points: the Bezier's and a fan center), its hull shader reading the vertex
              shader's SV_Position and SV_ClipDistance through plain dcl_input. Those were declared GENERIC, which GL
              doesn't link to gl_Position/gl_ClipDistance: degenerate patches. Hull/domain shader inputs now follow
              the input signature (POSITION, CLIPDIST by slot). Found by hooking the D3D11 device (shaders, states)
              under Direct2D (d2dprobe), then rebuilding its pipeline in dx-test step by step (new tests: TIR 16,
              domain shader clip distances, TIR with tessellation, XORed coverage masks, the curve patch with and
              without the coverage pass), and a debug virglrenderer (VREND_DEBUG=shader) for the GLSL. Windows 10 on
              WinBoat's host: 651 passed / 1 failed (half precision); Windows 11 675 / 0
        - [x] Half precision conversions (2026-10-22, dx-test): all 17 were f32tof16 of values that become half
              denormals (1e-7 and multiples): Windows 10's WARP rounds them to nearest, Windows 11's WARP truncates, as
              the GPU does (halfprobe: identical to Windows 11's WARP). Not the driver's: the test allows one unit
        - Windows 10 results (58118 user-mode drivers, kvm 0027): WinBoat's host stack 652 passed / 0 failed; stock host
          (plain QEMU 11.1, virglrenderer 1.3.0) 609 / 4 (the ExecuteIndirect state changes: a host patch, as on
          Windows 11)
        - dx-test there: SmartScreen holds new unsigned exes at an invisible prompt (the scheduled task never starts):
          turned off in that VM
  - [~] Windows 8.1 (2026-10-22): 8.1 Enterprise evaluation by dockur (VERSION=8e, ~/winboat-gpu-spike/win81, OpenSSH
        from oem/ (8.1 has no capability), test mode), booted by plain-qemu.sh. The driver runs (the 58288 build's
        8.1 package): dx-test 280 passed / 9 failed (D3D10/11 at feature level 11_0 with tessellation and compute,
        Direct2D, OpenGL 4.6, D3D11 video decoding pass; open items above), D3D12/DirectML/Vulkan/flip-discard
        swap chains optional there (dx-test: before Windows 10). Spike regression with these changes identical
        (regress-78). With Mesa 0032 (the 58381 build's 8.1 package): 476 passed / 4 failed (8.1's WARP, above);
        Direct3D 9, 9Ex, GDI interop and DXVA2 pass (64/32-bit); spike regression identical (regress-79, gpu.79 =
        58429)
