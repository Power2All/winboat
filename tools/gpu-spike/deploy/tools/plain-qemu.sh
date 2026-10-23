#!/bin/bash
# plain-qemu.sh: a copy of the spike's Windows (~/winboat-gpu-spike/plain/data.img) on the host's stock QEMU and
# virglrenderer, as docs/standalone-install.md tells users: virtio-vga-gl with Venus, the distro's OVMF, headless EGL
# with VNC on 127.0.0.1:5905, SSH on 127.0.0.1:12322. Runs in the foreground (run it in the background); the QEMU
# monitor on plain/monitor.sock. PLAIN_VENUS=0 leaves Venus out (the documented minimal setup); PLAIN_GPU=std boots without
# virtio-gpu; PLAIN_DIR picks another disk (data.img + vars.fd, e.g. ~/winboat-gpu-spike/win10); PLAIN_SERIAL=<file>
# writes COM1 to a file (a kernel driver's serial trace); PLAIN_BIOS=1 boots with SeaBIOS (Windows 7: no UEFI
# without CSM); PLAIN_MEM sets the memory (8G)
P=${PLAIN_DIR:-~/winboat-gpu-spike/plain}
MEM=${PLAIN_MEM:-8G}
VENUS="-machine memory-backend=mem -object memory-backend-memfd,id=mem,size=$MEM -device virtio-vga-gl,hostmem=4G,blob=true,venus=true"
[ "${PLAIN_VENUS:-1}" = 0 ] && VENUS="-m $MEM -device virtio-vga-gl"
# no virtio-gpu (the driver not loaded: to repair a VM whose driver doesn't boot)
[ "${PLAIN_GPU:-}" = std ] && VENUS="-m $MEM -vga std"
FIRMWARE="-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd -drive if=pflash,format=raw,file=$P/vars.fd"
[ "${PLAIN_BIOS:-}" = 1 ] && FIRMWARE=""
exec qemu-system-x86_64 -machine q35,accel=kvm -cpu host -smp 4 $VENUS \
   $FIRMWARE \
   -drive file=$P/data.img,id=d0,format=raw,if=none,cache=none,aio=native \
   -device virtio-scsi-pci,id=scsi0 -device scsi-hd,drive=d0,bus=scsi0.0 \
   -netdev user,id=n0,hostfwd=tcp:127.0.0.1:12322-:22 -device virtio-net-pci,netdev=n0 \
   -device qemu-xhci -device usb-tablet \
   -display egl-headless,rendernode=/dev/dri/renderD128 -vnc 127.0.0.1:5 \
   -monitor unix:$P/monitor.sock,server=on,wait=off -serial ${PLAIN_SERIAL:+file:}${PLAIN_SERIAL:-none}
