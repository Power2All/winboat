#!/usr/bin/env bash
# Builds the standalone driver setup (winboat-gpu-driver-<version>-setup.exe) for plain QEMU Windows VMs from the
# exported driver package (tools/gpu-spike/export-driver-package.sh fills guest_server/drivers). makensis runs in a
# Debian container, so nothing is installed on the host.
#
#   tools/standalone-installer/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$HERE/../.." && pwd)"
INF="$REPO_DIR/guest_server/drivers/viogpu3d/viogpu3d.inf"
for dir in viogpu3d spice gpucheck opencl; do
    [ -d "$REPO_DIR/guest_server/drivers/$dir" ] || { echo "No guest_server/drivers/$dir: run tools/gpu-spike/export-driver-package.sh first" >&2; exit 1; }
done
VERSION="$(sed -n 's/^DriverVer *= *[^,]*, *\([0-9.]*\).*/\1/p' "$INF" | tr -d '\r')"
[ -n "$VERSION" ] || { echo "No DriverVer in $INF" >&2; exit 1; }

docker image inspect winboat-nsis >/dev/null 2>&1 ||
    printf 'FROM debian:trixie\nRUN apt-get update && apt-get install -y --no-install-recommends nsis && rm -rf /var/lib/apt/lists/*\n' |
    docker build -q -t winboat-nsis - >/dev/null

mkdir -p "$HERE/dist"
OUT="winboat-gpu-driver-$VERSION-setup.exe"
# Windows 8.1 and 7 too when their parts are there (the spike's win81-package.sh and win7-package.sh: the WDDM 1.3 and
# 1.1 packages, the Universal CRT both need for the GPU check)
OLD=()
[ -f "$HERE/win81/viogpu3d/viogpu3d.inf" ] && OLD+=(-DWIN81=/build/win81) || echo "No win81/: no Windows 8.1 in the setup"
[ -f "$HERE/win7/viogpu3d/viogpu3d.inf" ] && OLD+=(-DWIN7=/build/win7) || echo "No win7/: no Windows 7 in the setup"
if [ ${#OLD[@]} -gt 0 ]; then
    [ -d "$HERE/ucrt/x64" ] || { echo "No ucrt/: run the spike's win81-installer-files.sh" >&2; exit 1; }
    OLD+=(-DUCRT=/build/ucrt)
fi
# The "Before you install" page's text: what the VM needs, the Windows versions this setup carries, what it changes
{
    echo "WinBoat GPU Driver $VERSION"
    echo
    echo "Hardware graphics for this VM on the host's GPU, through QEMU's virtio-gpu with 3D (virgl)."
    echo
    echo "WHAT THE VM NEEDS"
    echo
    echo "A virtio-gpu 3D device:"
    echo "- QEMU: -device virtio-vga-gl, and a display with gl=on (or -display egl-headless)"
    echo "- virt-manager: Video \"Virtio\" with \"3D acceleration\" on; Display \"Spice server\", listen type None, OpenGL on"
    echo "- Proxmox: Display \"VirGL GPU\""
    echo "- GNOME Boxes: 3D acceleration on in the VM's preferences"
    echo
    echo "Secure Boot off: Windows doesn't load test-signed drivers under Secure Boot."
    echo
    echo "Direct3D 12 and Vulkan also need Venus (QEMU 9.2 or later, virglrenderer with Venus):"
    echo "  -machine memory-backend=mem -object memory-backend-memfd,id=mem,size=<the VM's memory>"
    echo "  -device virtio-vga-gl,hostmem=4G,blob=true,venus=true"
    echo "Without Venus, Direct3D 9-11 and OpenGL work; Direct3D 12 and Vulkan programs say \"not supported\"."
    echo
    echo "VENUS UNDER LIBVIRT (virt-manager, GNOME Boxes): SECCOMP"
    echo
    echo "Venus starts a helper process on the host (virgl_render_server). libvirt runs QEMU in a seccomp sandbox that forbids starting processes, so with Venus on, ALL 3D fails (the VM's log says \"failed to initialize venus renderer\"). Turn the sandbox off on the host:"
    echo "1. In /etc/libvirt/qemu.conf (system VMs) or ~/.config/libvirt/qemu.conf (user session VMs, qemu:///session), add the line:"
    echo "     seccomp_sandbox = 0"
    echo "2. Shut the VMs down and restart libvirt: sudo systemctl restart virtqemud (or libvirtd). A user session's daemon: pkill -u \$USER virtqemud (it starts again by itself)."
    echo "3. Start the VM. Check: the QEMU command line (ps -ef | grep qemu) has no -sandbox ...spawn=deny."
    echo "This turns off a security layer around every QEMU libvirt starts on that host; leave it on if you don't need Direct3D 12/Vulkan. Plain QEMU: just don't pass -sandbox with spawn=deny."
    echo
    echo "WINDOWS VERSIONS (64-bit)"
    echo
    echo "- Windows 10 1709 or later, Windows 11: Direct3D 9-12, OpenGL, Vulkan, video decoding"
    [ -f "$HERE/win81/viogpu3d/viogpu3d.inf" ] && echo "- Windows 8.1: Direct3D 9-11 and OpenGL"
    [ -f "$HERE/win7/viogpu3d/viogpu3d.inf" ] && echo "- Windows 7 SP1: Direct3D 9-11 (feature levels 10_1 and 11_0 with the Platform Update, KB2670838), OpenGL. Give the device hostmem= for desktops above about 1680x1050. Shader compiling (most Direct3D 10/11 programs) needs Microsoft's KB4019990."
    echo
    echo "WHAT SETUP CHANGES"
    echo
    echo "- The driver is test-signed: setup turns on Windows test mode (\"Test Mode\" shows on the desktop) and signs the driver with a certificate made on this machine, whose private key is deleted right after."
    echo "- Remote Desktop sessions use the GPU, at up to 60 frames a second."
    echo "- The SPICE guest agent and a display resolution helper (Windows 8.1 and later)."
    echo "- The GPU check: \"WinBoat GPU Check\" on the desktop and in the Start menu shows what works."
    echo "- A restart is needed."
    echo
    echo "Uninstalling (Settings > Apps, or Programs and Features) reverts all of it. Test mode stays on only if it was on before."
    echo
    echo "IF SOMETHING'S WRONG"
    echo
    echo "- The console stays black in virt-manager (e.g. NVIDIA on Wayland): SPICE without OpenGL, plus an egl-headless display (<graphics type='egl-headless'>)."
    echo "- A failed silent install (/S) writes why to %TEMP%\\winboat-gpu-driver-setup.log."
    echo "- More: docs/standalone-install.md in WinBoat's source."
} | sed 's/$/\r/' > "$HERE/dist/readme.txt"
docker run --rm --user "$(id -u):$(id -g)" -v "$REPO_DIR/guest_server:/src:ro" -v "$HERE:/build" winboat-nsis \
    makensis -V2 -DVERSION="$VERSION" -DSOURCE=/src "${OLD[@]}" -DREADME=/build/dist/readme.txt \
    -DOUTFILE="/build/dist/$OUT" /build/installer.nsi
echo "Built $HERE/dist/$OUT"
