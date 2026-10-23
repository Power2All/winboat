#!/bin/bash
# win7-build.sh [--full]: one test VM at a time (the user's memory): the Windows 7 VM (plain-qemu.sh, if running)
# is shut down, the spike started, the sources pushed, and the 7 package made (win7-package.sh; WIN7_DEFINES
# passes through). --full first does the full build and the export (user-mode drivers changed). The spike is stopped
# again in any case. Stops at the first failure (a build that doesn't end in "build exit: 0" too)
set -uo pipefail
# the 7 VM's QEMU (by process name: a command line merely mentioning the disk isn't one)
vm7() { for p in $(pgrep -x qemu-system-x86); do grep -qa win7/storage/data.img /proc/$p/cmdline && return 0; done; return 1; }
T=$(dirname "$0")
stop_spike() { "$T/../guard.sh" 200 180 "docker stop -t 150 WinBoatGPUSpike" >/dev/null; }
trap stop_spike EXIT
if vm7; then
   SP_PORT=12322 SP_TIMEOUT=30 "$T/sp.sh" 'Stop-Computer -Force' >/dev/null 2>&1
   for _ in $(seq 1 36); do vm7 || break; sleep 5; done
   vm7 && { echo "the 7 VM didn't stop" >&2; exit 1; }
fi
docker start WinBoatGPUSpike >/dev/null
sleep 20
for i in $(seq 1 60); do SP_TIMEOUT=15 "$T/sp.sh" "echo up" 2>/dev/null | grep -q up && break; sleep 8; done
~/Coding/winboat/tools/gpu-spike/push-dev.sh mesa | tail -1
~/Coding/winboat/tools/gpu-spike/push-dev.sh kvm-guest-drivers-windows | tail -1
"$T/sp.sh" --put ~/Coding/winboat/tools/gpu-spike/build-driver.ps1 C:/OEM/build-driver.ps1  # (its -Win7)
if [ "${1:-}" = --full ]; then
   out=$("$T/full-build.sh" 2>&1)
   echo "$out" | grep 'build exit\|error C\|error LNK' | sort -u | head -8
   echo "$out" | grep -q 'build exit: 0' || { echo "full build failed" >&2; exit 1; }
   ~/Coding/winboat/tools/gpu-spike/export-driver-package.sh >/dev/null 2>&1 || { echo "export failed" >&2; exit 1; }
   grep DriverVer ~/Coding/winboat/guest_server/drivers/viogpu3d/viogpu3d.inf
fi
SP_TIMEOUT=900 "$T/win7-package.sh" 2>&1 | grep -i 'error\|viogpu3d.sys'
