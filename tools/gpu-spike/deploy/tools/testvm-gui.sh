#!/bin/bash
# testvm-gui.sh <win11|win10|win81|win7>: the GPU check's window tried on a prepared test VM (testvm-prep.sh), which is
# put back as it was afterwards: its disk (and UEFI variables) copied aside, the desktop's setup run silently, a
# restart, the desktop's "WinBoat GPU Check" shortcut opened in the user's session, screenshots taken there while it
# runs and when its report is complete (results/gui-<v>-*.png), then shut down and the copies restored. Bounded
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
v=$1
name=winboat-test-$v
case $v in win11) port=12611 ;; win10) port=12610 ;; win81) port=12681 ;; win7) port=12607 ;; *) exit 2 ;; esac
export SP_PORT=$port
[ $v = win7 ] && export SP_HTTP=1
V="virsh -c qemu:///session"
IMG=~/.local/share/libvirt/images/$name.img
NV=~/.local/share/libvirt/qemu/nvram/${name}_VARS.fd
state() { $V domstate $name 2>/dev/null | head -1; }
down() { $V shutdown $name >/dev/null 2>&1; for _ in $(seq 1 36); do [ "$(state)" = "shut off" ] && return; sleep 5; done; $V destroy $name >/dev/null; }
up() { for i in $(seq 1 40); do SP_TIMEOUT=10 "$T/sp.sh" 'echo up' 2>/dev/null | grep -q up && return 0; sleep 10; done; echo "$v: no SSH" >&2; return 1; }
# screenshot <tag>: the user's screen, captured in their session
shot() {
   SP_TIMEOUT=30 "$T/sp.sh" "Remove-Item C:\\Users\\Public\\shot.png -ErrorAction SilentlyContinue; schtasks /Run /TN WinBoatShot | Out-Null" >/dev/null
   for _ in $(seq 1 15); do SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\Users\Public\shot.png' 2>/dev/null | grep -q True && break; sleep 2; done
   sleep 2; SP_TIMEOUT=60 "$T/sp.sh" --get C:/Users/Public/shot.png "$T/results/gui-$v-$1.png" && echo "screenshot: $T/results/gui-$v-$1.png"
}
[ "$(state)" = "shut off" ] || down
cp --sparse=always $IMG $IMG.before-gui; [ -f $NV ] && cp $NV $NV.before-gui
restore() { down; mv $IMG.before-gui $IMG; [ -f $NV.before-gui ] && mv $NV.before-gui $NV; echo "$v: restored as prepared"; }
trap restore EXIT
ssh-keygen -q -R "[127.0.0.1]:$port" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
$V start $name >/dev/null && up || exit 1
echo "$v: installing from the desktop's setup"
SP_TIMEOUT=900 "$T/sp.sh" '$s = Get-ChildItem C:\Users\Public\Desktop\*-setup.exe | Select-Object -First 1; $p = Start-Process $s.FullName -ArgumentList "/S" -Wait -PassThru; "setup exit " + $p.ExitCode' | tr -d '\r'
SP_TIMEOUT=20 "$T/sp.sh" 'shutdown /r /t 1' >/dev/null 2>&1; sleep 40; up || exit 1; sleep 30
SP_TIMEOUT=60 "$T/sp.sh" '$v = Get-WmiObject Win32_VideoController | Select-Object -First 1; "display: " + $v.Name + " " + $v.DriverVersion; "shortcut: " + (Test-Path "C:\Users\Public\Desktop\WinBoat GPU Check.lnk")' | tr -d '\r'
# The screen grab and the shortcut, as tasks in the user's session
SP_TIMEOUT=60 "$T/sp.sh" "Set-Content C:\\Users\\Public\\shot.ps1 'Add-Type -AssemblyName System.Windows.Forms, System.Drawing; \$b = [Windows.Forms.SystemInformation]::VirtualScreen; \$i = New-Object Drawing.Bitmap \$b.Width, \$b.Height; [Drawing.Graphics]::FromImage(\$i).CopyFromScreen(\$b.Left, \$b.Top, 0, 0, \$i.Size); \$i.Save(\"C:\\Users\\Public\\shot.png\")'
schtasks /Create /F /TN WinBoatShot /TR 'powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\\Users\\Public\\shot.ps1' /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
Set-Content C:\\Users\\Public\\gui.cmd ('@start \"\" \"C:\\Users\\Public\\Desktop\\WinBoat GPU Check.lnk\"') -Encoding ASCII
schtasks /Create /F /TN WinBoatGui /TR C:\\Users\\Public\\gui.cmd /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
Remove-Item \"\$env:SystemDrive\\Users\\spike\\AppData\\Local\\WinBoat\\GPU Check\" -Recurse -ErrorAction SilentlyContinue
schtasks /Run /TN WinBoatGui | Out-Null; 'opened'" | tr -d '\r'
sleep 25; shot running
J='C:\Users\spike\AppData\Local\WinBoat\GPU Check\gpu-check.json'
for i in $(seq 1 60); do SP_TIMEOUT=15 "$T/sp.sh" "if (Test-Path '$J') { [bool]((Get-Content '$J') -match '\"complete\": true') }" 2>/dev/null | grep -q True && { echo "$v: check complete"; break; }; sleep 10; done
sleep 3; shot done
SP_TIMEOUT=60 "$T/sp.sh" --get "C:/Users/spike/AppData/Local/WinBoat/GPU Check/gpu-check.md" "$T/results/gui-$v-gpu-check.md" >/dev/null && grep -m1 'passed' "$T/results/gui-$v-gpu-check.md"
