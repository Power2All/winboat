# Games and applications on the WinBoat GPU driver

What programs get from the paravirtualized GPU, what they need that it can't give, and how each Windows version
compares. Checked against game and application requirements (Microsoft's documentation, vendors' system requirements,
DXVK/vkd3d-proton's issue trackers) on 2026-10-24; measured with dx-test and Topaz Video AI's trial on the test VMs.

## What the driver offers (Windows 11, WinBoat's host, RTX 3080)

- Direct3D 9 (shader model 3, 9Ex, DXVA2 decoding), 10/10.1, 11 up to feature level 11_1 with 11.1-11.4's options:
  tiled resources tier 2, min/max filtering, conservative rasterization tier 1, rasterizer ordered views, typed UAV
  loads, shared NV12 textures (decoder output too)
- Direct3D 12 at feature level 12_2 (DirectX 12 Ultimate): shader model 6.8, resource binding tier 3, DXR 1.1,
  mesh shaders, VRS tier 2, sampler feedback, enhanced barriers, ExecuteIndirect tier 1.1, work graphs, 64-bit atomics
  on textures and buffers (what Unreal Engine 5's Nanite and virtual shadow maps need)
- Vulkan 1.4 (ray tracing, mesh shaders, Vulkan Video), OpenGL 4.6
- DirectML (feature level 6.2: ONNX Runtime's DirectML provider, Topaz Video AI), OpenCL 3.0 through Microsoft's
  OpenCL compatibility pack (OpenCL 1.2's features, images; no extensions such as GL/D3D11 sharing)
- video: decoding H.264, HEVC (Main, Main 10, range extensions), VP9, AV1, MPEG-2, VC-1 through D3D11/DXVA2/D3D12;
  encoding H.264/HEVC through Media Foundation, D3D12 and an AMF-compatible runtime (amfrt64.dll)
- dedicated video memory: the host GPU's (WinBoat reads it on the host, `nvidia-smi` or amdgpu's sysfs, and sets the
  driver's `GpuMemoryLimitMB` to it: 16 GB on an RTX 3080 16 GB), which programs judge the GPU by (Topaz Video AI
  wants 6-8 GB, DaVinci Resolve 4 GB, newer games 8 GB). It is also how much the VM's GPU resources may hold. The
  host's own programs share that memory: when the host GPU is full, programs get "out of memory" (Direct3D 12's
  E_OUTOFMEMORY), as on a full GPU, and can lower their use - Topaz Video AI: Preferences, its GPU memory use. Without
  WinBoat (the standalone setup): 8 GB, or set `GpuMemoryLimitMB` (MB, DWORD) in the GPU's driver key and restart

Missing, optional in Direct3D and asked for by no known game: stencil reference from pixel shaders and standard
swizzle layouts (NVIDIA's own drivers have neither), typed UAVs of B8G8R8A8, R32G32B32_FLOAT render targets.

## Benchmarks

GravityMark 1.89 (the user's WinBoat VM, Windows 11, RTX 3080 Laptop host, 1600x900 window, 200,000 asteroids,
gpu.103: image 6.05-32, driver 58536, 2026-10-26; no viewer attached):

| API | Score | FPS |
|---|---|---|
| Vulkan | 37,859 | 226.7 |
| Direct3D 12 | 36,791 | 220.3 |
| OpenGL | 22,296 | 133.5 |
| Direct3D 11 | 7,238 | 43.3 |

Vulkan, Direct3D 12 and OpenGL render correctly (checked against each other on screen). Direct3D 11 still renders
without sunlight (flat, over-exposed asteroids; see `docs/gpu-roadmap-todo.md`) and is slow. Earlier figures
(gpu.101) were wrong: Direct3D 11 drew nothing after a host error and Direct3D 12 sampled empty textures, and frames
like that still count.

## What no driver of this kind can give

- **Anti-cheat.** Riot Vanguard (Valorant, League of Legends), many Easy Anti-Cheat and BattlEye titles (Fortnite,
  Apex Legends, PUBG), EA Javelin and others refuse to run in virtual machines, whatever the GPU.
- **NVIDIA-only interfaces.** The VM's GPU isn't NVIDIA-branded and has no CUDA, NVENC, NVAPI, TensorRT or DLSS:
  - Blender's Cycles renders on the CPU (on Windows it renders on GPUs through CUDA, OptiX, HIP or oneAPI only); the
    viewport (Eevee, Workbench: OpenGL 4.3) runs on the GPU
  - Topaz Video AI's CUDA-only models (Starlight) aren't offered; the others run on DirectML
  - games take FSR or XeSS instead of DLSS
- **Vendor checks.** OBS lists its AMF encoders only on AMD-branded GPUs (it checks the vendor ID), so its standard
  outputs offer x264 only. Workaround: Settings > Output > Advanced > Recording, type "Custom Output (FFmpeg)", video
  encoder `h264_amf` or `hevc_amf` (FFmpeg's AMF encoders use the driver's AMF runtime). HandBrake has no such check:
  its AMD VCN encoders work.
- **Adobe Premiere Pro 25.6 and later** want OpenCL 2.0 (the compatibility pack is 1.2-level): GPU effects off.
- **DaVinci Resolve (free)** decodes and encodes H.264/HEVC on the CPU on Windows by its own rules (Studio only);
  its OpenCL processing has the compatibility pack.

## Windows versions

| | Windows 11 | Windows 10 | Windows 8.1 | Windows 7 |
|---|---|---|---|---|
| Direct3D 9, 10, 11 | 11_1 | 11_1 | 11_0 | 11_0 |
| Direct3D 12 | 12_2, SM 6.8 | 12_1, SM 6.5 | - | - |
| Vulkan, DirectML, OpenCL | yes | yes | - | - |
| Dedicated video memory (DXGI) | the host GPU's | none reported | none reported | none reported |
| Topaz Video AI | GPU (DirectML) | CPU (OpenVINO) | doesn't start | installer refuses |

(Windows 11 and 10 on WinBoat's host; 8.1 and 7 measured on a stock QEMU host, where feature level 11_1 and OpenGL
above 4.2 need WinBoat's host patches.)

Topaz Video AI and every other program that picks its GPU by dedicated video memory treats Windows 10's as unusable:
Windows 10's video memory manager crashes at boot with the memory segment that reports it (tried two layouts), so the
driver reports it on Windows 11 only. Direct3D 12, DirectML and OpenCL themselves work on Windows 10. Topaz Video AI
needs Windows 10 or 11 (DirectML is Windows 10's); 8.1 and 7 have no Direct3D 12, so no DirectML, OpenCL compatibility
pack, Vulkan or Direct3D 12 games either.

## Other hosts

What the driver reports comes from the host GPU's capabilities: a host without a feature makes it "not supported" in
Windows, as on a real GPU without it, instead of failing. Nothing checks for an NVIDIA model; AMD and Intel hosts run
Mesa's GL and Vulkan drivers (radeonsi/RADV, iris/ANV), which virglrenderer and Venus are mostly developed on. Not
tested on such a host yet: measured instead with the host GPU hidden from QEMU, so that Mesa's llvmpipe renders (GL 4.6
without NVIDIA's extensions, no Vulkan), and llvmpipe made an older GPU (GL 3.3, 2010's extensions) - see
`reports/`. That found two bugs, both fixed in 58789: Mesa's GL refuses a draw to several render targets after a
dual-source blend (Direct2D's ClearType text uses one), which broke the whole GPU context; and a host that doesn't
report its video memory left Direct3D 9 without any device. On an AMD or Intel host, expect: D3D11 tiled resources
(and min/max filtering, ClearView on depth formats) reported missing (they use an NVIDIA GL extension; D3D12's go
through Vulkan), conservative rasterization and rasterizer ordered views as far as the host's GL has them, stencil
reference from pixel shaders possibly gained (their GL has GL_ARB_shader_stencil_export), and HEVC 4:4:4 decoding
missing (the host side packs NVIDIA's plane layout only). Stock QEMU and virglrenderer (no WinBoat patches) lack video,
feature level 11_1 and several 11.x options, and through Venus Direct3D 12's ExecuteIndirect with state changes
(standalone-install.md lists them). Without Venus there's no Direct3D 12 or Vulkan.
