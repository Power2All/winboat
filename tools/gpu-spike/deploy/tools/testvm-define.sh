#!/bin/bash
# testvm-define.sh <win11|win10|win81|win7>: the fresh test VM's disk (testvm-install.sh's; Windows 7: a copy of
# win7/storage/data.img.fresh) moved into libvirt's session images and defined there, so it opens from Virtual Machine
# Manager (QEMU/KVM User session): Video Virtio with 3D acceleration, QEMU rendering it (egl-headless) and SPICE showing
# its frames (SPICE's own OpenGL showed nothing in virt-manager on KDE Wayland with NVIDIA), plus Venus's hostmem/blob/venus as a QEMU property override, and SSH for
# the spike's tools on 127.0.0.1:<port> (a QEMU user network: libvirt's session forwards ports only through passt)
set -euo pipefail
v=$1
case $v in
   win11) title="Windows 11"; port=12611; mem=8; efi=1 ;;
   win10) title="Windows 10"; port=12610; mem=8; efi=1 ;;
   win81) title="Windows 8.1"; port=12681; mem=4; efi=1 ;;
   win7) title="Windows 7"; port=12607; mem=4; efi=0 ;;
   *) echo "usage: $0 win11|win10|win81|win7" >&2; exit 2 ;;
esac
name=winboat-test-$v
usb=qemu-xhci; [ $v = win7 ] && usb=ich9-ehci1   # (Windows 7 has no USB 3 driver: no tablet, a PS/2 mouse to grab)
S=~/winboat-gpu-spike/$v/storage
IMG=~/.local/share/libvirt/images
NVRAM=~/.local/share/libvirt/qemu/nvram
mkdir -p $IMG $NVRAM
virsh -c qemu:///session dominfo $name >/dev/null 2>&1 && { echo "$name is defined already" >&2; exit 1; }
if [ $v = win7 ]; then
   cp --sparse=always $S/data.img.fresh $IMG/$name.img
else
   mv $S/data.img $IMG/$name.img
   mv $S/windows.vars $NVRAM/${name}_VARS.fd
fi
loader=""
[ $efi = 1 ] && loader="<loader readonly='yes' type='pflash' format='raw'>/usr/share/edk2/x64/OVMF_CODE.4m.fd</loader>
    <nvram format='raw'>$NVRAM/${name}_VARS.fd</nvram>"
XML=$(mktemp)
cat > $XML <<EOF
<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
  <name>$name</name>
  <title>$title (WinBoat GPU driver test)</title>
  <description>Fresh $title without the driver: the setup and the GPU check are on its desktop</description>
  <memory unit='GiB'>$mem</memory>
  <memoryBacking><source type='memfd'/><access mode='shared'/></memoryBacking>
  <vcpu>4</vcpu>
  <os>
    <type arch='x86_64' machine='q35'>hvm</type>
    $loader
    <boot dev='hd'/>
  </os>
  <features><acpi/><apic/></features>
  <cpu mode='host-passthrough'/>
  <clock offset='localtime'/>
  <devices>
    <emulator>/usr/bin/qemu-system-x86_64</emulator>
    <controller type='scsi' model='virtio-scsi'/>
    <disk type='file' device='disk'>
      <driver name='qemu' type='raw' cache='none' io='native'/>
      <source file='$IMG/$name.img'/>
      <target dev='sda' bus='scsi'/>
    </disk>
    <controller type='usb' model='$usb'/>
    <input type='tablet' bus='usb'/>
    <graphics type='spice'>
      <listen type='none'/>
    </graphics>
    <graphics type='egl-headless'>
      <gl rendernode='/dev/dri/renderD128'/>
    </graphics>
    <video>
      <model type='virtio' heads='1' primary='yes'><acceleration accel3d='yes'/></model>
    </video>
    <channel type='spicevmc'><target type='virtio' name='com.redhat.spice.0'/></channel>
    <sound model='ich9'><audio id='1'/></sound>
    <audio id='1' type='spice'/>
  </devices>
  <qemu:commandline>
    <qemu:arg value='-netdev'/>
    <qemu:arg value='user,id=wbnet,hostfwd=tcp:127.0.0.1:$port-:22'/>
    <qemu:arg value='-device'/>
    <qemu:arg value='virtio-net-pci,netdev=wbnet,bus=pcie.0,addr=0x10'/>
  </qemu:commandline>
  <qemu:override>
    <qemu:device alias='video0'>
      <qemu:frontend>
        <qemu:property name='hostmem' type='unsigned' value='4294967296'/>
        <qemu:property name='blob' type='bool' value='true'/>
        <qemu:property name='venus' type='bool' value='true'/>
      </qemu:frontend>
    </qemu:device>
  </qemu:override>
</domain>
EOF
virsh -c qemu:///session define $XML
rm $XML
echo "$name: ssh on 127.0.0.1:$port"
