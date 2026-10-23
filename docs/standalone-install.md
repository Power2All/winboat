# The WinBoat GPU driver in your own QEMU Windows VM

`winboat-gpu-driver-<version>-setup.exe` (built by `tools/standalone-installer/build.sh`) installs WinBoat's virtual GPU
driver in any Windows 10 (1709 or later) / 11 x64 VM on QEMU/KVM (Windows 8.1 and 7 SP1 x64 too, with less: below): Direct3D 9/10/11 and OpenGL on the host's GPU through
virgl, Direct3D 12 and Vulkan through Venus, video decoding through the host's VA-API, and the GPU check (Start menu and desktop:
"WinBoat GPU Check": a window with a row per API, green when it works, red when it doesn't, grey when the host's GPU
doesn't offer it; a click on a row lists its checks, "Full report" opens every result). The driver works with any host GPU: what the host can't do is reported as not supported.

## The host side

The VM needs a virtio-gpu device with 3D and a display that does OpenGL:

| Setup | Settings |
|---|---|
| QEMU command line | `-device virtio-vga-gl -display gtk,gl=on` (or `sdl,gl=on`; SPICE: `-spice gl=on,...`; headless: `-display egl-headless` with VNC) |
| virt-manager / libvirt | Video: model Virtio, "3D acceleration" on; Display: SPICE, "OpenGL" on, listen type None (if the console stays black, e.g. NVIDIA on Wayland: SPICE without OpenGL plus an `<graphics type='egl-headless'>` device, below) |
| Proxmox | Hardware > Display: "VirGL GPU" (needs `libgl1` and `libegl1` on the host) |
| GNOME Boxes | 3D acceleration on in the VM's preferences |

Vulkan and Direct3D 12 also need Venus (QEMU 9.2 or later, virglrenderer built with `-Dvenus=true`):

    -machine memory-backend=mem -object memory-backend-memfd,id=mem,size=8G \
    -device virtio-vga-gl,hostmem=4G,blob=true,venus=true

Without it, Direct3D 9/10/11 and OpenGL work and Direct3D 12/Vulkan report "not supported".

With libvirt (virt-manager), Venus also needs QEMU's seccomp sandbox off: Venus runs in a render server process that
virglrenderer starts, which the sandbox forbids (spawn=deny), and then 3D fails altogether (the VM's log: "failed to
initialize venus renderer", "virgl could not be initialized"). `seccomp_sandbox = 0` in `/etc/libvirt/qemu.conf` (or
`~/.config/libvirt/qemu.conf` for the user session), then the libvirt daemon restarted. The Venus properties go in a
QEMU override of the video device, as libvirt has no attributes for them:

    <qemu:override>
      <qemu:device alias='video0'>
        <qemu:frontend>
          <qemu:property name='hostmem' type='unsigned' value='4294967296'/>
          <qemu:property name='blob' type='bool' value='true'/>
          <qemu:property name='venus' type='bool' value='true'/>
        </qemu:frontend>
      </qemu:device>
    </qemu:override>

(with `xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'` on `<domain>`, and `<memoryBacking><source
type='memfd'/><access mode='shared'/></memoryBacking>`).

A black console in virt-manager with SPICE's OpenGL on (seen on KDE Wayland with an NVIDIA GPU, the guest running
fine behind it): the viewer can't show the GPU buffers SPICE hands it. Let QEMU render instead and SPICE send plain
frames; the guest's 3D (virgl, Venus) is the same:

    <graphics type='spice'><listen type='none'/></graphics>
    <graphics type='egl-headless'><gl rendernode='/dev/dri/renderD128'/></graphics>

What stock QEMU and virglrenderer lack and WinBoat's image has (`spice/image`: QEMU and virglrenderer with patches, the
GPU in its own process). Measured on QEMU 11.1.2 and virglrenderer 1.3.0 as Arch ships them, NVIDIA host, the flags
above with Venus, Windows 11 (2026-10-21): the GPU check passes 624 tests and fails 4, and these are missing:

- video decoding (QEMU doesn't turn virgl's video on, and virglrenderer's video path needs patches 0006/0007 for NVIDIA
  hosts): programs decode on the CPU instead
- feature level 11_1 (rasterizer sample counts, virglrenderer 0004; programs get 11_0) and D3D10/11 stream output
  counts (0003)
- OpenGL above 4.2 (compute shaders, persistent buffer maps: virglrenderer 0009/0010)
- the 11.3 render target/viewport index from vertex and domain shaders (0016)
- through Venus: Direct3D 12 ExecuteIndirect with state changes (root constants, vertex/index buffers) draws and
  dispatches with wrong arguments (VK_EXT_device_generated_commands, 0013: the 4 failed tests, 64- and 32-bit),
  ExecuteIndirect tier 1.1, shader model 6.8's SampleCmpGrad (VK_KHR_maintenance8, 0011), Vulkan Video
- Direct3D 12 swap chains copy each frame (zero-copy needs 0012/0017; ~2 ms a present at 1440p instead of 0.1)
- smooth sound under heavy drawing (stock QEMU runs virglrenderer on its main loop: prefer 60 Hz and moderate
  resolutions)

NVIDIA hosts need `nvidia-drm.modeset=1`.

## Installing

1. **Turn Secure Boot off** in the VM: the driver is test-signed, and Windows won't turn on test mode under Secure Boot
   (OVMF: press Esc at boot, Device Manager > Secure Boot Configuration; libvirt: a firmware without `secboot`). Setup
   stops with this message when it's on.
2. Run the setup (or `setup.exe /S` for a silent install) and restart. Windows shows a "Test Mode" watermark from then
   on.

Setup signs the driver with a certificate made in this VM and trusts only that one; the private key is deleted right
after signing. It also lets RDP sessions use the GPU and installs the SPICE agent (resolution, later the clipboard).
Everything it changes is recorded and put back by the uninstaller (Settings > Apps > "WinBoat GPU Driver", or
`uninstall.exe /S`), which also turns test mode off if it was off before.

## Not covered (yet)

Windows 8.0, 32-bit Windows, Windows on ARM, attestation signing (no test mode): see standalone-gpu-driver.md.

Windows 10 (tested on 22H2 with a stock host, 2026-10-21): Direct3D 12 runs at feature level 12_1 / shader model 6.5
(as far as Windows 10's runtime goes), the boot crash with Venus is fixed (driver 58724 and later), and so are the GPU
timeouts after boot and the slow presents (58985 and later), and 32-bit Direct3D 9/11 programs no longer run out of
address space (100.6.101.58024 of 10/22/2026 and later), and Direct2D draws curves right with WinBoat's host (58118 of
10/22/2026 and later). The GPU check there: 609 passed, 4 failed on a stock host (the ExecuteIndirect state changes, as
on Windows 11).

Windows 8.1 (tested on 8.1 Enterprise with a stock host, 2026-10-22): the setup installs a separate WDDM 1.3 package
there, which runs Direct3D 9/10/11 at feature level 11_0, Direct2D and OpenGL; no Direct3D 12, Vulkan or video encoding.
The GPU check comes with the Universal CRT it needs (8.1 may not have it). The GPU check there: 476 passed, 4 failed
(where 8.1's WARP, the reference, differs from later Windows' WARP). Uninstalling puts the device back on the 2D
driver. The setup carries the 8.1 package when `tools/standalone-installer/win81/` is there (the spike's
`win81-package.sh` makes it).

Windows 7 SP1 x64 (tested on 7 Ultimate with a stock host, 2026-10-23): the setup installs a separate WDDM 1.1 package
there, with Direct3D 9 and 9Ex, Direct3D 10, Direct3D 11 up to feature level 10_0 (Windows 7's runtime knows no newer
interface of the driver's) and OpenGL; no Direct3D 12, Vulkan or video. Use `-device virtio-vga-gl` (the BIOS boot
display) with `hostmem=` (as in the Venus flags above): Windows 7 keeps the desktop's surface in device memory, which
is the top 64 MB of that window; without it, virtio-vga's 8 MB VGA memory limits the desktop to about 1680x1050. The driver files are SHA-1 signed (7 without KB3033929 can't check SHA-2). Shader compiling (HLSL, which
most Direct3D 10/11 programs need) comes with KB4019990 (D3DCompiler_47, Microsoft Update Catalog); without it the GPU
check skips those tests. Known: windowed Direct3D 9 presents don't reach the screen yet (full screen and Direct3D 9Ex do). The setup
carries the 7 package when `tools/standalone-installer/win7/` is there (the spike's `win7-package.sh`), and needs
`tools/standalone-installer/ucrt/` for the GPU check on 7 and 8.1 (`win81-installer-files.sh`).
