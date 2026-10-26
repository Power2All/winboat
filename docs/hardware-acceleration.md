# Hardware Acceleration (GPU) for the Windows VM

Work-in-progress notes for the `hardware-acceleration` branch.

| Phase | What | Vendors | Status |
|---|---|---|---|
| 1 | Whole-GPU passthrough (VFIO) + RDP GPU policies | NVIDIA, AMD, Intel | **On hold, implemented but UNTESTED** |
| 2 | Splitting one GPU between host and VM (Intel GVT-g / SR-IOV) | Intel only | **On hold**, needs a two-GPU test machine |
| 3 | Paravirtualized GPU: share the host GPU through virtio-gpu + a Windows 3D driver | NVIDIA, AMD, Intel | **In progress**: spike works, D3D10/11 + OpenGL 4.2 accelerated in RDP sessions |

## Phase 1: GPU passthrough (on hold, untested)

> ⚠️ **This code has never run against real hardware.** It type-checks (`vue-tsc`) and the Guest Server
> builds, but no GPU has been passed through with it yet. Do not merge or ship it before it's tested on a
> machine with a working IOMMU and a second GPU.

The code is **not in the working tree**; it's kept as a patch outside the repo at
`~/Coding/winboat-hwaccel-patches/phase1-gpu-passthrough.patch` (made against `b9a2df2`). Restore it with
`git apply ~/Coding/winboat-hwaccel-patches/phase1-gpu-passthrough.patch`.

Why it's on hold: the only development machine so far is a laptop where IOMMU makes the system unstable,
and its only visible GPU (NVIDIA, discrete/MUX mode) drives the Linux display.

What's implemented:

- `src/renderer/lib/gpu.ts`: host GPU detection via sysfs (vendor, IOMMU group, boot VGA, companion
  functions), a `pkexec` root helper that moves devices between their host driver and `vfio-pci` (refuses
  while a host process has the GPU open), and the compose changes (`-device vfio-pci,host=...` in
  `ARGUMENTS`, `/dev/vfio/*` devices, `memlock` ulimit).
- `src/renderer/components/GPUPassthrough.vue`: Settings → Devices card (experimental features only).
- `src/renderer/lib/winboat.ts`: claims the GPU before the container starts, optionally returns it to Linux
  on stop, syncs the RDP GPU policies with the guest, adds `/gfx:AVC444` to FreeRDP when supported.
- Guest Server: `GET /gpu` (display adapters Windows sees) and `POST /gpu/rdp-acceleration` (sets
  `bEnumerateHWBeforeSW`, `AVC444ModePreferred`, `AVCHardwareEncodePreferred`).

Known limitations: Docker only (no Podman); incompatible with Auto Start Container; laptop NVIDIA GPUs
may need a vBIOS `romfile` (not supported yet); the GPU sits on the root bus rather than behind a PCIe root
port.

How to test it: a host with IOMMU enabled (`intel_iommu=on iommu=pt` or `iommu=pt` on AMD) and two GPUs,
where the one to pass through is not the boot/display GPU.

## Phase 2: GPU splitting (on hold)

Intel only (consumer NVIDIA and AMD cards don't support it). Needs a machine where the Intel iGPU and the
host display GPU can be active at the same time; to be revisited on the two-GPU desktop.

## Phase 3: Paravirtualized GPU (virtio-gpu 3D)

Windows talks to a virtual `virtio-gpu` device; the host renders the commands on its own GPU through
virglrenderer. The host keeps its GPU and any vendor works.

### Building blocks and their state

- **Host / QEMU**: done upstream. `dockur/windows` (6.05) supports `GPU=Y`, which switches to
  `-display egl-headless,rendernode=...` with `virtio-vga-gl`, and auto-detects Intel, AMD and NVIDIA
  render nodes. NVIDIA needs the NVIDIA Container Toolkit so the proprietary EGL/GBM libraries are
  available inside the container, and `nvidia-drm.modeset=1`.
- **Windows kernel + user-mode driver**: the missing piece. Best starting point is **viogpu3d**:
  - Kernel driver: [virtio-win/kvm-guest-drivers-windows#943](https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943)
    (open since 2023, rebased Sept 2025, not merged)
  - User-mode (Mesa, OpenGL via WGL + Direct3D 10 via `d3d10umd`): [mesa!24223](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/24223) (draft)
  - Host side: [virglrenderer!1185](https://gitlab.freedesktop.org/virgl/virglrenderer/-/merge_requests/1185) (merged 2023)
  - Reported problems: crashes, rendering glitches (WinUI3), black Electron windows, no preemption, poor
    performance and high CPU use in community tests.
- **Driver signing**: test-signing is enough for development. Unattended dockur Windows installs boot plain
  UEFI without Secure Boot and with device encryption disabled, so `bcdedit /set testsigning on` works.
  A release would need EV + Microsoft attestation signing.

### Plan

1. **Spike (disposable VM, never the real WinBoat VM)**
   1. Host: install the NVIDIA Container Toolkit; start the spike VM below; confirm the container log says
      `Hardware rendering enabled on NVIDIA render node`.
   2. Build viogpu3d (kernel driver + Mesa) from source, ourselves; don't use prebuilt zips from issue
      comments.
   3. Enable test signing, install the driver, test OpenGL (wglgears), D3D10/11 samples.
   4. **Key unknown:** does an RDP session render on the virtio-gpu adapter with `bEnumerateHWBeforeSW`
      (the Phase 1 guest endpoint)? WinBoat's apps go through RDP, so this decides whether Phase 3 helps
      WinBoat at all.
2. **Reproducible driver build**: e.g. a GitHub Actions workflow on the fork (Windows runner, EWDK + Meson)
   producing a test-signed driver package.
3. **WinBoat integration**: "Virtual GPU" option that sets `GPU=Y` / `virtio-vga-gl`, adds the NVIDIA
   device request on NVIDIA hosts, detects the toolkit in the prerequisites, and installs the driver + test
   certificate through the OEM payload / Guest Server.
4. **Driver hardening**: stability, preemption, performance. Longer term: Vulkan via Venus + DXVK/vkd3d-proton
   for D3D9–12.

### Spike log

- **2026-10-11, host side works on NVIDIA (RTX 3080 Mobile, driver 615.71, Arch/CachyOS).** With the NVIDIA
  Container Toolkit and `GPU=Y`, QEMU runs `-display egl-headless,rendernode=/dev/dri/renderD128 -device virtio-vga-gl`
  and shows up in `nvidia-smi` as a graphics process.
  - **Gotcha (affects WinBoat integration):** on non-Debian hosts the toolkit mounts the NVIDIA libraries at the
    host's paths (`/usr/lib/gbm/nvidia-drm_gbm.so` on Arch, `/usr/lib64/...` on Fedora), but the container's Debian
    Mesa only searches `/usr/lib/x86_64-linux-gnu/gbm`. QEMU then dies with `gbm_create_device failed` /
    `nouveau: driver missing`. Fix: `GBM_BACKENDS_PATH=/usr/lib/x86_64-linux-gnu/gbm:/usr/lib/gbm:/usr/lib64/gbm`.
    Worth reporting to dockur.
- **2026-10-11, viogpu3d built, installed and tested in the spike VM** (Windows 11 Pro 26H2, build 26300).
  Reports: `~/winboat-gpu-spike/reports/console-20261011-183704` and `rdp-20261011-184917`.
  - **The key unknown is answered: yes.** A RemoteApp session (how WinBoat shows apps) gets the virtio-gpu as its
    hardware adapter, with `bEnumerateHWBeforeSW` set. The session's display is still the Microsoft Remote Display
    Adapter (RDP encodes the frames), but apps render on the host GPU.

  | Check | Before (software) | viogpu3d, console | viogpu3d, RDP RemoteApp |
  |---|---|---|---|
  | D3D11 device on the GPU | ❌ none (Basic Render) | ✅ feature level 10_0 | ✅ feature level 10_0 |
  | D3D11 draw from a vertex buffer | (WARP only) | ✅ | ✅ |
  | D3D11 draw generated from `SV_VertexID` | (WARP only) | ❌ → ✅ with our Mesa patch | ❌ → ✅ with our Mesa patch |
  | D3D11 fill-rate benchmark | 97–117 fps (WARP) | 1660 fps (14.5x) | 1695 fps (13.6x) |
  | D3D11 present to the screen (flip + blt) | ✅ | ✅ | ✅ |
  | OpenGL | GDI Generic 1.1 | ✅ 4.2 compat / 4.0 core on the RTX 3080 | ✅ same |
  | OpenGL fill-rate benchmark | | 1311–1700 fps (11–15x) | 1524 fps (12.2x) |
  | OpenGL `SwapBuffers` reaches the screen | ✅ | ❌ → ✅ with our Mesa patch | ❌ → ✅ with our Mesa patch |
  | Compute shaders | | ⏭️ not at feature level 10_0 | ⏭️ |
  | Direct3D 9 | ✅ (software) | ❌ → ✅ with our D3D9 driver | ❌ → ✅ with our D3D9 driver |
  | Direct3D 9 fill-rate benchmark | | | 1544 fps (13.5x WARP), 32-bit 1576 fps (14.1x) |
  | Direct3D 12 / Vulkan / video decode | ❌ | ❌ (Vulkan: ✅ since 2026-10-14, see below) | ❌ |
  | 32-bit apps (D3D9, D3D11, OpenGL) | software | ❌ → ✅ with the 32-bit build | ❌ → ✅ with the 32-bit build |

  - **OpenGL output never reached the window** (fixed by `tools/gpu-spike/patches/mesa/0001-*.patch`; very likely
    the "black Electron windows" report). Three separate bugs in the WGL target:
    1. The DXGI swapchain framebuffer created its D3D11 device with `D3D11_CREATE_DEVICE_DEBUG`, which fails without
       the optional Graphics Tools, so there was no swapchain and every `SwapBuffers` did nothing.
    2. In an RDP session DXGI lists the virtio-gpu twice with different LUIDs; the swapchain's device used the
       default one, whose shared textures virgl (opened on the window DC's adapter) can't import. Now the device is
       created on the window's adapter.
    3. The GDI path (`PFD_SUPPORT_GDI` formats, or when the DXGI device can't be created) presented through
       `gdikmt_d3dkmt_present()`, a stub. It now reads the frame back and draws it with `StretchDIBits`. This path
       is **not covered by the tests yet** (the test's double-buffered format uses the swapchain).

    After the patch, `SwapBuffers` shows the frame the right way up in the console (~3000 swaps/s) and over RDP
    (~2950 swaps/s).
  - **Direct3D 9 didn't work**: the D3D9 runtime calls the user-mode driver's `OpenAdapter` export, and
    `viogpu_d3d10.dll` only has `OpenAdapter10`/`OpenAdapter10_2` (`D3DERR_NOTAVAILABLE`). Before the driver,
    Microsoft's `d3d10warp.dll` provided it (software). Forwarding `OpenAdapter` to WARP is not an option: WARP
    creates allocations and command buffers in its own format, and the viogpu3d kernel driver only accepts virgl
    resources and virgl command streams.
  - **Fixed with our own D3D9 driver**, `viogpu_d3d9.dll` / `viogpu_d3d9_x86.dll` (Mesa
    `patches/mesa/0003-*`, packaged by `patches/kvm-guest-drivers-windows/0002-*` as `UserModeDriverName[0]`):
    a WDDM D3D9 user-mode driver (the D3DDDI interface) in a new Mesa frontend, `src/gallium/frontends/d3d9umd`.
    Microsoft's runtime keeps the API, swapchains and presentation; the driver maps the DDI calls onto a headless
    Gallium Nine device (Nine translates D3D9 shaders and fixed function to Gallium) on the same virgl winsys as the
    other drivers. Nine needed an MSVC port and a few changes for life without a swapchain. Over RDP RemoteApp every
    D3D9 check passes for 64- and 32-bit apps (HAL device with hardware vertex processing and shader model 3.0,
    clear, pre-transformed and transformed fixed function, textures, vs/ps 2.0, presenting to the screen). Bugs
    found on the way:
    1. The runtime owns system memory resources and writes them directly, only telling the driver through
       `NotifyOnly` lock/unlock calls (and `LockAsync`/`UnlockAsync` for the app's own locks). Nine must re-upload
       on those, or every draw from a reused buffer (all `DrawPrimitiveUP` calls go through one) uses stale vertices.
    2. Nine ignored the memory it was given for container-less system memory surfaces and allocated its own, so
       `GetRenderTargetData` wrote where the app never looked.
    3. virgl turns a real buffer in constant buffer slot 0 into a UBO the host's shaders don't read: every shader
       constant was zero. Nine now has a `user_constbufs` cap to pass them as user data (as its fixed-function path
       already did).
    4. The runtime emulates fixed-function vertex processing itself (it hands the driver a generated `vs_2_0` and
       its constants, never `SetTransform`), so the shader constants path matters even for fixed-function apps.
  - **Direct3D 9 on the VM console reported no HAL** (`GetDeviceCaps(HAL)` failed before the runtime ever called
    `CreateDevice`). Found by stepping through `d3d9.dll` with `cdb` and Microsoft's public symbols: when the adapter
    drives the display, `GetDX8HALCaps` only keeps the driver's caps if `IsD3DHALSupported` accepts them, a long list
    of minimum requirements for a shader model 2/3 HAL (RDP sessions skip that check, which is why RDP worked).
    Ours failed two of them, both DDI-only details Gallium Nine (an API implementation) doesn't know about:
    `D3DPMISCCAPS_FOGINFVF` (0x2000, "vertices may carry a separate fog value") must be in `PrimitiveMiscCaps`, and
    a format's multisample mask must include `NONMASKABLE` whenever it lists any sample counts. Fixed in the D3D9
    driver (Mesa `0003`). (Marking `A8R8G8B8` as a display format, an earlier guess, is rejected by the runtime and
    broke `CheckDeviceType` over RDP.)
  - **32-bit apps got nothing** (`run-in-vm.sh rdp32`, before the 32-bit build): no D3D10/11 at all
    (`DXGI_ERROR_UNSUPPORTED`, where they had software rendering before), OpenGL 1.1 software only, no D3D9.
    **Fixed**: Mesa is now built a second time for x86 (`viogpu_d3d10_x86.dll`, `viogpu_wgl_x86.dll`, zlib linked
    statically in both) and registered for WOW64 (`UserModeDriverNameWow`, `OpenGLDriverNameWow`), via
    `patches/kvm-guest-drivers-windows/0001-*` and Mesa `0002`. 32-bit results now match 64-bit in the console and
    over RDP: D3D11 FL 10_0 at ~14.5–16x WARP, OpenGL 4.2 at ~15x, OpenGL presents correctly.
  - **Security bug in the kernel driver**: the `VIOGPU_GET_CAPS` escape copied into a caller-supplied address without
    `ProbeForWrite`, so any process that can open the adapter could make the kernel write to kernel memory. Fixed in
    the same patch (the address is also a `UINT64` now, so 32-bit callers send the same layout).
  - The INF sets `EnablePreemption=0` (GPU preemption off), still to revisit before a release. It also set
    `TdrDebugMode=1` (GPU timeouts ignored, a hung GPU never recovered); since 2026-10-17 it sets `TdrDebugMode=2`
    and `TdrDelay=10`, and the driver resets from a timeout (gpu-roadmap-todo.md, stage E).
  - **Shaders that index an array drew nothing** (the `SV_VertexID` check: a constant array indexed by the vertex
    id). The D3D10/11 driver turned D3D's indexable temps (`x#[]`) into plain temporaries addressed relatively, which
    virgl's host-side translation to GLSL doesn't handle. Fixed by `patches/mesa/0004-*`: they are declared as TGSI
    temporary arrays, as the OpenGL frontend does. `SV_VertexID` itself was fine (`VERTEXID_NOBASE` works on virgl).
    Now every D3D11 check passes, 64- and 32-bit, console and RDP; the DWM and the shell use the same driver.
  - Feature level 10_0 means no compute shaders and no D3D11-only apps (most modern games need 11_0). Since
    2026-10-14 the driver reports 11_0 (the native route's stage 4, below).
  - **Vulkan: Venus, in stages** (2026-10-12; stage 1 then, stages 2-4 done 2026-10-14: see "Vulkan (roadmap stage
    B)" below). The only route to hardware Vulkan here is Venus (Vulkan commands over
    virtio-gpu, executed by the host's Vulkan driver). Every layer needs work:
    1. **Host (done).** dockur's Debian virglrenderer is built without Venus (no `virgl_render_server`).
       `tools/gpu-spike/venus/` builds `winboat-windows-venus`: dockur 6.05 with virglrenderer 1.3.0 (the same
       version, so QEMU sees the same ABI) built with Venus and the Vulkan loader. QEMU 11.1 already supports it:
       dockur passes a custom `VGA` value as the device string, so `VGA="virtio-vga-gl,venus=on,blob=on,hostmem=4G"`
       is enough (`setup.sh` does this with `VENUS=1`). Checked on the host without a VM (`docker build --target
       test`: Mesa's Venus driver through `virgl_test_server --venus` reports `Virtio-GPU Venus (NVIDIA GeForce RTX
       3080 Laptop GPU)`), then in the VM: the kernel driver now lists the `venus` capset (gpucheck reports the
       capsets), and every other check still passes. **For WinBoat this means its own container image** (or Venus in
       dockur's package upstream).
    2. **Kernel driver (next).** Venus needs blob resources (`RESOURCE_CREATE_BLOB` of host memory, mappable) for its
       command rings and host-visible device memory, mapped into the app through the `hostmem` window (a new memory
       segment over that PCI BAR), plus a replacement for the DRM timeline sync objects it uses on Linux. Context
       init with a capset, capset queries and command submission already exist.
    3. **Mesa:** a Windows backend for the Venus driver (`vn_renderer`), talking to the kernel driver through D3DKMT
       instead of DRM ioctls.
    4. **Packaging:** `VulkanDriverName` / `VulkanDriverNameWow` and the ICD manifests in the INF.
  - Build fixes needed: run `win_flex` serially (parallel runs corrupt each other's output), build the Win10
    configuration (the Win11 one doesn't build the VirtioLib it links), point the package at `z-1.dll` (Meson's zlib
    name, which `viogpu_wgl.dll` imports), add the test certificate with `certutil` (`Import-Certificate` fails as
    SYSTEM).

### WinBoat integration (2026-10-12, local testing)

A "Virtual GPU (3D acceleration)" card under Experimental features. Two versions with the same design:

- **On `main`** (this branch): kept for an eventual upstream PR; backup patch in
  `~/Coding/winboat-hwaccel-patches/phase3-virtual-gpu-main.patch`.
- **On 0.9.2** (worktree `~/Coding/winboat-0.9.2-gpu`, branch `hardware-acceleration-0.9.2` from `v0.9.2`): for
  testing on an existing install. `main`'s Windows-side guest server is a newer, incompatible design (token auth, a
  separate updater) with no migration from 0.9.x VMs, so a `main` build can't manage a VM that 0.9.2 set up. Built as
  `dist/winboat-0.9.2-gpu.1-x86_64.AppImage`; the distinct version makes WinBoat push its guest server (which carries
  the driver) into the VM.

How it works:

- **Host** (`src/renderer/lib/virtualGpu.ts`, `components/VirtualGPU.vue`): detects render nodes, and on NVIDIA the
  Container Toolkit (and its CDI spec for Podman) and `nvidia-drm.modeset`. Turning it on edits the compose file:
  `GPU=Y`, `/dev/dri`, dockur 6.05 if older (6.03 ignores `GPU=Y` for Windows and on AMD CPUs), and on NVIDIA the
  toolkit device request (Docker `deploy`, Podman CDI `nvidia.com/gpu=all`) plus `GBM_BACKENDS_PATH`. Then it waits
  for Windows and the guest server update, installs the driver and restarts Windows. Turning it off removes the
  driver first, then the compose settings (the image stays on 6.05).
- **Guest** (`guest_server/gpu.go`, `scripts/gpu-driver.ps1`): `GET /gpu` (state) and `POST /gpu/driver`
  (install/uninstall, password-checked like `/update`). The driver package is bundled with the guest server
  (`drivers/viogpu3d`, ~17 MB zipped), exported from the spike build by `tools/gpu-spike/export-driver-package.sh`
  and kept out of git. Install signs it with a certificate created in that VM, trusts only that certificate and
  deletes its private key (no signing key is shipped), turns test signing on and sets `bEnumerateHWBeforeSW`.
  Everything it changes is recorded in `HKLM\SOFTWARE\WinBoat\VirtualGPU` and Uninstall reverts only that.
  Packages are told apart by the catalog's hash (rebuilds keep the INF version).
- **Tested on the spike VM** as SYSTEM through the real guest server: install, reboot, every GPU check passing
  (except Vulkan), uninstall falling back to the 2D driver with the previous settings restored, and the 0.9.2
  server's endpoints and password gate. **Not yet tested:** the WinBoat UI flow end to end, Podman, AMD/Intel hosts.

### RDP display smoothness (measured 2026-10-12, user's VM)

What you see goes through RDP, separate from the 3D work. Measured with the RemoteFX Graphics counters while a
1280x720 GDI animation (app paints 62 fps) and a 1080p30 H.264 video in Edge ran:

| Session | 2D output fps | Video output fps | Codecs |
|---|---|---|---|
| RemoteApp (WinBoat apps), defaults | 30 | 29-30 | progressive + ClearCodec, AVC refused |
| RemoteApp, GPU for RDP off (`bEnumerateHWBeforeSW=0`) | 24-30 | 29 | same |
| RemoteApp, 60 fps + AVC444 settings | 30 | 29 | same (server confirms with `AVC_DISABLED`) |
| RemoteApp, `hidef:off` | 40 | 30 | same |
| Full desktop ("Windows Desktop"), 60 fps + AVC444 settings | **62** | 30 (source rate) | AVC444 |

- No skipped frames anywhere; encoding takes 5-7 ms a frame and the VM's CPU stays at 10-15%. The limit is Windows'
  frame pacing, not resources.
- `bEnumerateHWBeforeSW` makes no measurable difference for 2D or video.
- Video is decoded in software (the driver has no video decode DDI). Edge plays 1080p30 at about half a core.
- Windows applies `DWMFRAMEINTERVAL=15` (after a reboot) and `AVC444ModePreferred` only to full desktop sessions.
  For RemoteApp it stays at 30 fps and refuses H.264, whatever FreeRDP offers; FreeRDP 3's defaults already offer
  AVC444. `/app:...,hidef:off` (legacy RemoteApp) reaches 40 fps, at the cost of the HiDef window handling.
- `gpu-driver.ps1` Install now sets both values (recorded and reverted like the INF's values).
- 2D (GDI) apps aren't GPU-accelerated: GDI renders on the CPU, and in RemoteApp sessions DWM composites on the CPU
  too (no GPU engine time, whether `bEnumerateHWBeforeSW` is on or off). That isn't the bottleneck: a full-window
  GDI animation paints 62 fps for about 35% of a core (DWM about 24%), twice what RemoteApp sends. Also tried,
  without effect: `/gfx:frame-ack:off` (31 fps), `/network:lan` (31 fps). Turning off the "advanced RemoteFX graphics
  for RemoteApp" policy (`fEnableRemoteFXAdvancedRemoteApp=0`) gives 34 fps, the same as `hidef:off`.
- Settings toggle "Faster App Window Updates" (`legacyRemoteApp` in the config) adds `hidef:off`. It has to come
  before `cmd:"..."` in `/app:`: FreeRDP ignores sub-options after the quoted value.

### Low-latency display over SPICE GL (prototype, 2026-10-12, spike VM)

An alternative to RDP: QEMU hands the guest's scanout to a local SPICE client as a dmabuf (zero-copy, no capture or
encoding in Windows). Whole desktop only, no RemoteApp windows.

- **QEMU/SPICE:** dockur's `GPU=Y` always adds `egl-headless`, which can't share the console with SPICE GL ("The
  console already has an OpenGL context"), so the VM runs with `GPU=N`, `DISPLAY=none` and
  `ARGUMENTS="-device virtio-vga-gl... -spice unix=on,addr=/spice/spice.sock,disable-ticketing=on,gl=on,
  rendernode=/dev/dri/renderD128"`, the socket on a volume (QEMU creates it root-only: `chmod 666`).
- **NVIDIA needs the format modifier.** NVIDIA's buffers are tiled and it has no implicit modifiers; dockur 6.05's
  spice-server 0.15 drops the modifier, so clients import the buffer as linear and show black. Making QEMU hand out a
  linear copy doesn't work on NVIDIA (it can't render into, or copy into, a linear dmabuf, and re-importing as a linear
  memory object fails). `tools/gpu-spike/spice/Dockerfile` builds spice-server 0.16 and QEMU 11.1 against it
  (GL_SCANOUT2 carries the modifier); the client needs spice-gtk 0.43 (June 2026; distros still ship 0.42).
- **spice-gtk 0.43 draws black on NVIDIA under Wayland** (it imports the image on its own EGL display but draws in
  GtkGLArea's context); under X11 (`GDK_BACKEND=x11`, XWayland) it works.
- **The driver's vblank ran at 32 Hz**: the flip thread slept 16.7 ms, rounded up to two 15.6 ms timer ticks. DWM
  composed at 32 Hz in the console. Fixed with a high-resolution periodic timer (kvm patch 0003): DWM now measures
  58.9 Hz and SPICE delivers 60 fps; spicy uses ~5% of a core.
- **Latency** (key press to changed pixels in the client's X11 window, 30 presses, a window that flips black/white,
  `latency` tool in the session scratchpad): SPICE median 30.9 ms (p90 35.5); RDP RemoteApp median 35.8 ms (p90 50.4)
  at 30 fps.
- **One session per user** on client Windows: an RDP connection takes over the console session, so the SPICE desktop
  and RDP app windows can't be used at the same time. A "display route" setting switches between them.
- **In WinBoat** (both `main` and the 0.9.2 port): a "Display" setting in the Virtual GPU card, "App windows (RDP)"
  or "Low-latency desktop (SPICE)". SPICE builds `spice/image/Dockerfile` locally the first time (tag
  `winboat-windows-spice:6.05-1`), rewrites the compose (`GPU` off, `DISPLAY=none`, the SPICE options appended to
  `ARGUMENTS` after WinBoat's QMP option, `~/.winboat/spice` mounted at `/spice`), and app launches open the desktop
  instead of RDP. The viewer is bundled: `spice/build-viewer.sh` builds spice-gtk 0.43, virt-viewer 11's
  `remote-viewer` and libxml2 2.11 (libxml2 2.14 renamed its library to `.so.16`) on Ubuntu 22.04 into
  `spice/viewer` (3.9 MB, git-ignored). It runs with `GDK_BACKEND=x11`.
- **Resizing** Windows to the viewer window: the SPICE agent (vdagent-win 0.10, bundled in `guest_server/drivers/spice`)
  passes the size, QEMU raises the virtio-gpu display event, viogpu3d adds the mode, and virtio-win's `viogpuap.exe`
  applies it (ChangeDisplaySettings). `gpu-driver.ps1` installs both with the driver: the agent's MSI, and
  `viogpuap.exe` as an elevated logon task for the Administrators group, run through `conhost --headless`. The agent
  also shares the **clipboard** both ways.
- **Resizing hung Windows' display at first, a driver bug** found with a live kernel dump: DWM's mode change waited in
  `VidSchiWaitForEmptyHwQueue` for a 3D submission that never completed. `CtrlQueue::QueueBuffer` ignored `-ENOSPC`
  from the 256-entry control ring (no indirect descriptors) and dropped the command with its completion callback;
  SPICE pausing the device until the viewer acknowledges a frame lets the ring fill. kvm patch 0004 waits for room
  instead (where the IRQL allows) and also splits scatter-gather elements at page boundaries.
- **Audio** over SPICE: `-audiodev spice -device ich9-intel-hda -device hda-duplex` (Windows' HD Audio driver);
  the viewer plays it through GStreamer/PipeWire.
- **Opening an app** in SPICE mode starts it on the console desktop: the guest server's `POST /desktop/launch` (as
  LocalSystem: `WTSQueryUserToken` of the console session, `CreateProcessAsUser` with the user's environment on
  `winsta0\default`; password-checked on 0.9.2, token-protected on `main`), then shows the viewer.
- **Hardware cursor** (kvm patch 0005): viogpu3d had the pointer DDIs stubbed and the capability commented out, so
  Windows drew the cursor into every frame and over SPICE it moved only as fast as frames made the round trip.
  viogpudo's cursor (64x64 ARGB resource on the cursor queue) is ported and `HWCursor` defaults to 1; the SPICE
  client (client mouse mode) now draws the cursor itself. (Even viogpudo ships `HWCursor=0`.)
- **Signing in**: WinBoat's installer sets `AutoAdminLogon=0` (`RDPApps.reg`), because a signed-in console session
  blocks RemoteApp logons (client Windows: one session per user; a RemoteApp session can't be moved to the console
  either, `tscon` error 7069). The SPICE route turns auto logon on (`POST /desktop/autologon`: the password as the
  LSA secret `DefaultPassword`, like Sysinternals Autologon) and switching back to RDP turns it off again; both
  restart Windows, so it takes effect at once.
- **App window mode**: apps start maximized (`STARTF_USESHOWWINDOW`/`SW_SHOWMAXIMIZED`) in
  `winboat-spice-viewer` (`spice/viewer-build/winboat-spice-viewer.c`): spice-gtk's display widget in a plain window
  titled and classed after the app (like FreeRDP's `/wm-class`), no menus, toolbar or status bar; it replaced
  remote-viewer (and the bundled libxml2). One app window at a time. The app hides Windows' taskbar (the guest server
  runs itself in the user's session, `--taskbar hide`: auto hide via `ABM_SETSTATE`, then the bar's window hidden;
  "Windows Desktop" shows it again), and its window closes when the app has exited (`GET /desktop/process/{pid}`,
  the process or what it started; a launcher or Store app that hands over within seconds can't be followed, so its
  window stays).
- **Large windows crashed QEMU**, a driver bug (kvm patch 0006): the 2D frame buffer segment is sized at start (16 MB
  or the BAR), so a maximized window on a 5120x2160 display (5106x2051, 42 MB) failed `VioGpuObj::Init`, whose error
  message dereferenced a NULL segment; the 2D resource was left without backing and QEMU segfaulted on it. The
  segment now grows with the mode. The viewer no longer scales: Windows' resolution follows the window. When Windows
  keeps another resolution than the window (a game switched it) for a second, the picture is scaled to fit, its
  aspect kept, as a monitor shows it (WinBoat 0.9.2-gpu.100; before, a larger picture showed its bottom left and
  looked black: spice-gtk patch 0007).
- QEMU's `info virtio-status` crashes QEMU 11.1 (`hmp_virtio_dump_features`) on this device: don't use it.
- **Windows composed at 20 fps over SPICE** (user's VM, measured with QEMU trace events and a 60 fps test window):
  without a viewer DWM submitted a frame every 17 ms, with one every 50 ms (every third vblank), although the app
  painted 63 fps. After each scanout update QEMU blocks the whole virtio-gpu queue until the client confirms the draw
  (`GL_DRAW_DONE`); spice-gtk draws at its next frame clock tick, so the guest's next frame waited for it and missed
  its own vblank. QEMU patch `spice/image/patches/0001` draws local clients' updates without blocking and coalesces
  updates that arrive meanwhile (the guest flips between three buffers, so it renders into another one). Remote
  clients (video encoding) keep the blocking. Image `winboat-windows-spice:6.05-2`: DWM composes and the viewer draws
  59 fps with the viewer open (was 20).
  Measuring: `trace-event virtio_gpu_cmd_ctx_submit on` etc. on the HMP socket (`/run/shm/monitor.sock` in the
  container), with a topmost WinForms window repainting every 15 ms that reports its own fps in its title.
- **GPU fences were polled every 10 ms**: QEMU only uses virglrenderer's async fence callbacks when built against
  1.1.2+, and the image was built with Debian trixie's 1.1.0 headers (the dockur image runs 1.3.0). It is now built
  against 1.3.0's headers: fence latency 4.8 ms median (up to 10) before, 0.3 ms after.
- **A finished frame waited for the driver's next vblank tick** before it went to the host: `SetVidPnSourceAddress`
  only marked the flip, and the flip thread sent the scanout at the next 60 Hz tick together with the vsync that
  completes it. kvm patch 0007 queues a DPC that wakes the flip thread to send it at once; the vsync stays on the tick,
  so DWM's pacing is unchanged. GPU work done -> scanout sent: 14.6 ms median before (user's VM), 0.1 ms
  after (spike, p90 0.2), 60 fps either way. (A first measurement of 5.3 ms was of a stale build, see below; WinBoat
  0.9.2-gpu.8 shipped that stale driver, gpu.9 has the patch.)
- **The viewer swapped with vsync** (spice-gtk's own EGL surface on X11, `spice/viewer-build/patches/0001`): it now
  swaps with interval 0, so Xwayland can hand the frame to the compositor without waiting for a frame. Measured with
  an `eglSwapBuffers` hook (libepoxy resolves it with `dlsym`, so the hook wraps `dlsym`): swaps took 0.5 ms either
  way and happen twice per frame (the scanout and the draw), so any gain is in Xwayland's presentation, which can't be
  measured from the client.
- **Refresh rates up to 240 Hz** (kvm patch 0008, WinBoat 0.9.2-gpu.9): Windows composed at most 60 fps because the
  driver offered one 60 Hz timing. It now offers every mode at 60/75/90/100/120/144/165/180/240 Hz (60 preferred) and
  runs the vblank timer at the committed rate. Two more things kept Windows at 60: QEMU's EDID range limits (50-125
  Hz, 30-160 kHz), which the driver now widens, and `DescribeAllocation` reporting 60 Hz for every allocation, so at any
  other rate Windows refused DWM's flips and the desktop froze after one frame. The viewer prints the refresh rate of
  the monitor its window is on (`refresh-rate <Hz>` on stdout, also when the window moves to another monitor), and
  WinBoat sends it to the Guest Server (`POST /desktop/refresh-rate {maxRate}`), which runs itself in the user's
  session (`--refresh-rate <Hz>`) to pick the fastest rate Windows offers up to it (`ChangeDisplaySettingsEx`).
  Resizes through the SPICE agent keep the rate. On the spike (unpatched QEMU, which polls fences every 10 ms) DWM
  composed at half the rate above 60 Hz; it needs the SPICE image's QEMU (async fences) for one frame per vblank.
- **Seamless app windows, phase 1** (WinBoat 0.9.2-gpu.10), like RDP's RemoteApp: an app's windows show as Linux
  windows of their own, its own title bar the frame. Windows' desktop gets the primary monitor's size; the Guest
  Server's window tracker (`--window-tracker`, started by `/desktop/launch` with `seamless`, in the user's session)
  follows the app's process tree with `SetWinEventHook` (coalesced, plus a 250 ms timer for processes) and reports
  its top-level windows (`DWMWA_EXTENDED_FRAME_BOUNDS`, cloaked ones left out) over a SPICE port: `-chardev
  spiceport,name=org.winboat.windows` on a virtserialport, `\\.\Global\org.winboat.windows` in the guest (overlapped
  I/O; reads fail with ERROR_NO_SYSTEM_RESOURCES while no client is connected, writes are dropped). The viewer
  (`--seamless`, apps to follow as `track <pid>` on stdin) makes an undecorated window per guest window (popups such
  as menus and tooltips override-redirect, dialogs transient for their owner) at the same place, each a
  `SpiceDisplay` showing that rectangle: spice-gtk patch 0002 adds `spice_display_set_window_area()` (drawing, size,
  and pointer positions for the whole display, also outside the widget so title bar drags keep going) and sends one
  GL_DRAW_DONE per gl-draw however many widgets draw it. Focus, close, minimize and restore go back to Windows; the
  host monitor's work area becomes Windows' (maximized windows leave the panels free). The protocol is described in
  `winboat-spice-viewer.c`. Without the port or an answering tracker the viewer shows the whole desktop. Port
  channels aren't connected by spice-gtk by themselves (display widgets connect theirs). Tested on the spike:
  Notepad as its own window, following moves made in Windows (25 updates for a 21-step move), its File menu as a
  popup, input through the cropped window. Not yet: window manager moves and resizes (phase 2), stacking order with
  Windows' other windows, more than one monitor, matching Windows' DPI to the host's scale (the user's main
  monitor is at 175 %, Windows at 100 %; under Xwayland apps see physical pixels, xdotool takes logical ones).
- **Seamless app windows, phase 2** (WinBoat 0.9.2-gpu.11): the Linux window manager moves and resizes the windows,
  like RemoteApp's local move/size. Dragging a title bar or edge starts Windows' own move loop; the tracker hooks
  `EVENT_SYSTEM_MOVESIZESTART`, asks the window what was grabbed (`WM_NCHITTEST`, or the pointer's place for apps
  that start the loop themselves) and sends `movesize <hwnd> move|n|…|se`. Once the pointer moves 4 px with the
  button down (a click or double-click on the title bar stays Windows'), the viewer releases the button in Windows,
  which ends its loop, and hands the drag to the window manager (`_NET_WM_MOVERESIZE`). Every configure the window
  manager makes goes back as `move <hwnd> <frame>`; the tracker applies the latest one per window (`SetWindowPos`
  around the visible frame, keeping the invisible borders) and Windows' own reports don't move the Linux window back
  for 400 ms. Meanwhile the window shows where Windows still has it: spice-gtk patch 0003 draws a window area at the
  top left instead of centered, and requests no size for it (with scaling off spice-gtk requests the area's size,
  which became the window's minimum size: windows could only grow). Windows' resize borders on the left, right and
  bottom are outside the visible frame, where no Linux window is: the viewer has its own 6 px borders there for
  windows with `WS_THICKFRAME` (flag `r`). Gotchas: when the window manager ends a drag, Xwayland releases the button
  it still holds only after a motion event that still has it down, which the SPICE agent turns into a click in
  Windows; the viewer swallows that window's pointer events until the button is up. KWin restoring a maximized window
  uses its own idea of the size: the viewer ignores KWin's geometry for 500 ms after un-maximizing and lets Windows
  restore it. Maximize state syncs both ways (flag `M`, `maximize`/`restore`). Other apps' windows that cover a
  tracked one in Windows (from using the desktop) go under the lowest tracked window (not the foreground window or
  topmost ones). Icons: the executable's at 64 px (`SHDefExtractIconW`), else the window's own, as RGBA (`icon`
  line, `_NET_WM_ICON`). The viewer now waits for a tracker to open the port before sending, and repeats its `track`
  lines whenever one opens it again (the tracker can take seconds to start). Tested on the spike with xdotool: title
  bar move, top edge (Windows'), right edge and bottom-right corner (the viewer's), double-click maximize and restore,
  KWin's maximize shortcut both ways, Character Map pushed under Notepad.
- **Monochrome hardware cursors** (kvm patch 0009, WinBoat 0.9.2-gpu.12): the hardware cursor took color shapes only
  (`PointerCaps.Color`), so Windows drew monochrome ones, the text cursor among them, into the frames itself: they
  lagged behind the host's pointer, and in a seamless window the last one stayed where the pointer left it. The
  driver now takes them too (`PointerCaps.Monochrome`) and converts them to ARGB: the shape is an AND mask followed
  by an XOR mask, 1 bit a pixel, `Height` rows each (checked against the real I-beam); AND 0 is black or white, AND
  1 transparent or inverting. ARGB can't invert: those pixels are black with a white outline. Masked color shapes
  stay software cursors. Tested on the spike by making the I-beam the system's normal cursor (`SetSystemCursor`):
  the host's X cursor (XFixes) was the I-beam, outlined, and nothing was drawn into the picture.
- **The window tracker blocked Guest Server updates** (fixed in WinBoat 0.9.2-gpu.13): it ran from the Guest
  Server's own executable, in the user's session, and Windows can't delete, overwrite or move an executable that
  runs, so updates left the old server in place and WinBoat sent the update again and again. The tracker now runs
  from a copy per version (`%ProgramData%\WinBoat\WindowTracker\winboat_window_tracker-<version>.exe`); starting one
  ends trackers of other versions and old ones running from the server's executable, and removes old copies. The
  0.9.2 port's update script also ends processes running from the application directory before replacing it (it
  only helps updates from versions that have it: the installed version's script runs). A VM stuck on an older
  version gets unstuck by restarting Windows (no tracker runs until an app opens seamlessly).
- **QEMU aborted when a frame went unconfirmed** (fixed in WinBoat 0.9.2-gpu.14, image `winboat-windows-spice:6.05-3`):
  `spice_qxl_gl_draw_async: condition 'qxl_state->gl_draw_cookie == GL_DRAW_COOKIE_INVALID' failed`, right after
  "no gl-draw-done within one second". QEMU patch 0001's timer treated a draw the client hadn't confirmed within a
  second as done and sent the next one, but spice-server takes one draw at a time and aborts on another while it
  waits. The timer only warns again (as upstream; the guest isn't blocked by a local client's draw). Why no
  confirmation came: in seamless mode, once a window has connected the display channel, a moment with no window
  (an app's last window closed while the app runs on, or one window replacing another) leaves nobody to confirm
  draws. The viewer confirms them itself then (its own `gl-draw` handler, when no window and no desktop window
  exist). Reproduced on the spike (stock QEMU: the warning and a stalled display) by hiding the tracked Notepad's
  windows and moving another app's window: 1 warning with the old viewer, none with the new one.
- **Seamless glitches, round 1** (WinBoat 0.9.2-gpu.15):
  - Focus loop: several windows appearing at once (Windows 11 Notepad restores its earlier windows) each took the
    focus when mapped and sent `activate`; Windows' delayed reports of each one being in front made the viewer present
    them again, forever. New windows now take the focus only if Windows has them in front
    (`gtk_window_set_focus_on_map`), focusing the window Windows already has in front sends nothing, and a report that
    a window the viewer activated earlier is in front is ignored for a second after the viewer activated another one.
  - Hover effects and tooltips stuck when the pointer left a window (Windows never learns it left): the viewer moves
    Windows' pointer to a spot no app window covers 150 ms after it leaves them (not while a button is down). Not
    verifiable with xdotool under Wayland: Xwayland only produces crossing events for it the first time.
  - Tooltips counted as app windows (icon, taskbar entry): `MicrosoftWindowsTooltip` is `WS_POPUP | WS_BORDER`, and
    WS_BORDER is one of WS_CAPTION's two bits. Popups are now popups or tool windows without both caption bits.
  - Menus stayed open when the user went to a Linux app: on losing the focus to another app the viewer sends
    `deactivate`; the tracker sends WM_CANCELMODE to the foreground window and makes the shell's desktop window the
    foreground one (without raising it).
  - The viewer's side resize borders reached into the title bar (the close button's edge): they start below it.
- **Minimized windows** (WinBoat 0.9.2-gpu.16): Windows moves a minimized window far off its screen (-32000), and the
  viewer took that as the window being gone and destroyed its Linux window (minimizing from KDE's taskbar ended the
  same way once Windows reported it). Minimized windows now stay, minimized in KDE too, and come back from either
  side. The tracker reports a minimized window's restored place instead (`GetWindowPlacement`: work area coordinates,
  with the invisible borders measured while the window was shown; flag `M` when it comes back maximized), so a window
  that appears minimized gets its size right away. Coming back from minimized, Windows reports the window off-screen
  but not minimized for a moment: off-screen moments leave the Linux window alone (only hidden windows are gone).
  Tested on the spike: minimize in Windows, restore from KDE, minimize from KDE, restore in Windows, the same Linux
  window throughout, at the exact place.
- **Windows color mode** (WinBoat 0.9.2-gpu.17, settings: General): Follow Linux (default), Light or Dark. The Guest
  Server's `POST /desktop/theme {mode}` runs itself in the user's session (`--theme light|dark`; the console session,
  or the active RDP one when nobody is signed in at the console: `startOptions.AnySession`) to set
  `AppsUseLightTheme` and `SystemUsesLightTheme` and broadcast `WM_SETTINGCHANGE "ImmersiveColorSet"`, so running
  apps switch at once. WinBoat's health check keeps it applied: when the Guest Server comes online, when the setting
  changes, and for Follow Linux when Electron's `nativeTheme` changes (Electron reads the desktop portal's
  `color-scheme` on Linux); before anybody is signed in it retries every 15 s. Tested on the spike: both values
  switch.
- **Behind seamless app windows** (WinBoat 0.9.2-gpu.17, Virtual GPU card): app windows show a part of Windows'
  screen, so Windows' desktop shows around and through them (rounded corners, see-through windows such as FL
  Studio's). The viewer (`--background black|transparent`) sends `background <RRGGBB>` when the port opens; the
  tracker saves the wallpaper and desktop color, then sets no wallpaper and that color for the session only (no
  `SPIF_UPDATEINIFILE`), and puts them back when the viewer goes. A pending read on the port doesn't fail when the
  viewer disconnects: the tracker asks the virtio-serial driver once a second (`IOCTL_GET_INFORMATION`,
  `VIRTIO_PORT_INFO.HostConnected`). Black is the default. Transparent uses 0x010203 and the viewer leaves that color
  out: spice-gtk patch 0004 adds `spice_display_set_transparent_color()` (the fragment shader discards it over a
  transparent clear and draws the rest opaque: the guest's alpha byte means nothing) and `SPICE_GTK_EGL_ALPHA`, which
  picks an EGL config with alpha and a 32-bit visual (spice-gtk picks its config when a display is created, so the
  viewer sets it before); the viewer's windows get the RGBA visual. Tested on the spike (NVIDIA host): a form
  painted 0x010203 shows the Linux desktop through it, its red block stays. Limits: half-transparent pixels blended
  with 0x010203 come out a little dark; clicks on see-through areas still go to Windows; other apps' windows behind a
  see-through window in Windows show there (not the desktop). Gotchas while testing: `CopyFromScreen` leaves out
  layered windows, and Windows 11 on this VM draws square corners (no rounded ones to see through). The spike's
  Windows stayed at 1280x1024 after the cursor driver's reinstall although the agent accepted 5120x2160 (not seen on
  the user's VM; open).
- **No sound from the SPICE viewer** (fixed in WinBoat 0.9.2-gpu.18): WinBoat starts the viewer from its renderer
  process, whose environment is Chromium's trimmed one (no `XDG_RUNTIME_DIR`, no `WAYLAND_DISPLAY`). spice-gtk plays
  through GStreamer's `autoaudiosink`: without the runtime directory `pulsesink` can't reach PipeWire/PulseAudio,
  `alsasink` has no device, and `openalsink` fails ("[ALSOFT] (EE) Failed to connect PipeWire event context (errno:
  112)"), so there was no sound in desktop and seamless mode alike. The viewer now gets the main process's
  environment (`remote.process.env`), with `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS` filled in if missing.
  Reproduced on the spike: with only DISPLAY/XAUTHORITY/HOME/PATH no stream and the OpenAL error; with
  XDG_RUNTIME_DIR added a `winboat-spice-viewer` stream plays Windows' sound.
- **SPICE viewer crashed on start** (fixed in WinBoat 0.9.2-gpu.20, a regression of the sound fix): the main process's
  environment carries `EGL_PLATFORM=wayland`, which Chromium sets for itself under Wayland. The viewer runs on X11
  (XWayland), and NVIDIA's EGL then took its X11 display for a Wayland one: SIGSEGV in `wl_proxy_create_wrapper`
  (libnvidia-egl-wayland2) from spice-gtk's EGL setup, before the viewer printed anything ("The SPICE viewer didn't
  start (exit code null)"). The viewer now gets `EGL_PLATFORM=x11`. Found in the core dump's environment (Chromium
  overwrites its processes' `/proc/<pid>/environ`); on the spike the viewer exits with 139 with `EGL_PLATFORM=wayland`
  and runs with `x11`.
- **Edge didn't show in seamless mode; apps started inside Windows didn't either** (fixed in WinBoat 0.9.2-gpu.21):
  the tracker followed the started process and what it starts. Edge runs in the background from sign-in ("startup
  boost", `msedge.exe --no-startup-window`): the `msedge.exe` WinBoat starts hands the request to it and exits, so the
  window belonged to an untracked process, and the tracker reported `idle` (the viewer quit) right away. Explorer's
  folders and single-instance apps do the same, and windows of apps started inside Windows (a link opening the
  browser) never showed. The tracker now notes when every top-level window first shows, and while it tracks apps it
  adopts each new window (shown since 15 s before the first `track`, for 0.5 s at least, not minimized, no owner, not
  a popup): its process is tracked from then on (Explorer's shell process: only that window) and the topmost one is
  made Windows' foreground window (the viewer brings that window to the front). A `track` keeps the viewer for 20 s
  even without a running tracked process (an app handed to a running instance may take a moment); adopted processes
  keep it only while one of their windows shows (Edge keeps running in the background). The 0.5 s wait keeps out
  consoles that flash up in Windows Terminal for programs started hidden. On the spike: Edge started like startup
  boost and then launched is adopted within 40 ms of `track` and goes with its window (`idle` 0.4 s later); Edge
  started inside Windows while Character Map is tracked is adopted and in front; Character Map alone is unchanged.
- **"Starting …" in the app list** (WinBoat 0.9.2-gpu.21): in seamless mode nothing showed until the app's window
  did (Edge: seconds, big apps longer), so apps got clicked again. The viewer prints `window-shown` on stdout when it
  shows an app's window (and `seamless-unavailable` when it shows the whole desktop instead); until then, or for a
  minute at most, the app's entry shows "Starting…" with a spinner, and clicking it again doesn't start it twice.
- **FL Studio's main window wasn't a window of its own** (fixed in WinBoat 0.9.2-gpu.81): seamless mode took it for
  a popup (no Linux window decoration of its own, no taskbar entry) while Notepad's was fine. Its main window
  (`TFruityLoopsMainForm`, a VCL form drawing its own title bar) is `WS_POPUP` without the caption bits, which the
  tracker's popup rule (menus, tooltips) matched, but it has `WS_EX_APPWINDOW`, which asks Windows for a taskbar
  button. Windows with `WS_EX_APPWINDOW` are app windows now, whatever their style; tooltips and menus don't set it.
- **App windows on every monitor** (WinBoat 0.9.2-gpu.96): Windows' desktop was the primary monitor, and the
  viewer kept Windows' window on it, so a window dragged to another monitor came back. The Multi-Monitor setting
  (MultiMon or Span) now starts the viewer with `--span`: Windows' desktop gets all monitors' bounding box (the
  user's 5120x2160 + 3360x1890 under Xwayland: 8488x2160, width rounded up to 8) and windows go wherever the window
  manager puts them. Windows has one monitor, so it maximizes to its work area (the primary monitor's); maximized on
  another monitor here, the viewer moves the maximized window to where KWin maximized it (`move ... M`, which the
  tracker applies to maximized windows only). The tracker puts the work area back after Windows' resolution changes
  (which resets it). Full screen windows (games) cover all monitors then, like RDP's /span; None keeps the old
  behaviour. Tested on the spike with xdotool: moved to the second monitor (Windows follows, no snap back),
  maximized there from KWin and from Windows (IsZoomed, the monitor's area within Windows' 2 px border), restored,
  moved back.
  gpu.96 showed black (and flickering) app windows: QEMU's virtio-gpu devices refused resources backed by more than
  16384 guest pages (64 MB; `vg_create_mapping_iov_at: nr_entries is too big (17905 > 16384)` in the container log),
  and every screen-sized surface of the 8488x2160 desktop is 73 MB, so Windows' compositor lost its surfaces and its
  contexts failed ("Illegal resource") until Windows restarted. The gpu.96 test had only compared positions; GDI
  screenshots inside Windows are black with this driver anyway, so the check is a host screenshot. Image 6.05-30
  (QEMU patch 0012) takes up to 1048576 entries (4 GB); WinBoat 0.9.2-gpu.97 uses it. Verified with host screenshots
  on the spike (Notepad on the second monitor, maximized there) and in the user's VM.
- **Windows' stacking follows KDE's** (WinBoat 0.9.2-gpu.22): every Linux window shows its part of Windows' one
  screen, so where two app windows overlap in Windows, both Linux windows showed whichever was on top there, e.g.
  after raising a window in KDE without clicking into it, or two apps on different virtual desktops at the same
  place. The viewer watches KWin's `_NET_CLIENT_LIST_STACKING` and `_NET_CURRENT_DESKTOP` on the (XWayland) root
  window and sends `order <hwnd>...`: its app windows (not popups, not minimized) from the top down, the current
  virtual desktop's first (`_NET_WM_DESKTOP`). On every snapshot the tracker moves each shown window that is above
  the one before it in the order right under that one (`SetWindowPos`, no activation); a window Windows just made
  the foreground window (a new dialog) keeps its place for up to a second, until the viewer has brought it to the
  front in KDE. The viewer links libX11 now. On the spike: two overlapping Character Map windows, the lower one
  raised in KDE with `xdotool windowraise` (no activation): Windows stacks it on top within the next snapshot,
  Windows' foreground window unchanged. Not tested: switching virtual desktops (the user has one).
- **GPU on the Home screen** (WinBoat 0.9.2-gpu.23): Windows can't measure it: its `GPU Engine` counters exist for
  the virtio GPU (3D engine per process) but stay at 0 even while DWM composes an animation (the KMD doesn't report
  engine busy time). The work is done on the host GPU in QEMU's process anyway, so `src/renderer/lib/gpuMetrics.ts`
  measures that while the Home view shows (helpers run only then, each ends after a minute and is restarted, so none
  outlives WinBoat): QEMU's host pid by the container id in its cgroup (its process name is "windows": dockur's
  `-name windows,process=windows`; matched by its command line). NVIDIA: `nvidia-smi pmon -s um` (the process's sm %
  and fb MB), `dmon -s u` (the GPU's own load) and the GPU's name. Other drivers: DRM fdinfo (`drm-engine-*` ns,
  xe's `drm-cycles-*`, `drm-memory-vram`/`drm-resident-vram*`), read inside the container (QEMU runs as root, its
  fdinfo isn't readable from the host), plus AMD's `gpu_busy_percent`. The card shows the VM's share of the GPU and
  its video memory (tooltip: the GPU's total load), "No virtual GPU" without one. Checked on the user's laptop: their
  VM idle 0 % / 638 MB (GPU 48-61 % busy from host apps), the spike VM 7 %. The fdinfo path is untested (NVIDIA
  only here; the in-container script runs and finds QEMU's file descriptors).
- **Virtual GPU traffic on the Home screen** (WinBoat 0.9.2-gpu.24): a third line on the GPU card, "N cmd/s · X MB/s
  · N fps", each part where it's known:
  - *cmd/s*: QEMU's used index of the virtio GPU's control queue (16 bits), read through dockur's HMP monitor
    socket (`/run/shm/monitor.sock`, `info virtio` / `info virtio-queue-status <path> 0`) by a Python script in the
    container, connecting for each sample only (dockur uses the socket to shut Windows down; WinBoat holds the QMP
    port). Works with any driver build.
  - *MB/s*: viogpu3d counts what it sends (kvm-guest-drivers-windows patch 0010): commands and their bytes with
    their data (3D command streams, which also carry most texture data inline: virgl's inline writes), and the bytes
    of transfers to/from the host (estimated from the box and strides). The `VIOGPU_GET_STATS` escape returns the
    totals; the guest server finds the adapter that reports `VIOGPU_IAM` (D3DKMTEnumAdapters2 +
    D3DKMTQueryAdapterInfo UMDRIVERPRIVATE) and adds them to `/metrics` (`gpu`, absent with an older driver: it needs
    the driver update from the Virtual GPU settings and a Windows restart). WinBoat makes bytes per second of them.
  - *fps*: the viewer counts the display channel's `gl-draw`s and prints `fps <n>` every two seconds.
  On the spike, with a GDI animation in a 900x600 window: QEMU 186 commands/s, the driver ~200 commands/s and
  0.5-1 MB/s (upload counter only 40 KB: the cursor), the viewer 58-59 fps; a static desktop 0 fps.
- **Caption buttons of packaged apps (fixed 2026-10-13, WinBoat 0.9.2-gpu.26)**: in Settings, Calculator, Store (UWP apps framed by
  ApplicationFrameHost) minimize/maximize/close show as white blocks (user's VM, Calculator on the spike) or not at
  all (Settings on the spike); Win32 frames (Character Map) and WinUI 3 title bars (Notepad) are fine, and so is the
  apps' own content. Transparency effects off: still white. ApplicationFrameHost draws the title bar with system XAML
  (Windows.UI.Xaml) on our D3D10 UMD. What was ruled out with test programs (d2dtest/sharetest/nttest, hardware vs
  WARP): Direct2D lines, outlines, paths, geometry realizations, clips, bitmaps, A8 opacity masks (uploaded and drawn
  into A8 targets) all match WARP; legacy shared textures (A8 and BGRA) opened on a second device read back right;
  NT-handle shared BGRA works. What fails: an **A8_UNORM texture shared with an NT handle** (MISC_SHARED_NTHANDLE)
  can't be created (E_INVALIDARG from the runtime; WARP: fine). Sharing formats beyond the basic ones with NT
  handles is D3D11.1's ExtendedResourceSharing, which the runtime only grants to drivers with the D3D11.1 DDI; Mesa's
  d3d10umd implements the D3D10.0/10.1 DDIs (11.0 compiled out). Composition's alpha-only (mask) surfaces need it;
  SystemSettings and dwm then also load D3D10Warp (a software fallback), and the masks come out wrong. Likely fix:
  a D3D11.1 DDI in the UMD reporting ExtendedResourceSharing (most 11.0/11.1 entry points can refuse or be no-ops at
  feature level 10_0), then retest the caption buttons.
  - **D3D11.1 and WDDM 1.3 DDIs (2026-10-13, Mesa patches 0005-0007)**: new
    `d3d10umd/D3D11_1.cpp`: the 11.1 device table built from the 10.0 one (wrappers for the changed entry points:
    constant buffers with offsets, ClearView, Flush, 11.x resource/view/state/shader arguments; stubs for feature
    level 11 and tiled resources), DXGI 1.2/1.3 tables (Present1), GetCaps answering the 3D pipeline level (10_0).
    The runtime then reports ExtendedResourceSharing and A8 is shareable; A8 NT-handle textures work (nttest,
    sharetest incl. keyed mutex, d2dtest unchanged). Found on the way: (1) 11.1 shaders carry minimum-precision
    extended operand tokens on destinations, which ShaderParse.c didn't skip (asserts compiled out: misparse, crash
    in Direct2D's pixel shaders): now skipped on destinations and declarations, chains handled on sources.
    (2) With 11.1 the caption buttons went from white blocks to empty: DirectComposition makes A8 masks as
    *composition buffers* (dcomp `CDynamicPool` → `d3d11!CDevice::CreateCompositionBuffer`), which the runtime only
    allows in A8/R8/BC formats for drivers whose DDI index is ≥ 3 (`NDXGI::CDevice` caps bit 0x20 set in
    `LLOCompleteLayerConstruction`): the WDDM 1.3 DDI. (3) The WDDM 1.3 DDI bug-checked the kernel (0x113 subtype 7
    in `DXGDEVICE::CreateDriverAllocations`: a second allocation set on a shared resource) on the next D3D app and on
    every boot (DWM): gdikmt_d3dddi tagged every allocation made during CreateResource (also virgl's staging
    buffers) with the runtime resource; now only the first. Recovery from the boot loop: the disk image mounted from
    a privileged container (ntfs3, loop at the partition offset), the previous DLL put back (script in
    ~/.cache/claude-winboat/tools/spike-restore-umd.sh); `-action panic=none` in the spike's QEMU arguments lets
    Windows write minidumps (QEMU powered off on hv-crash before). (4) DirectComposition presents composition
    buffers in batches (Present1 with 1 or 2 surfaces); presenting only the first left garbage (recycled host
    memory) in the buttons; now each is presented. State then: the buttons were empty (no garbage, no white).
  - **GDI on Direct3D surfaces (2026-10-13, kvm-guest-drivers-windows patch 0011): the actual cause.** UxTheme draws
    the glyphs with GDI through `IDXGISurface1::GetDC` on the BGRA composition surfaces (cdb on ApplicationFrameHost:
    `CGdiInterop::CreateDeviceBitmap` → `NtGdiCreateBitmapFromDxSurface2` returned 0, the runtime swallowed the
    error and GDI's drawing was lost; gditest, GDI's FillRect on a GDI-compatible texture: WARP 255,255,255 vs 0 on
    ours). Read from win32kfull/cdd with public symbols (`cdb -z <image>`, `uf`): `GrepCreateBitmapFromDxSurface`
    needs `GreDwmUseDeviceBitmaps` (`HKLM\Software\Microsoft\Windows\DWM\DisableDeviceBitmaps` unset, so on) and
    then CDD's `DrvCreateDeviceBitmapEx`, which refuses unless the driver's `DXGK_PRESENTATIONCAPS` has
    `SupportKernelModeCommandBuffer` (GDI hardware acceleration) or `SupportSoftwareDeviceBitmaps` (PDEV flags
    0x40/0x8000 at +0xAB8, copied from caps at +0x76C). viogpu3d reported neither. Patch 0011: reports
    `SupportSoftwareDeviceBitmaps`; `OpenAllocation` on GDI's device (`DXGK_CREATEDEVICEFLAGS.GdiDevice`) returns the
    linear layout (offset 0, pitch width*4) of 2D BGRA/BGRX allocations, and CDD then draws on their pages with the
    CPU (`CddBitmapSw`; Start/EndDxInterop only sync and flush, the driver sees nothing at GetDC/ReleaseDC). The pages
    are virgl's staging copy, not the texture, so: the texture is read into them when GDI opens the allocation, and
    while GDI has it open (until the surface is destroyed) every submission that uses it uploads the pages before it
    and reads the texture back after it, before its fence completes (asynchronously, in order on the control queue).
    The cap also makes DWM ask for CPU-visible GDI surfaces as redirection bitmaps of GDI windows (GetStandardAllocation
    DriverData, `D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE`): accepted, they worked but meant uploading whole windows on
    every composed frame (1.2 GB while opening four windows); refused, DWM keeps system memory redirection (only what
    changed is uploaded), and the D3D-surface path still works. Also in 0011: `DescribeAllocation` reported every
    allocation as 2x multisampled (now the real count), a present without a destination no longer dereferences it.
    Result: Settings' and Calculator's caption buttons render; Character Map/regedit unchanged; gpucheck all pass
    (console/RDP, 64/32-bit; one 32-bit flip-model present showed black once, passed on reruns); idle cost none.
    **Limitation**: GDI drawing is lost when the app has Direct3D/Direct2D drawing on the same surface that wasn't
    flushed before GetDC (d2dgdi, Direct2D's `ID2D1GdiInteropRenderTarget`: the GDI rectangle comes back as D2D's
    red; with an explicit `Flush()` before GetDC gditest matches WARP). The runtime calls no driver function at
    GetDC (no Flush DDI, traced), the UMD can't tell GDI-compatible resources apart (the runtime passes them as
    MISC_SHARED), and flushing after every draw to a shared target would cost too much. UxTheme isn't affected.
- **Audio latency** (WinBoat 0.9.2-gpu.19): measured from Windows starting a sound (a click at an agreed moment, the
  clocks matched over one SSH session that streams the guest's clock: best sample) to it reaching PipeWire (the
  default sink's monitor, `parec`): 167 ms (166-170). spice-gtk plays through `autoaudiosink`, here `pulsesink`
  (PipeWire's PulseAudio server), which buffers 200 ms by default. spice-gtk patch 0005 sets the sink's
  `buffer-time`/`latency-time` from `SPICE_GTK_AUDIO_BUFFER_MS` (buffer[,segment] ms; on the sink autoaudiosink adds,
  through `deep-element-added`); the viewer sets 30,5 unless the variable is set. Measured with an override pipeline
  first (`SPICE_GST_AUDIOSINK`): 60/10 ms 103, 40/10 87, 30/10 76, 20/10 88 (PipeWire negotiates worse), 30/5 72,
  25/5 74, 20/5 67. Dropouts (2 ms or more of silence inside an 8 s tone): none at any size idle; with every host core
  busy 20/5 once had 2 (3 ms), 30/5 none, hence 30/5. The patched viewer: 74, 72, 75 ms. What's left (~45 ms) is
  Windows' audio engine, QEMU's HD Audio emulation (it keeps its 8 KB buffer half full: 21 ms) and SPICE's 10 ms
  frames. QEMU's audio timer at 5 ms (`-audiodev spice,timer-period=5000`) changed nothing measurable.
  Tried and dropped (spike, test image with two more QEMU patches, the viewer's sound in a PipeWire null sink so the
  host's own audio stays out of the recording): an `hda-duplex` `buffer-size` property (its 8 KB buffer is kept half
  full) and the SPICE backend honouring `out.buffer-length` (its mixing buffer is 480 * 4 frames, 40 ms, before a
  real-time rate limiter). HDA 8192 / default: 72, 73, 74 ms; HDA 4096: 75, 71, 74; HDA 4096 + 20 ms: 72, 72, 71;
  HDA 4096 + 10 ms: 75, 74, 71 with 21 dropouts (13 ms). Neither buffer holds the latency; what's left is
  Windows' audio engine and how far ahead it writes the sound card's buffer, and PipeWire's quantum (1024 frames,
  21 ms here, the user's setting).
- **Crackling sound under load (WinBoat 0.9.2-gpu.27)**: on the user's VM (SPICE route, 5120x2065, Windows at
  180 Hz to match the monitor) FL Studio's sound glitched while playing and badly while a window moved, with the
  Windows sound device and FL Studio ASIO alike; Linux apps were fine. The viewer's stream had no gaps and PipeWire no
  xruns: the damage happens in the VM. Sampling QEMU's main thread (`/proc/<pid>/task/<tid>/stat` about every ms):
  busy 61% of the time, 40 stretches of 20-50 ms in 20 s. virgl runs the guest's GL work on the main loop, which also
  runs the HD Audio emulation (its buffer has ~21 ms of slack), so each long stretch is a glitch. At 60 Hz: busy 8%,
  longest 6.4 ms. Two changes: a "Windows refresh rate" setting (Virtual GPU card: Match monitor or 60-240 Hz,
  applied right away; `vmRefreshRate`), and the guest server now changes the rate at custom resolutions: the SPICE
  agent sizes the display to the monitor (5120x2065), Windows lists no modes for that resolution, and the helper
  picked rates only from that list, so it never lowered the rate (the log showed "host monitor at 100 Hz, Windows at
  180 Hz"); without listed modes it uses the driver's rates. Smoothing on the Linux side can't help: the sound is
  already broken when it leaves the VM. At 60 Hz the main loop was still 44% busy with FL Studio playing (5-14 ms per
  frame): perf from a helper container (`docker run --privileged --pid=container:WinBoat`, `perf record -t <main
  thread>`) showed it copying pixels: virglrenderer's `read/write_transfer_data` gathering a transfer row by row from
  the guest's 4 KiB pages, walking the page list from its start for every row (19%), memcpy (14%) and NVIDIA's own
  conversion (55%). The driver's counters showed no transfers of its own: they come through the command stream.
- **The GPU in a process of its own (WinBoat 0.9.2-gpu.28, image winboat-windows-spice:6.05-4)**: the SPICE route
  now runs QEMU's `vhost-user-gpu` (contrib/) instead of `virtio-vga-gl`: virglrenderer's work happens in that
  process, and QEMU's main loop (sound card, vCPU I/O) no longer waits for it. The image builds it with QEMU, and
  `qemu-wrapper.sh` (installed as qemu-system-x86_64, the real binary in /usr/libexec next to share/qemu for its
  ROMs) starts it on the socket the arguments name (`-device vhost-user-vga,chardev=winboat-gpu -chardev
  socket,id=winboat-gpu,path=/run/shm/winboat-gpu.sock`) and adds the shared guest memory vhost-user needs
  (`memory-backend-memfd,share=on` of the `-m` size). On the spike under the same guest load the main loop went from
  15-16% busy (stretches up to 17 ms) to 2-4% (up to 11 ms, rare); no dropouts in the tone test, viewer at 59-60 fps.
  What it took:
  - kvm patch 0012: viogpu3d acknowledged VIRGL after writing the features to the device (and EDID never):
    QEMU's own device runs 3D commands anyway, vhost-user-gpu ran in 2D mode ("TODO handle ctrl 205/207").
  - QEMU 0002: vhost-user-gpu waits for an answer to each scanout update, which QEMU sends when the console
    unblocks; with 0001 SPICE never blocks for a local client, so it answers right away (only if not answered
    meanwhile: a display that flushes from within the update answered too, and the extra answers piled up).
  - QEMU 0003: a guest reboot resets the device; QEMU closes the GPU socket while the backend still sends the
    guest's last commands (SIGPIPE killed it) and sets a new socket (an assertion). SIGPIPE ignored; every socket
    starts afresh (old socket, resources, `virgl_renderer_reset`, fences, scanouts).
  - QEMU 0004: on reset the consoles kept the backend's dmabuf without a surface and VGA (which takes over until
    the driver starts) crashed in `vga_update_display`: reset like virtio-gpu-gl (placeholder surface, scanout off).
  - QEMU 0005: an unexpected reply made `vg_recv_msg` return without reading its payload, and the stream stayed out
    of step (hundreds of assertion failures); it now skips the whole message.
  - QEMU 0006 (found on the user's VM, not on the spike): set_status made a new GPU socket on every status write
    with DRIVER_OK, also while running; the backend switched to it and closed the old one (0003) while QEMU kept
    reading the old one: "failed to read msg header: 0, 11" millions of times, a spinning main loop (the sound
    still crackled), no display, and the VM didn't shut down (killed after the timeout). Now one socket per start,
    and a socket the backend closed isn't read any more. Look for that message when the VM misbehaves.
  - WinBoat's metrics (gpu.29): the "QEMU commands/s" helper ran `info virtio-queue-status <gpu> 0` every second;
    for a running vhost-user device QEMU reads the queue state from the backend with GET_VRING_BASE, which in
    vhost-user stops the queue: Windows' GPU stopped every second on the user's VM (not on the spike, which runs
    no WinBoat). The helper now checks QEMU's command line and leaves a vhost-user GPU alone (that metric is absent
    there; the driver's counters and the viewer's fps remain).
  - Result on the user's VM (5120x2065 at 60 Hz, FL Studio maximized and playing, FL Studio ASIO): main loop 4%
    busy, longest stretch 2.6 ms (was 45-61%, 20-50 ms); the GPU process ~33% CPU; no glitches (the user's ear).
  - QEMU 0007 (image 6.05-5, WinBoat 0.9.2-gpu.30): `gl_flushed` armed the GPU socket's read handler also when
    there was no socket (fd -1: before the device started, after it stopped), so on the user's VM a display flush
    during the firmware and the driver's reset logged "failed to read msg header: 0, 0/2" until the device started.
    It only arms it with a socket. (The spike didn't show the noise even with a viewer connected from QEMU's start.)
  - virglrenderer (built in the image, 1.3.0, replacing the base image's library) 0002: `virgl_renderer_reset`
    stopped the fence thread and only restarted it with the async fence callback, which vhost-user-gpu doesn't use:
    after a reboot no fence completed and Windows hung at boot. 0001 (the copy itself): rows are copied with a
    cursor in the page list instead of a walk from its start for each row.
  Venus (Vulkan) wasn't available with vhost-user-gpu (no blob resources) until QEMU patch 0008 (image 6.05-8, see
  "Vulkan (roadmap stage B)"). VMs set up by
  gpu.27 or older show "Apply" in the Virtual GPU card, which builds the new image and switches the device.
- **Stale driver builds in the spike VM**: the VM's clock was ahead for a while, so its object files are stamped in
  the future, and msbuild relinked old objects for sources pushed later (push-dev.sh gives them the VM's current
  time). Delete `viogpu3d\objfre_win10_amd64` objects before building when in doubt, and check the `.sys` for a new
  string.
- **Switching display routes moves the GPU to another PCI slot** (dockur's device sits in slot 1, the SPICE route's
  from `ARGUMENTS` in slot 4), and Windows keeps the old one as a disconnected device (problem 45, phantom).
  `gpu-driver.ps1` took the first VirtIO GPU it found, so the card showed "problem 45"; it prefers the present one.

- **Dragging windows out of the viewer** (WinBoat 0.9.2-gpu.32/33): moving a Windows window by its title bar stopped
  when a fast pointer left the Linux window. Seamless windows (gpu.32, the viewer): the hand-over to KWin waited for a
  motion event after Windows' "movesize" report and measured the drag from where the pointer was when the report
  came; it now measures from the press and hands over at once when the pointer moved already, anchored at the
  press point (a line per hand-over goes to spice-viewer.log). The desktop window (gpu.33, spice-gtk patch 0006):
  client mouse mode dropped the pointer grab on every press (for drags between displays), dropped motion outside the
  widget and releases outside it: the drag stopped where the pointer left, and stuck once it came back. The grab
  stays now, motion outside with a button down goes to Windows clamped to the screen's edge, releases always go.
- **Borderless full screen** (WinBoat 0.9.2-gpu.34): a window without a full title bar that covers exactly Windows'
  screen (how most games do full screen) was a popup to the tracker. The tracker reports it with flag `F` (not a
  popup), and the viewer makes its Linux window full screen on the monitor Windows' screen stands for
  (`gtk_window_fullscreen_on_monitor`), back when the flag goes; meanwhile it doesn't place or resize it and sends
  no moves. Spike test (fsgame: a D3D11 window, 5 s windowed, 5 s WS_POPUP at 0,0 with the screen's size, windowed
  again): `_NET_WM_STATE_FULLSCREEN` at 5120x2160 (the monitor in physical pixels), back to 786x493 at its place.
  Not covered: exclusive full screen (a game changing Windows' resolution), relative mouse for 3D games.
- **Driver versions** (from WinBoat 0.9.2-gpu.31): every build used to be 100.6.101.58000 (virtio-win's
  `_BUILD_MINOR_VERSION_` default) dated with the build day, so builds of one day looked alike to Windows (WinBoat
  itself goes by the package's catalog hash). `build-driver.ps1` now sets the last part to 58000 + the minute of
  the day it builds (e.g. 100.6.101.58596), the INF's DriverVer and the files' version alike: unique per build
  and growing with the build time, the order Windows ranks drivers in (date, then version).
- **Feature level 11_0, native route, stage 1 of 4** (2026-10-13, spike VM, Mesa patch 0008): shader model 5 in the
  D3D10/11 driver's DXBC-to-TGSI translator, for vertex, geometry and pixel shaders. Stages 2-4 (compute and UAVs,
  tessellation, the rest and reporting 11_0 for real) followed (below); until stage 4 only programs started with
  `WINBOAT_D3D_FL11=1` got 11_0.
  - D3D10 bugs fixed on the way (virgl path): HLSL `continue` and `switch` were dropped by virglrenderer (no TGSI
    CONT/SWITCH; lowered to loops and ifs now); textures were sampled through the sampler's slot (t3 with s0 read
    texture 0); single-channel results ignored the texture operand's swizzle; texel offsets, comparison sampling
    (shadow maps), GetDimensions' float results and integer texture types were wrong or missing.
  - Test: `sm5test.exe` renders 16 HLSL pixel/vertex shaders (compiled at run time) on the driver and on WARP and
    compares the pixels: control flow, sampling with offsets/pairs/cubes/arrays/LOD, comparison, gather, integer
    textures and typed buffers, bit operations, half floats (WARP converts f32tof16 rounding toward zero), all pass.
    A Linux harness runs the translator on DXBC compiled with Windows' d3dcompiler_47 under Wine and prints TGSI.
  - Regression found and fixed: DWM crash-looped (black desktop, exit code 0xc00000fd) because the translator's
    ~90 KB state lived on the stack and overflowed DWM's thread once it grew; it is allocated now. The desktop,
    Direct2D (effects and drawing, identical to before) and GDI interop checked after the fix.
  - Not done yet: MSAA (the driver reports no multisample quality levels at all), subroutine calls (`call`, rare in
    fxc output; virglrenderer has no TGSI CAL either). Both done in stage 4.
- **Feature level 11_0, stage 2 of 4: compute shaders and UAVs** (2026-10-14, spike VM, Mesa patch 0009):
  - compute shaders: thread groups, IDs, group shared memory, barriers, `Dispatch`/`DispatchIndirect`
  - UAVs in compute and pixel shaders: typed as images, raw/structured as shader buffers, raw/structured views
  - atomics, append/consume counters, UAV clears, `CopyStructureCount`
  - barriers after dispatches and after draws with UAVs
  - Problems found and how they're handled (several are virglrenderer/virgl limitations, worked around in the guest
    driver so stock hosts work too):
    - virglrenderer declares store-only images `rgba32f`, a format mismatch with anything else: the driver
      translates a shader again for the bound UAV formats (a few variants per shader).
    - A texture whose only bind is UAV reaches the host with no bind flag and is refused (the first command using
      it then fails the whole GL context: everything after drew nothing); UAV textures carry a sampler bind.
    - GLSL needs `coherent` for writes of other invocations to be seen after a barrier: set where a shader has UAV
      barriers.
    - Integer negate modifiers (fxc's `iadd r0, -r1, l(3)`) became float negations: `INEG` now (a D3D10 bug too).
    - virglrenderer reads back only the first slice of a 3D box: 3D read maps go slice by slice (a D3D10 bug too).
  - Test: `sm5test.exe` now 35 tests vs WARP, 11 of them compute/UAV (structured/raw/typed buffers and textures
    incl. arrays and 3D, typed loads, atomics, group shared memory, 1024-thread and 3D groups, append/consume,
    indirect dispatch, pixel shader UAVs, clears, vertex shader structured buffers): all pass; desktop, Direct2D
    (identical to before) and GDI interop checked after a reboot.
  - Tooling: `vrend_glsl` runs virglrenderer's own TGSI-to-GLSL translation on Linux, so every test shader's GLSL
    is checked with glslang before it reaches the VM (`harness-all.py`).
- **Feature level 11_0, stage 3 of 4: tessellation** (2026-10-14, spike VM, Mesa patch 0010):
  - hull shaders as GL tessellation control shaders: the control point phase per invocation, then the fork/join
    (patch constant) phases once per patch, each over its instance count; tess factors to GL's outer/inner levels
  - hull shaders without a control point phase (fxc leaves it out when it only copies the input points) pass the
    control points through, using the output signature the runtime gives the driver
  - domain shaders as GL tessellation evaluation shaders. GL declares partitioning, winding and point mode there
    (D3D in the hull shader), so a domain shader is translated for the bound hull shader's settings.
  - D3D defines winding with the domain origin top left, GL bottom left: D3D `triangle_cw` is GL `ccw`
  - patch list topologies, `HS`/`DSSet*` for shaders, resources, samplers and constant buffers
  - Problems found (virglrenderer limitations, worked around in the guest driver; the first two are D3D10 bugs too):
    - Lookup tables read with a run-time index (`static const` arrays, i.e. indexed immediate constant buffers)
      always read the first entry: virglrenderer ignores relative addressing on immediates. Such tables are now
      bound as a hidden constant buffer (gallium slot 0, which D3D's slots 1..14 leave free) and read from there.
      The translator also dropped the offset of `icb[r0.x + n]`.
    - Writes to local arrays at a run-time index (indexable temps) used address register 0 whichever one was named:
      destinations now always get ADDR[0], everything else the other two.
    - A domain shader's final `ret` became a plain `return` before virglrenderer's epilogue, so the y flip never
      ran (upside-down output); the domain shader body now runs in a run-once loop where `ret` is a break.
    - virglrenderer refuses arrays indexed by a system value directly (the pass-through hull shader): it goes
      through an address register.
  - Test: `sm5test.exe` now 48 tests, 12 of them tessellation:
    - quad domain with control points and fork instances
    - triangles with `fractional_odd`
    - pass-through hull shader
    - winding checked with back-face culling
    - point mode
    - two patches with `SV_PrimitiveID` in both stages
    - isolines: WARP's isolines are wrong and change between runs, so this one is checked against the expected
      image (density = `SV_TessFactor[0]`, detail = `[1]`, as fxc labels them)
    - `idx_tables`: lookup tables and local arrays in a pixel shader
    - All pass. Desktop (no DWM crashes), Direct2D (identical to before) checked after a reboot.
- **Feature level 11_0, stage 4 of 4: indirect draws, MSAA, calls; 11_0 for real** (2026-10-14, spike VM, Mesa
  patch 0011):
  - The driver reports feature level 11_0 (and 10_1) to every program when the host can do it: compute and
    tessellation shaders, 4x MSAA, 8 images and shader buffers in pixel and compute shaders. The runtime asks before
    any device exists, so the driver reads the virgl caps itself through the kernel driver's adapter.
    `WINBOAT_D3D_FL11=0` forces 10_0 (troubleshooting), `=1` forces 11_0. DWM runs at 11_0 now.
  - `DrawInstancedIndirect` / `DrawIndexedInstancedIndirect` (arguments written by the CPU or by a compute shader)
  - MSAA: quality levels, multisampled render targets, depth buffers and loads (`Texture2DMS`), `ResolveSubresource`,
    sample masks, `SV_Coverage` in and out, per-sample shading, multisampled swap chains
  - The D3D11.1 format caps (vertex and typed buffers, UAV writes, gather)
  - `call`/`callc`: inlined, since virglrenderer has no TGSI subroutines (such a shader failed and stopped the
    program's rendering). fxc emits calls for `[call] switch` and similar.
  - Problems found:
    - The first MSAA build black-screened the desktop: every device creation failed with
      `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. In d3d11.dll (`CDevice::LLOCompleteLayerConstruction`, found with cdb and
      public symbols) the runtime checks that a typeless format and every format it casts to report the same quality
      levels; the driver answered per format. Sample counts are offered per format family now.
    - DXGI's `Blt` (resolves, format conversion and stretching on present) was a stub.
    - D24S8 depth buffers were never created (a D3D10 bug too): virgl hosts only know GL's packed depth-stencil
      layout (`S8_UINT_Z24_UNORM`), which the driver uses now. Not done: a CPU read of a D24S8 staging copy gets GL's
      byte layout.
    - `SV_Coverage` as a pixel shader input was declared as a compute thread ID (its operand number is among
      theirs), so the shader failed to translate.
    - The D3D11.1 table asked the 10.0 format caps only.
  - Indirect draws: virglrenderer ignores the index buffer offset in indirect draws, so with an offset the indices
    are first copied (on the GPU) to a buffer of their own. Without host support the arguments are read back.
  - Test: `sm5test.exe` now 66 tests, all pass:
    - 4 indirect draws (with start instance, base vertex, an index buffer offset, arguments from a compute shader)
    - 11 MSAA and depth tests compared with WARP (edge pixels within one sample: the host's sample positions differ)
    - quality levels and format caps, which match WARP's apart from video-processor bits and BGRA8 typed UAVs (GL
      has no BGRA images)
    - a 4x swap chain presented to a window
    - the feature level an application gets (11_0 without the switch)
    - `cf_calls` (nested `[call] switch`)
  - Also checked after a reboot: the desktop (no DWM crashes), Direct2D (identical; A8/R8 format caps now equal
    WARP's), GDI/D2D interop, sharing, D3D10.0 and 10.1 devices with MSAA. Still not supported: doubles, deferred
    contexts (the runtime emulates them), interface classes (`fcall`), tiled resources, 11_1.
- **In the user's WinBoat VM, and a DirectX test** (2026-10-14, WinBoat 0.9.2-gpu.35, driver 100.6.101.58506): the
  full driver package with Mesa patches 0008-0011 went into the user's VM the usual way (AppImage, guest server
  update, `/gpu/driver` install and restart). `tools/dx-test` (see Tooling) tests D3D9/10/11/12 feature by feature.
  Result on the user's VM: 367 passed, 27 failed, 26 optional features missing (warnings), 10 skipped (D3D12).
  - Everything of stages 1-4 passes there: shader model 4/5, compute and UAVs, tessellation, indirect draws, MSAA,
    D3D10/10.1 (but stream output), D3D11 formats compared with WARP.
  - Driver bugs it found (also on the spike):
    - D3D9: fixed-function lighting crashes the process; A8L8 textures not offered; volume textures can't be created
      (`D3DERR_INVALIDCALL`); occlusion and event queries return `E_FAIL`.
    - D3D10/11: stream output with `DrawAuto` crashes; rendering to sRGB render targets stores linear values (no
      encoding, even for clears; the same with blending); pipeline statistics queries return zeros.
    - D3D12: no driver (expected).
  - Only on the user's VM (the spike, same driver and display mode, passes): flip-model presents with vsync run at
    ~7 per second (66 on the spike; bit-block transfer ones at ~32), and bit-block transfer windows read black
    through `PrintWindow`, also with WARP. Not the launch path (a scheduled task does the same), not covered windows,
    not GPU load. To look into: the VM's image is still `winboat-windows-spice:6.05-4` (the spike runs 6.05-5).
    Gone with the image update (next item): 64 presents per second with vsync, bit-block transfer windows fine.
- **Fixes for what the DirectX test found** (2026-10-14, WinBoat 0.9.2-gpu.36, driver 100.6.101.58749, Mesa patch
  0012, virglrenderer patch 0003, image `winboat-windows-spice:6.05-6`; the list as worked through:
  `docs/directx-fixes-todo.md`). DirectX test after it, spike and user's VM alike: 395 passed, 1 failed (no D3D12
  driver), 24 optional features missing, 10 skipped.
  - D3D9:
    - Fixed-function lighting: Windows turns fixed-function vertex processing into `vs_2_0` shaders that use `ifc`.
      `vs_2_x`/`ps_2_x` shaders carry the 2.0 version token (the caps enable the 2_x instructions), so nine checks
      2.0 shaders against the 2_x set now; it refused `ifc`, and the `endif` after it underflowed its condition
      stack (found with cdb: the crash was in the translator, under `ff2vs::CConverterToVertexShader`).
    - A8L8: virgl hosts have no L8A8 (GL core profile), so nine keeps it as R8G8 (same bytes) with an `rrrg`
      sampler swizzle.
    - Volume textures: system memory ones (the managed pool's copies) wrap the runtime's memory, and `VolBlt` (what
      UpdateTexture uses for volumes) copies them.
    - Queries: "not ready yet" has to be `S_FALSE`. `d3d9!CD3DDDIDX10::GetQueryData` takes any failure, also
      `D3DERR_WASSTILLDRAWING`, for a lost device and fails the app's GetData with `E_FAIL` (read in its disassembly).
      Timestamp queries are offered now.
  - D3D10/11:
    - Stream output from a vertex shader (no GS): gallium has no "empty geometry shader", virgl crashed on one
      without tokens; the vertex shader gets the declaration (a translation made at draw time for the pair).
    - Buffers with several bind flags (vertex + stream output, vertex + index): virglrenderer only takes them with
      the sampler view flag, refused them otherwise, and the missing resource's first use stopped the context.
      virgl adds the flag.
    - Unbinding stream output sent "one target, handle 0": virglrenderer made an empty transform feedback object of
      it and began it at the next draw (`GL_INVALID_OPERATION`, the context stopped). virgl drops trailing NULL
      targets.
    - `DrawAuto`: d3d10umd passed no draw range (virgl crashed reading it), and the host drew the stream output
      target's size (the buffer's capacity) as a vertex count: the guest can't know how much a capture wrote.
      virglrenderer patch 0003 draws with `glDrawTransformFeedback` and the transform feedback object that last
      captured into vertex buffer 0 (ending its capture first).
    - sRGB render targets: draws were encoded, clears not: the host clears on a framebuffer of its own, where
      `GL_FRAMEBUFFER_SRGB` is whatever the bound framebuffer left. virgl clears a linear view of the surface with
      the encoded color (what `util_clear_render_target` does).
    - Pipeline statistics: D3D11's query types (with HS/DS/CS counts, per-stream SO statistics and overflow
      predicates) weren't known to d3d10umd; and virgl returned one statistic per `PIPE_QUERY_PIPELINE_STATISTICS`
      (the host's queries are per statistic). virgl makes one host query per statistic now and offers
      `PIPE_QUERY_PIPELINE_STATISTICS_SINGLE` for GL. SO statistics are made of the host's primitives
      emitted/generated queries.
    - Timestamp-disjoint queries: virglrenderer doesn't know them, and the query it couldn't make stopped the
      context's rendering. Emulated in the guest (nanoseconds, never disjoint).
    - Not done: `D3D11_SO_NO_RASTERIZED_STREAM` doesn't discard rasterization (apps using it normally bind no
      pixel shader).
  - Lessons: a virgl context error shows only in the container's log (`docker logs`: "context error reported",
    the GL error, the command); every later draw of that device renders nothing, so one bad command looks like many
    failing tests. `VREND_DEBUG=khr` in the compose environment reaches vhost-user-gpu, but NVIDIA's GL printed
    nothing through KHR_debug. Which GL call fails can be checked on the host GPU with a small EGL program
    (EGL device platform; the default display gives llvmpipe). The test's "rendered primitives" statistic isn't
    compared anymore: clipping splits differently (WARP 3, NVIDIA 1).
  - The user's container went to image 6.05-6 (only the image differs from the route gpu.36 sets up: what WinBoat's
    Apply does), after a clean shutdown; the compose file before it is `~/.winboat/docker-compose.yml.before-6.05-6`.
    Its host ports moved (guest server 47281); WinBoat looks them up.

- **Direct3D 11 optional features, Direct2D, complete Direct3D 9 driver, DXVA2 video processing** (2026-10-14,
  WinBoat 0.9.2-gpu.37, driver 100.6.101.58945, Mesa patch 0013, virglrenderer patch 0004, image
  `winboat-windows-spice:6.05-7`; stages A and A2 of `docs/gpu-roadmap-todo.md`). DirectX test, spike and user's VM
  alike: 476 passed, 1 failed (no D3D12 driver yet), 17 optional features missing, 10 skipped.
  - D3D11: feature level 11_1 when the host can (target-independent rasterization and UAV-only rendering with a
    forced sample count: virglrenderer patch 0004 adds `VIRGL_CCMD_SET_RASTER_SAMPLES` for
    `GL_EXT_raster_multisample`; UAVs in every stage; ClearView on UAVs), doubles (including 11.1's extended ones),
    logic ops, B4G4R4A4. Fixed on the way: `SV_ClipDistance` was never applied (Direct2D clips batched geometry
    with it), GDI drawing through `IDXGISurface1::GetDC` was lost (DXGI's ResolveSharedResource was a stub).
    Tiled resources and the 11.3 options need the Vulkan/WDDM 2 route (stage C). New dx-test groups `d2d`/`d2d-32`
    (16 Direct2D scenes against WARP).
  - D3D9 flip model (`D3DSWAPEFFECT_FLIPEX`) crashed in the runtime (`DdBltLH`, a NULL kernel handle). Found with
    cdb and public symbols (`ba w4` on the runtime's adapter table): with no 16-bit display mode format,
    `GetDX8HALCaps` marks the adapter as having no hardware primary, the runtime makes no driver surface for the
    desktop, and the redirected-flip present reads its handle. R5G6B5 is a display mode now; only one 16-bit format
    may be (with X1R5G5B5 too, `FetchDirectDrawData` refuses the driver).
  - D3D9 exclusive full screen: `SetDisplayMode`; a flip chain is one resource with front and back buffers as
    subresources; only the first allocation of a CreateResource may name the runtime's resource (a second
    `AllocateCb` on it bugchecked the VM, 0x113 subtype 7, as in the D3D10 driver before).
  - D3D9Ex shared surfaces opened by a second D3D9 device read black: nine clears a new render target, also the one
    wrapping the other device's content.
  - Lock writes lost after GPU work (LockRect on render targets, GDI drawing through `IDirect3DSurface9::GetDC`):
    upstream virgl turned READ|WRITE maps of staged textures into read-only staging maps, so unmapping dropped the
    writes (D3D10/11 `Map(READ_WRITE)` had the same problem).
  - DXVA2 video processing (what Windows' video renderer EVR uses): progressive and bob processors, YUY2/UYVY/NV12/
    RGB input, BT.601/709/SMPTE 240M, studio and full range, scaling, background color. YUV surfaces are textures
    with the bytes as they are (packed formats as A8R8G8B8 of half width, NV12 as L8 with the chroma rows below):
    a shader model 3 pass converts them pixel for pixel, StretchRect scales; StretchRect from YUV surfaces,
    ColorFill and locks work on them too.
  - Also: DepthFill, ResolveSharedResource, UpdateSubresourceUP, GetInfo and the VCACHE query, the D3DCAPS review
    (ATOC alpha to coverage offered), and the DLLs carry the driver's version (applications saw 0.0.0.0).
  - The D3D10 interface audit found nothing missing that's advertised: driver command lists (the runtime emulates
    deferred contexts), tiled resources and markers aren't offered.
  - Asked on the way: GPU compute and video for applications (Topaz Video AI, ffmpeg): roadmap stage E. CUDA,
    NVENC and TensorRT need NVIDIA's own driver in the VM (a real GPU given to it); the virtual GPU's routes are
    DirectML on the D3D12 driver (Topaz's route on non-NVIDIA GPUs), Vulkan compute/video and D3D11/12 video.
  - Lessons: the runtimes' decisions are in tables filled at Direct3DCreate9 time; a write breakpoint on the field
    the failing path reads finds the code that sets it. The `0013` patch file had been overwritten by a re-export
    with itself applied; regenerated from the tree (patches 0001-0013 verified to reproduce it).

### Vulkan (roadmap stage B), 2026-10-14

Vulkan works in the spike VM on the host's GPU, 64- and 32-bit: `vulkaninfo` lists `Virtio-GPU Venus (NVIDIA
GeForce RTX 3080 Laptop GPU)`, Vulkan 1.3, and the dx-test groups `vulkan`/`vulkan-32` pass (38 checks: memory,
compute, rendering, texture sampling, timeline semaphores, a swapchain presenting to a window). The layers:

- **Host** (QEMU patch 0008, image `winboat-windows-spice:6.05-8`): Venus runs in WinBoat's GPU process
  (vhost-user-gpu `--venus`, virglrenderer's render server), so Vulkan work stays off QEMU's main loop like the
  OpenGL work. The device gets a host-visible memory window with `hostmem=4G` (address space only); blobs are mapped
  into it over vhost-user's SHMEM_MAP. Two QEMU fixes were needed: vhost's memory table must skip those mappings (it
  deadlocked), and the mapping must exist before the backend's reply. NVIDIA's Vulkan driver needs libxext6 in the
  image. A Linux guest with Mesa's Venus checked this before Windows (`~/.cache/claude-winboat/venus-linux`).
- **Kernel driver** (kvm patch 0013): blob resources and the window, mapped into the calling process; four escapes
  (`VIOGPU_BLOB_CREATE/MAP/DESTROY`, `VIOGPU_EXECBUFFER` with an event per ring fence). The blobs belong to the
  D3DKMT device and go away with it, and are attached to its Venus context (Venus looks them up there). A double
  free in the first version (the control queue frees a command's data buffer itself) showed up as kernel heap
  corruption, bugcheck 0x13A, after the first real Vulkan app; the minidump plus `!analyze` pointed at the pool.
- **Mesa** (patch 0014): `vn_renderer_wddm.c` replaces the DRM renderer; Venus' few Linux-isms got Windows
  stand-ins; Win32 WSI runs on its software path (the GPU copies a frame to host-visible memory, GDI draws it). Built
  with clang-cl: Venus uses C11 atomics and `typeof`, which the EWDK's MSVC (14.38) doesn't have. clang-cl needed
  small build fixes (u_atomic.h's MSVC branch, Linux linker-option checks that pass by accident).
- **Package** (kvm patch 0014): the ICDs and manifests (`VulkanDriverName`/`VulkanDriverNameWow`) and the Khronos
  loader into System32/SysWOW64 when there's no newer one (Windows 11 24H2 ships its own; Windows 10 and 8.1
  don't). Already-signed files (the loader) keep their signature when WinBoat test-signs the package.
- **WinBoat**: `virtualGpu.ts` moves the SPICE route to image 6.05-8 and adds `hostmem=4G` to the GPU device; VMs
  set up by older builds show "Apply".

Deployed as gpu.38 (driver 100.6.101.59253, image 6.05-8): in the user's WinBoat VM the DirectX test gives 514
passed, 1 failed (no D3D12), with both Vulkan groups passing.

Faster presents (Mesa patch 0015, after gpu.38): presents were capped at 63 fps because Mesa slept with `Sleep()`,
which lasts a whole 15.6 ms timer tick, and Venus waits for the GPU by polling with short sleeps. With a
high-resolution waitable timer, drawing straight from the mapped frame, and a present thread (FIFO paced by
`DwmFlush`, IMMEDIATE and MAILBOX added), a 1200x781 window goes from 63 to ~1100 fps uncapped and vsync'd apps run
at the display's rate. The copy through the CPU cost ~1 ms per megapixel on the present thread. Since gpu.83 (Mesa
patch 0035, mesa-venus 0009) presents stay on the GPU: each swapchain image has a shared Direct3D 11 texture whose
host storage is Vulkan memory, which Venus imports by its host resource id; a present blits into it and the driver's
own DXGI swap chain shows it. A present costs ~0.4 ms whatever the window size (1440p: 1.58 -> 0.44 ms). Anything
missing falls back to the CPU path; `WINBOAT_VK_PRESENT=copy` forces it.

Not yet: ray tracing, mesh shaders and Vulkan Video (the host's Venus doesn't forward them; video is stage E).

### Tooling (in `tools/`)

- `tools/gpu-spike/setup.sh`: creates the disposable spike VM in `~/winboat-gpu-spike` (own container, storage,
  ports 18006/13389/12222, SSH key login, test signing on, RDP GPU policies set).
- `tools/gpu-spike/setup-build-env.ps1` / `build-driver.ps1`: EWDK 24H2 + Python/Meson toolchain (plus LLVM 18 and
  the Vulkan runtime's loader) in the VM, then builds Mesa (`max8rr8/mesa@viogpu_win`: the D3D/OpenGL drivers with
  MSVC, the Vulkan driver with clang-cl) and the kernel driver (`max8rr8/kvm-guest-drivers-windows@viogpu3d`).
- `tools/gpu-spike/install-driver.ps1`: test-signs the built package, trusts the test certificate and installs it,
  replacing earlier builds (reboot afterwards).
- `tools/gpu-spike/sync-sources.sh`: clones Mesa and the driver at pinned commits on the host, applies
  `tools/gpu-spike/patches/<repo>/*.patch`, and copies the sources into the VM. `build-driver.ps1 -UserModeOnly`
  then rebuilds only Mesa and swaps its DLLs into the installed driver, for quick iterations without a reboot. A DLL
  that is in use (dwm.exe keeps `viogpu_d3d10.dll` loaded) is renamed aside first; new processes get the new one,
  already running ones (DWM) only after a reboot.
- D3D9 driver development: `tools/gpu-spike/push-dev.sh` copies uncommitted work in the clones to the VM: the
  files whose content changed since the last push (hashes in `~/winboat-gpu-spike/pushed-<repo>.sha1`, so a file
  changed back to the pinned version is pushed too), extracted with the VM's current time. The VM's clock doesn't
  match the host's, and with the host's timestamps a pushed file could look older than its object file and silently
  not be recompiled.
  `dev-build.ps1` builds single Mesa targets, `dev-deploy-d3d9.ps1 [-Trace]` swaps `viogpu_d3d9(_x86).dll` into the
  installed driver and registers it, and `[TRACE=0] d3d9-iterate.sh rdp|rdp32|console|console32` runs the whole
  round (push, build, deploy, D3D9 checks, trace). With tracing on, the driver logs every DDI call, the draw state
  and crash stacks (module + offset, to look up in the linker map) to `C:\viogpu_d3d9_trace.log`.
- `tools/gpu-check/`: `winboat-gpucheck.exe`, a Windows test app that checks every graphics path (DXGI, D3D9/11/12,
  OpenGL, Vulkan, video decode, on-screen presentation) with pixel read-back, benchmarks against software rendering,
  and writes a pass/fail checklist (`gpucheck-report.md` + `.json`). Each API group runs in a child process with a
  timeout, so driver crashes and hangs show up as failed checks. It also captures the drivers' `OutputDebugString`
  messages per group (Mesa's only log channel in release builds) and saves a screenshot when a present check fails. `run-in-vm.sh console|rdp|ssh` runs it in the spike
  VM in the chosen session type and copies the report back.
- `tools/dx-test/`: `winboat-dxtest.exe` (+ `winboat-dxtest32.exe`), a feature-by-feature test of Direct3D 9, 10/10.1,
  11 and 12 with a check mark or a cross per test, about 430 tests in 14 groups. Rendering tests read the pixels back:
  D3D10/11/12 compare them with WARP, D3D9 (no WARP) with the expected colors. Optional features that are missing
  show as warnings, not failures. A crashing or hanging test fails and the group goes on after it (the parent process
  restarts the group past it). The D3D11 groups include the driver's development tests (shader model 5, compute,
  tessellation, MSAA). Report: `dxtest-report.html` (+ `.md`, `.json`). `build.sh` cross-compiles it with MinGW;
  `run-in-winboat.sh [--only GROUP,...]` runs it in the WinBoat VM through the guest server (`/desktop/launch`, the
  programs reach Windows through the shared home folder) and writes the report to `dist/results-<time>/`;
  `--warp` runs everything on WARP to check the tests themselves.

### Spike VM compose

Separate name, storage and ports so it can't touch an existing WinBoat VM (`tools/gpu-spike/setup.sh` generates
this, plus SSH and test-signing setup):

```yaml
name: winboat-gpu-spike
services:
  windows:
    image: ghcr.io/dockur/windows:6.05
    container_name: WinBoatGPUSpike
    environment:
      VERSION: "11"
      RAM_SIZE: "8G"
      CPU_CORES: "4"
      DISK_SIZE: "128G"
      GPU: "Y"
      # NVIDIA on non-Debian hosts only, see the spike log
      GBM_BACKENDS_PATH: "/usr/lib/x86_64-linux-gnu/gbm:/usr/lib/gbm:/usr/lib64/gbm"
    devices:
      - /dev/kvm
      - /dev/dri
    cap_add:
      - NET_ADMIN
    ports:
      - 127.0.0.1:18006:8006
      - 127.0.0.1:13389:3389/tcp
      - 127.0.0.1:13389:3389/udp
    volumes:
      - ./storage:/storage
    stop_grace_period: 2m
    # NVIDIA hosts only: injects the proprietary EGL/GBM libraries
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: all
              capabilities: [gpu, graphics, display, utility]
```
