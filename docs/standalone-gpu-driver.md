# A standalone GPU driver for Windows VMs: feasibility (future work)

Written 2026-10-13, nothing of this is started. The question: can WinBoat's GPU driver (viogpu3d with the Mesa
user-mode drivers: Direct3D 9/10/11 and OpenGL on the host's GPU) and its audio be installed in any Windows 11/10 VM
(VirtualBox, VMware, QEMU), and also Windows 7/8, ideally with a simple MSI?

## Short answer

| Where | Possible? | Why |
|---|---|---|
| QEMU/KVM with virgl (plain QEMU, libvirt/virt-manager, GNOME Boxes, Proxmox "VirGL GPU", Quickemu) | **Yes**, Windows 10/11 first | The driver needs a virtio-gpu device with 3D (virgl), which only QEMU has |
| UTM (macOS) | Only Windows on ARM, later | UTM has virtio-gpu with virgl (over ANGLE/Metal), but needs an ARM64 build of the driver; x86 Windows there is emulated and too slow |
| VirtualBox | **No** | No virtio-gpu device: its graphics are VMSVGA/VBoxSVGA with its own 3D driver (Guest Additions) |
| VMware Workstation/ESXi | **No** | Its SVGA 3D device has its own driver (VMware Tools, DirectX 11/OpenGL 4.3) |
| Hyper-V | **No** | No virtio devices (GPU-P is a different thing) |
| Windows 8.1 | **Done** (2026-10-22) | A WDDM 1.3 build with the current EWDK, in the setup: Direct3D 9/10/11 and OpenGL (gpu-roadmap-todo.md, D) |
| Windows 8.0 | Possible, more work | WDDM 1.2: the newer interfaces have to be optional |
| Windows 7 | **Done** (2026-10-23), test mode only | A WDDM 1.1 build, in the setup: Direct3D 9/10, 11 up to feature level 10_0, OpenGL (gpu-roadmap-todo.md, D); no way to sign a new kernel driver for it anymore |

Audio needs no driver of ours anywhere: WinBoat's VM uses QEMU's emulated Intel HD Audio (`ich9-intel-hda` +
`hda-duplex`), which Windows 7 and later drive with their own HD Audio driver. What made WinBoat's sound good is host
side (the GPU in a process of its own, the viewer's buffering), not something to install in Windows. VirtualBox and
VMware bring their own sound devices and drivers.

## Why only QEMU

The driver talks to the virtio-gpu device (PCI 1af4:1050, `virtio-vga-gl` / `virtio-gpu-gl` / `vhost-user-vga`) and
sends virgl's command streams, which the host's virglrenderer turns into OpenGL on the host's GPU. Windows only loads
it for that device (the INF matches its PCI ID). VirtualBox and VMware emulate other hardware with other protocols:
supporting them would mean writing another driver for each, while each already ships one. Nothing is gained there.

What QEMU users need on the host:
- QEMU with virglrenderer and a display that does GL: `-display gtk,gl=on` / `-display sdl,gl=on`, SPICE with
  `gl=on` and a local client (virt-manager, remote-viewer), `-display egl-headless` (+ VNC), or D-Bus.
- The device: `-device virtio-vga-gl` (or virt-manager: Video model "Virtio" with "3D acceleration", SPICE with
  OpenGL; Proxmox: Display "VirGL GPU").
- RDP from inside Windows also uses the GPU (the policy the installer sets, `bEnumerateHWBeforeSW`).

Caveat: with stock QEMU, virglrenderer runs on QEMU's main loop. Heavy drawing at large resolutions then delays the
emulated sound card (the crackling WinBoat had, see hardware-acceleration.md). WinBoat's image avoids it with
`vhost-user-gpu` and QEMU/virglrenderer patches (spice/image/patches, spice/image/virglrenderer-patches). For other
QEMU users those fixes should go upstream (QEMU: patches 0002-0007; virglrenderer: 0001-0002); until then the
documentation recommends 60 Hz and moderate resolutions.

## Host operating systems

The Windows driver doesn't change with the host: what the host's QEMU and virglrenderer offer does (to verify
hands-on before relying on it):
- **Linux**: everything. The OpenGL path (our D3D9/10/11 and OpenGL drivers) works today; Venus (Vulkan, then
  DXVK/vkd3d-proton) is made for Linux hosts (the proprietary NVIDIA driver is the hardest case for its memory
  sharing). WinBoat's separate GPU process (vhost-user-gpu) is Linux-only too.
- **Windows** (QEMU with the Windows Hypervisor Platform): the OpenGL path is a project of its own (virglrenderer
  builds for Windows, QEMU's GL display path there is experimental). Venus: no (its render server relies on Linux).
- **macOS** (UTM/QEMU): virglrenderer runs on OpenGL ES through ANGLE on Metal, short of desktop-GL features our
  D3D10/11 driver uses; x86 Windows is emulated there, so Windows on ARM only (an ARM64 build). Venus over
  MoltenVK: prototypes at best.

So the OpenGL-based (native) route is the portable one; Vulkan is effectively for Linux hosts. That favors native
Shader Model 5 (feature level 11_0) for a driver meant for any host, and Venus first for WinBoat on Linux.

## The MSI

One MSI per architecture (x64 first; 32-bit Windows needs a 32-bit kernel driver build, which doesn't exist yet),
built with WiX (virtio-win's own guest tools MSI is WiX too, a model to follow):
- installs the driver package (INF, SYS, CAT, the Mesa DLLs, 64- and 32-bit user-mode parts) with
  `pnputil /add-driver /install` (Windows 10/11; Windows 7/8 need DPInst or DIFx instead, pnputil there is older)
- replaces virtio-win's 2D driver (viogpudo) on the device, and removes ours cleanly on uninstall
- sets the RDP hardware GPU policy, optionally the SPICE agent (vdagent) and the resolution helper (viogpuap)
- asks for the restart

**Signing is the real decision**, not the MSI:
- Today the package is test-signed: Windows has to run in test mode (`bcdedit /set testsigning on`, Secure Boot
  off, a "Test Mode" watermark). WinBoat does that inside its own VM. For other people's VMs an installer that turns
  off Secure Boot protection isn't something to hand out lightly.
- Windows 10/11 load kernel drivers signed by Microsoft only. The way for a project like this is **attestation
  signing** through the Microsoft Partner Center: needs an EV code signing certificate (a few hundred euros a
  year, tied to a company or person) and the Hardware Developer Program; then the driver loads with Secure Boot on,
  no test mode. (WHQL certification would add the HLK test runs; not needed for attestation.)
- Windows 7/8.x: attestation-signed drivers don't load there, and cross-signing (the old way) ended in 2021. Only
  test mode remains (or WHQL, which isn't realistic). So Windows 7/8 would stay a test-mode option.

## Windows 8.1, 8.0 and 7: what changes

- **Toolchain**: we build with the EWDK for Windows 11 24H2, which only targets Windows 10 and later. Windows 8.1/7
  builds need an older WDK (the Windows 10 2004-era WDK with Visual Studio 2019 still targets them; to be checked)
  and an older Windows SDK for the user-mode parts. virtio-win itself no longer builds for Windows 7/8.
- **Kernel driver**:
  - 8.1: build with `TargetVersion` 8.1; the driver's interface is WDDM 1.3 already. Check the code under
    `NTDDI_VERSION >= NTDDI_WIN10` (VIRTIO_F_ACCESS_PLATFORM) and anything newer the KMD uses.
  - 8.0: WDDM 1.2 interface (`DXGKDDI_INTERFACE_VERSION_WIN8`), drop or gate the 1.3 parts.
  - 7: WDDM 1.1: no `DxgkCbAcquirePostDisplayOwnership` / `StopDeviceAndReleasePostDisplayOwnership` /
    `SystemDisplayEnable/Write` (all Windows 8), so boot and the crash screen go through the VGA side of virtio-vga
    instead; the boot display hand-over and the VidPN code need a Windows 7 path.
- **User-mode drivers (Mesa)**:
  - d3d10umd: we added the D3D11.1 and WDDM 1.3 interfaces (Windows 8+); it still offers the D3D10/10.1 ones, which
    is what Windows 7's runtime and DWM use (Windows 7's DWM draws with D3D10.1). d3d10umd started out as VMware's
    Windows 7 driver, so the base fits.
  - The D3D11 runtime of Windows 7 asks for the 11.0 DDI, which we compile out today (we went 10.x -> 11.1).
  - Our D3D9 driver: Windows 7's D3D9Ex paths (DWM doesn't need them, apps may).
  - OpenGL (WGL ICD): fine on Windows 7.
- **WinBoat-specific parts** stay out of the standalone package or become optional: the GDI interop fixes and caption
  buttons are Windows 10/11 matters; the guest server, seamless windows and the SPICE port are WinBoat's.

## Plan (when we pick this up)

1. **Windows 10/11 on QEMU** (the big win, smallest step)
   - Test the current package on this machine in a plain libvirt/virt-manager VM (virtio-vga-gl, SPICE with OpenGL)
     and a Proxmox-like setup (VirGL GPU display), test mode first.
   - The WiX MSI (x64), with uninstall; a short page per host (QEMU command line, virt-manager, Proxmox, Boxes).
   - Decide on signing (attestation needs the EV certificate first).
   - Upstream the QEMU/virglrenderer patches, so stock QEMU gets the separate GPU process without crackling.
2. **Windows 8.1**: done (the current EWDK at the 8.1 API level; standalone-install.md).
3. **Windows 8.0**: WDDM 1.2 interface.
4. **Windows 7**: WDDM 1.1 kernel paths, the D3D11.0 DDI in d3d10umd, test mode only.
5. Later: 32-bit Windows (x86 kernel driver), Windows on ARM for UTM (ARM64 build).

Testing on this machine: QEMU/KVM with libvirt next to WinBoat's VM (the NVIDIA GPU shared through virgl as it is
now), Windows 10/11/8.1/7 ISOs. VirtualBox/VMware only to confirm the driver doesn't apply there (no matching
device), not as targets.
