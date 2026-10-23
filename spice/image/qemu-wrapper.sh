#!/bin/bash
# Installed as /usr/bin/qemu-system-x86_64 in WinBoat's SPICE image, in front of the real QEMU.
#
# When the VM's arguments ask for the virtual GPU in its own process (WinBoat adds
# "-device vhost-user-vga,chardev=winboat-gpu -chardev socket,id=winboat-gpu,path=<socket>"), this starts QEMU's
# vhost-user-gpu on that socket first and gives the guest's memory to QEMU as a shared memfd, which vhost-user needs.
# virglrenderer's GL work then runs in that process: on QEMU's main loop (QEMU's own virtio-vga-gl) it held up the
# sound card and the vCPUs whenever the guest drew a lot, and the sound crackled. Any other command line goes to QEMU
# unchanged.
REAL=/usr/libexec/qemu-system-x86_64  # next to share/qemu, where QEMU looks for its ROMs
GPU=/usr/libexec/vhost-user-gpu

socket="" memory="" rendernode=/dev/dri/renderD128 venus="" prev=""
for arg in "$@"; do
    case "$arg" in
        vhost-user-vga,*hostmem=*|vhost-user-gpu-pci,*hostmem=*)
            # A host-visible memory window: Vulkan (Venus) with blob resources
            venus=--venus
            ;;
        socket,id=winboat-gpu,path=*)
            socket=${arg#*path=}
            socket=${socket%%,*}
            ;;
        *rendernode=*)
            rendernode=${arg##*rendernode=}
            rendernode=${rendernode%%,*}
            ;;
    esac
    [ "$prev" = "-m" ] && memory=${arg%%,*}
    prev=$arg
done

if [ -z "$socket" ]; then
    exec -a "$0" "$REAL" "$@"
fi
if [ -z "$memory" ]; then
    echo "winboat: the vhost-user GPU needs the memory size (-m)" >&2
    exit 1
fi

# One GPU process per QEMU run: it serves a single connection and exits when QEMU goes away
pidfile="${socket}.pid"
if [ -f "$pidfile" ]; then
    kill "$(cat "$pidfile")" 2>/dev/null
fi
rm -f "$socket"
# Video (virgl's VA-API): the NVIDIA driver's VA-API driver by name, in case libva doesn't map the DRM driver to it
if [ "$(basename "$(readlink -f "/sys/class/drm/${rendernode##*/}/device/driver" 2>/dev/null)")" = nvidia ]; then
    export LIBVA_DRIVER_NAME=nvidia
fi
"$GPU" --socket-path="$socket" --virgl $venus --render-node="$rendernode" &
echo $! > "$pidfile"
for _ in $(seq 1 200); do
    [ -S "$socket" ] && break
    if ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "winboat: vhost-user-gpu didn't start" >&2
        exit 1
    fi
    sleep 0.05
done

exec -a "$0" "$REAL" "$@" \
    -object "memory-backend-memfd,id=winboat-mem,size=$memory,share=on" -machine memory-backend=winboat-mem
