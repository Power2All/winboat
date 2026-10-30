#!/bin/bash
# testvm-dialog.sh <win7|win81|win10>: what the user's desktop shows right after a silent setup (no restart): the
# setup run, a screenshot of the user's session 10 s and 40 s later (results/dialog-<v>-*.png), the driver install
# log's prompts; the VM put back as it was. Bounded
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
v=$1; name=winboat-test-$v
case $v in win10) port=12610 ;; win81) port=12681 ;; win7) port=12607 ;; *) exit 2 ;; esac
export SP_PORT=$port; [ $v = win7 ] && export SP_HTTP=1
V="virsh -c qemu:///session"
IMG=~/.local/share/libvirt/images/$name.img
NV=~/.local/share/libvirt/qemu/nvram/${name}_VARS.fd
state() { $V domstate $name 2>/dev/null | head -1; }
down() { $V shutdown $name >/dev/null 2>&1; for _ in $(seq 1 36); do [ "$(state)" = "shut off" ] && return; sleep 5; done; $V destroy $name >/dev/null; }
up() { for i in $(seq 1 40); do SP_TIMEOUT=10 "$T/sp.sh" 'echo up' 2>/dev/null | grep -q up && return 0; sleep 10; done; return 1; }
psf() { local f; f=$(mktemp --suffix=.ps1); printf '%s\r\n' "$2" > "$f"; SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/step.ps1 >/dev/null; rm -f "$f"
        SP_TIMEOUT=$1 "$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Public\step.ps1' | tr -d '\r'; }
shot() { SP_TIMEOUT=30 "$T/sp.sh" "Remove-Item C:\\Users\\Public\\shot.png -ErrorAction SilentlyContinue; schtasks /Run /TN WinBoatShot | Out-Null" >/dev/null
         for _ in $(seq 1 15); do SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\Users\Public\shot.png' 2>/dev/null | grep -q True && break; sleep 2; done
         sleep 2; SP_TIMEOUT=60 "$T/sp.sh" --get C:/Users/Public/shot.png "$T/results/dialog-$v-$1.png" && echo "screenshot: $T/results/dialog-$v-$1.png"; }
[ "$(state)" = "shut off" ] || down
cp --sparse=always $IMG $IMG.before-dialog; [ -f $NV ] && cp $NV $NV.before-dialog
restore() { down; mv $IMG.before-dialog $IMG; [ -f $NV.before-dialog ] && mv $NV.before-dialog $NV; echo "$v: restored as prepared"; }
trap restore EXIT
ssh-keygen -q -R "[127.0.0.1]:$port" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
$V start $name >/dev/null && up || exit 1; sleep 20
psf 60 '$shot = @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
`$b = [Windows.Forms.SystemInformation]::VirtualScreen
`$i = New-Object Drawing.Bitmap `$b.Width, `$b.Height
[Drawing.Graphics]::FromImage(`$i).CopyFromScreen(`$b.Left, `$b.Top, 0, 0, `$i.Size)
`$i.Save("C:\Users\Public\shot.png")
"@
Set-Content C:\Users\Public\shot.ps1 $shot
schtasks /Create /F /TN WinBoatShot /TR "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\shot.ps1" /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
"ready"'
psf 600 '$s = Get-ChildItem C:\Users\Public\Desktop\*-setup.exe | Select-Object -First 1; $p = Start-Process $s.FullName -ArgumentList "/S" -Wait -PassThru; "setup exit " + $p.ExitCode'
sleep 10; shot a
sleep 30; shot b
psf 60 '$log = "$env:SystemRoot\inf\setupapi.dev.log"; $lines = Get-Content $log; $i = [Math]::Max(0, $lines.Count - 400); $lines[$i..($lines.Count - 1)] | Select-String -Pattern "prompt|unsigned|trust|signature|publisher|Exit status|Driver package|viogpu|Error|!!!" | Select-Object -Last 40 | ForEach-Object { $_.Line }'
