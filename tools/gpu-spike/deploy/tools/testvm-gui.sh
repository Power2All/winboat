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
# psf <seconds> <script>: PowerShell run from a file in the VM (Windows 10's inbox OpenSSH drops double quotes from
# commands, which broke the inline ones)
psf() {
   local f; f=$(mktemp --suffix=.ps1); printf '%s\r\n' "$2" > "$f"
   SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/step.ps1 >/dev/null; rm -f "$f"
   SP_TIMEOUT=$1 "$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Public\step.ps1' | tr -d '\r'
}
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
# SETUP_ARGS (default /S): "/S /REBOOT" lets the setup restart Windows itself (then waited for here)
args=${SETUP_ARGS:-/S}
if [[ "$args" == */REBOOT* ]]; then
   # Windows' boot time before and after: a missed SSH answer during the busy install isn't the restart (it was taken
   # for one on Windows 7, and the driver checked before the setup had finished)
   boot() { SP_TIMEOUT=20 "$T/sp.sh" '(Get-WmiObject Win32_OperatingSystem).LastBootUpTime' 2>/dev/null | tr -d '\r' | grep -o '^[0-9]\{14\}' ; }
   b0=$(boot)
   # waited for (Windows' OpenSSH ends a session's processes with it: a setup merely started was killed); the
   # connection then drops with the restart
   t0=$(date +%s)
   psf 900 '$s = Get-ChildItem C:\Users\Public\Desktop\*-setup.exe | Select-Object -First 1; "setup " + $s.Name + " running: '"$args"'"; $p = Start-Process $s.FullName -ArgumentList "'"$args"'" -Wait -PassThru; "setup exit " + $p.ExitCode'
   echo "$v: setup returned after $(( $(date +%s) - t0 )) s"
   b1=$b0
   for _ in $(seq 1 90); do sleep 10; b1=$(boot); [ -n "$b1" ] && [ "$b1" != "$b0" ] && break; done
   [ -n "$b1" ] && [ "$b1" != "$b0" ] || { echo "$v: Windows didn't restart within 15 min (boot time $b0)"; exit 1; }
   echo "$v: Windows restarted by the setup (boot $b0 -> $b1), $(( $(date +%s) - t0 )) s after the setup started"
   sleep 30
else
   psf 900 '$s = Get-ChildItem C:\Users\Public\Desktop\*-setup.exe | Select-Object -First 1; $p = Start-Process $s.FullName -ArgumentList "'"$args"'" -Wait -PassThru; "setup " + $s.Name + " exit " + $p.ExitCode'
fi
[[ "$args" == */REBOOT* ]] || { SP_TIMEOUT=20 "$T/sp.sh" 'shutdown /r /t 1' >/dev/null 2>&1; sleep 40; up || exit 1; sleep 30; }
psf 60 '$v = Get-WmiObject Win32_VideoController | Select-Object -First 1; "display: " + $v.Name + " " + $v.DriverVersion + " status " + $v.Status; "shortcut: " + (Test-Path "C:\Users\Public\Desktop\WinBoat GPU Check.lnk")'
# The screen grab and the shortcut, as tasks in the user's session
psf 60 '$shot = @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
`$b = [Windows.Forms.SystemInformation]::VirtualScreen
`$i = New-Object Drawing.Bitmap `$b.Width, `$b.Height
[Drawing.Graphics]::FromImage(`$i).CopyFromScreen(`$b.Left, `$b.Top, 0, 0, `$i.Size)
`$i.Save("C:\Users\Public\shot.png")
"@
Set-Content C:\Users\Public\shot.ps1 $shot
schtasks /Create /F /TN WinBoatShot /TR "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\shot.ps1" /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
Set-Content C:\Users\Public\gui.cmd ("@start """" ""C:\Users\Public\Desktop\WinBoat GPU Check.lnk""") -Encoding ASCII
schtasks /Create /F /TN WinBoatGui /TR C:\Users\Public\gui.cmd /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
Remove-Item "$env:SystemDrive\Users\spike\AppData\Local\WinBoat\GPU Check" -Recurse -ErrorAction SilentlyContinue
schtasks /Run /TN WinBoatGui | Out-Null; "opened"'

sleep 25; shot running
J='C:\Users\spike\AppData\Local\WinBoat\GPU Check\gpu-check.json'
for i in $(seq 1 60); do SP_TIMEOUT=15 "$T/sp.sh" "if (Test-Path '$J') { [bool]((Get-Content '$J') -match 'complete.: true') }" 2>/dev/null | grep -q True && { echo "$v: check complete"; break; }; sleep 10; done
sleep 3; shot done
SP_TIMEOUT=60 "$T/sp.sh" --get "C:/Users/spike/AppData/Local/WinBoat/GPU Check/gpu-check.md" "$T/results/gui-$v-gpu-check.md" >/dev/null && grep -m1 'passed' "$T/results/gui-$v-gpu-check.md"
