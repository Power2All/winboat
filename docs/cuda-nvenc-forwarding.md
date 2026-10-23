# CUDA, TensorRT and NVENC in the Windows VM: forwarding to the host (feasibility, stage F)

Written 2026-10-16 (roadmap stage F, asked 2026-10-14 and again 2026-10-15). Nothing of this is built: this page is
the go/no-go input.

## What programs use, and what the virtual GPU gives them today

| What | Used by | Through the virtual GPU today |
|---|---|---|
| DirectML (AI inference on D3D12) | Topaz Video AI on non-NVIDIA GPUs, ONNX Runtime, Windows ML | **Works** (stage E: GEMM/convolution FP32+FP16 on the host GPU) |
| OpenCL | ffmpeg filters, image tools | **Works** with Microsoft's compatibility pack (OpenCLOn12 on our D3D12) |
| Vulkan compute | ffmpeg's Vulkan filters, ncnn-based upscalers (Real-ESRGAN, waifu2x, RIFE builds) | **Works** (Venus) |
| Video decoding (DXVA, D3D11) | players, browsers, ffmpeg `d3d11va` | **Works**: H.264, HEVC 8/10-bit, VP9 8/10-bit, AV1 8/10-bit, MPEG-2 on NVDEC (stage E) |
| CUDA (+ cuDNN, TensorRT) | Topaz on NVIDIA, PyTorch, ffmpeg `cuda`/`scale_cuda` | **No** |
| NVENC / NVDEC through NVIDIA's APIs | ffmpeg `*_nvenc`, `*_cuvid`, OBS, Topaz export | **No** |

CUDA and NVENC are not graphics APIs a virtual GPU driver can implement: they are NVIDIA's own closed libraries
(`nvcuda.dll`, `nvEncodeAPI64.dll`, TensorRT on top of CUDA) that talk to NVIDIA's own kernel driver on an NVIDIA
GPU. A Windows VM gets them only with the GPU itself (passthrough, roadmap phase 1/2) - or by forwarding the calls.

## Forwarding ("API remoting")

The guest gets its own `nvcuda.dll` (and `nvEncodeAPI64.dll`, `nvml.dll`) that sends every call to a process on the
Linux host, which makes it on NVIDIA's real `libcuda.so` / `libnvidia-encode.so`. This is how rCUDA, gVirtuS,
Cricket and similar projects work for Linux guests; no Windows-guest implementation exists.

### What it takes

1. **The CUDA driver API** (`cu*`, ~500 entry points): mostly mechanical marshalling. Pointers to device memory are
   just numbers to pass back and forth; host memory (cuMemcpyHtoD/DtoH, pinned memory, managed memory) must cross
   the VM boundary.
2. **The CUDA runtime's private tables**: `cudart` (statically linked into most programs, including Topaz and
   PyTorch) calls `cuGetExportTable` for undocumented function tables identified by UUIDs. Every forwarded driver
   must reproduce them - ZLUDA (CUDA on AMD) had to, by reverse engineering. This is the hardest and legally least
   comfortable part.
3. **Kernels**: programs ship PTX or cubins for the host's GPU architecture; they run unchanged on the host (same
   NVIDIA GPU), so no translation is needed - an advantage over ZLUDA's case.
4. **cuDNN / TensorRT / cuBLAS**: libraries on top of the driver API. The Windows builds programs bundle (Topaz
   ships its own TensorRT) call the forwarded `nvcuda.dll` like any other client, as long as (2) is complete.
   TensorRT's engines are built for the exact GPU and driver: built on the host, they'd match.
5. **NVENC** (`NvEncodeAPICreateInstance` fills a table of ~40 functions): forwardable the same way when the input
   frames are CUDA memory (`NV_ENC_DEVICE_TYPE_CUDA`). Programs that hand NVENC D3D11 textures
   (`NV_ENC_DEVICE_TYPE_DIRECTX`, e.g. OBS) would need our D3D11 textures exported to CUDA on the host: possible
   (virgl/Venus resources are host GL/Vulkan objects; CUDA imports Vulkan/GL memory), but a project of its own.
6. **Detection**: programs look for an NVIDIA GPU by DXGI vendor ID (ours is Red Hat's, 0x1AF4), NVML
   (`nvml.dll`: also to forward), or simply `cuInit` success. Topaz decides by CUDA device presence and its own
   checks; some programs would still refuse a non-NVIDIA DXGI adapter.
7. **The channel**: a transport with copies in both directions. virtio-vsock (QEMU: `vhost-vsock-pci`) works without
   new devices but copies every byte through sockets (a few GB/s); a shared-memory window (the hostmem BAR Venus
   uses, or ivshmem) avoids the copy for bulk transfers. A 4K frame is ~25 MB (NV12 ~12 MB): at 60 fps that is
   0.7-1.5 GB/s each way - fine for vsock on paper, tight with the latencies of many small calls (CUDA programs make
   thousands of calls per frame; each round trip through the host costs ~50-100 us, so batching asynchronous calls
   matters).

### Licensing

- Nothing NVIDIA-made would be shipped in the guest: the guest DLLs would be our own code implementing published
  API names. NVIDIA's driver stays on the host where the user installed it.
- NVIDIA's CUDA EULA (toolkit, since 11.6) forbids reverse engineering and using SDK output on non-NVIDIA platforms
  through translation layers. Forwarding runs on NVIDIA hardware, which is not the case that clause targets, but
  reproducing the private `cuGetExportTable` interfaces (point 2) is reverse engineering of NVIDIA's runtime. That
  needs a decision by the project, ideally with legal advice - not something to settle in code.

### Effort (rough)

| Part | Size |
|---|---|
| Transport (vsock first), host server, guest DLL skeleton, call batching | 2-3 weeks |
| CUDA driver API marshalling (generated from cuda.h) | 2-4 weeks |
| Private export tables for cudart (as ZLUDA did), until PyTorch/Topaz start | 4-8+ weeks, open-ended |
| NVML (detection) | 1 week |
| NVENC with CUDA input | 2 weeks |
| NVENC with D3D11 input (resource export to CUDA on the host) | 3-4 weeks |
| Testing with real programs (Topaz trial, ffmpeg, PyTorch), per NVIDIA driver version | continuous |

## Recommendation

**Not now.** What programs need from a GPU without CUDA is covered or close: DirectML (Topaz's non-NVIDIA path,
working), OpenCL and Vulkan compute (working), video decoding (working). CUDA forwarding is months of work whose
hardest part sits on reverse-engineered private interfaces, and it only helps NVIDIA hosts.

The cheaper middle ground, if hardware *encoding* turns out to matter (Topaz exports, OBS): **NVENC forwarding with
CUDA-memory input only** - small, public API, no private tables - but programs only use it when CUDA works too
(ffmpeg's `h264_nvenc` needs a CUDA context for its default path), so even this depends on part of the CUDA work.
Hardware encoding on an NVIDIA host has no route yet: the host's VA-API (nvidia-vaapi-driver) decodes only, and
Vulkan Video (which NVIDIA's driver encodes with) isn't in the Venus protocol on either side. Adding it there - video
sessions, bitstream buffers and the encode commands generated into the protocol, with vkr implementing the objects -
is the standards-based alternative to NVENC forwarding (then D3D12 video encoding and a Media Foundation encoder on
top in the guest); NVENC does the work on the host either way.

Go/no-go: **go** (the user, 2026-10-16), roadmap stage F. This recommendation stays as the record of the trade-off.

## Feasibility spike (2026-10-17)

What the programs in the VM actually need, looked up in their binaries (Topaz Video AI 7.1.5, its ffmpeg 8):

- **Topaz's only CUDA user is TensorRT** (`nvinfer_10.dll`, 325 MB). It imports nothing from CUDA: the CUDA runtime is
  linked in statically and loads `nvcuda.dll` at run time. There is no separate cudart, cuDNN, cuBLAS or ONNX Runtime
  CUDA provider in the install. So a forwarding `nvcuda.dll` is the single entry point Topaz's GPU path would need.
- **It names 517 driver API functions** (92 of them versioned `_v2`/`_v3`...: the runtime's whole table, fetched
  through `cuGetProcAddress`): memory 113, graphs 104, streams 59, D3D interop 47, devices 27, contexts 24, plus
  modules/libraries, launches, events, arrays, linker. **And `cuGetExportTable`**: the statically linked runtime
  needs NVIDIA's private interfaces, which is the reverse-engineered part (point 2, Licensing). Without them the
  runtime fails at its first call, so public-API forwarding alone gets Topaz nowhere.
- **Backend choice**: Topaz's engine (`videoai.dll`: `DeviceSelector::preferredBackend`, "CUDA ID", `disable_trt_ov`)
  enumerates CUDA devices for TensorRT; today it runs ONNX Runtime + DirectML on our GPU (its `-ox` models). The
  DXGI adapter now presents itself as AMD (for the AMF encoders); a CUDA device next to it would be a second
  identity, and whether Topaz then also wants an NVIDIA DXGI vendor ID is open.
- **ffmpeg** needs only the public driver API (its `dynlink_cuda.h`: ~40 functions) plus `nvcuvid`/`nvEncodeAPI`.
  But hardware decoding and encoding already reach NVDEC/NVENC through D3D11/D3D12/Vulkan and the AMF runtime, so
  public-API forwarding would add `*_nvenc`, `*_cuvid` and the CUDA filters, and little that isn't covered.

So the decision is the private tables: with them (and the legal call they need), TensorRT for Topaz and PyTorch
come in reach, months of work; without them, CUDA forwarding has little left to offer.
