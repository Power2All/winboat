#!/bin/bash
# testvm-prep.sh <win11|win10|win81|win7> [--keep-running]: boots the virt-manager test VM (libvirt session), waits
# for SSH (bounded), shows its display adapter and test mode, puts the newest standalone setup on the Public desktop
# (Windows 7: test mode off, as a user's 7 is), takes a screenshot (results/testvm-<v>.png) and shuts it down
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
v=$1
name=winboat-test-$v
case $v in win11) port=12611 ;; win10) port=12610 ;; win81) port=12681 ;; win7) port=12607 ;; *) exit 2 ;; esac
export SP_PORT=$port
[ $v = win7 ] && export SP_HTTP=1   # (7's OpenSSH hangs in SFTP)
V="virsh -c qemu:///session"
SETUP=$(ls -t ~/Coding/winboat/tools/standalone-installer/dist/*-setup.exe | head -1)
state() { $V domstate $name 2>/dev/null | head -1; }
ssh-keygen -q -R "[127.0.0.1]:$port" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
[ "$(state)" = running ] || $V start $name >/dev/null || exit 1
up=no
for i in $(seq 1 40); do SP_TIMEOUT=10 "$T/sp.sh" 'echo up' 2>/dev/null | grep -q up && { up=yes; break; }; echo "$v: waiting for SSH ($i)"; sleep 10; done
[ $up = yes ] || { echo "$v: no SSH" >&2; exit 1; }
sleep 20
# (single quotes only in what Windows runs: Windows 10's built-in OpenSSH drops double quotes)
SP_TIMEOUT=60 "$T/sp.sh" "\$v = Get-WmiObject Win32_VideoController | Select-Object -First 1; 'display: ' + \$v.Name + ' ' + \$v.CurrentHorizontalResolution + 'x' + \$v.CurrentVerticalResolution; 'test mode: ' + ((Get-ItemProperty HKLM:\\SYSTEM\\CurrentControlSet\\Control).SystemStartOptions -match 'TESTSIGNING'); 'explorer: ' + [bool](Get-Process explorer -ErrorAction SilentlyContinue)" | tr -d '\r'
if [ $v = win7 ]; then
   SP_TIMEOUT=30 "$T/sp.sh" 'bcdedit /set testsigning off | Out-Null; "test mode off at the next boot"' | tr -d '\r'
fi
SP_TIMEOUT=300 "$T/sp.sh" --put "$SETUP" "C:/Users/Public/Desktop/$(basename "$SETUP")" || true
SP_TIMEOUT=30 "$T/sp.sh" "Get-ChildItem C:\\Users\\Public\\Desktop\\*.exe | % { 'on the desktop: ' + \$_.Name + ' ' + \$_.Length }" | tr -d '\r'
mkdir -p "$T/results"
timeout 30 $V screenshot $name "$T/results/testvm-$v.ppm" >/dev/null 2>&1 && echo "screenshot: $T/results/testvm-$v.ppm"
[ "${2:-}" = --keep-running ] && exit 0
$V shutdown $name >/dev/null
for i in $(seq 1 36); do [ "$(state)" = "shut off" ] && { echo "$v: shut off"; exit 0; }; sleep 5; done
echo "$v: didn't shut down in 3 min, forced off"; $V destroy $name >/dev/null
