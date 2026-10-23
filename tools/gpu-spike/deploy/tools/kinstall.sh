#!/bin/bash
# kinstall.sh: pushes the kernel driver sources to the spike, builds the kernel driver only (C:\build\kbuild.ps1),
# installs the package and restarts the spike
T=$(dirname "$0")
~/Coding/winboat/tools/gpu-spike/push-dev.sh kvm-guest-drivers-windows | tail -1
"$T/sp.sh" 'Remove-Item -Recurse -Force C:\build\src\kvm-guest-drivers-windows\viogpu\viogpu3d\objfre_win10_amd64 -ErrorAction SilentlyContinue; powershell -NoProfile -ExecutionPolicy Bypass -File C:\build\kbuild.ps1' 2>&1 | tr -d '\r' | grep -iE "error|exit|warning C" | tail -8
"$T/install-built.sh"
