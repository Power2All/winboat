#!/bin/bash
# tvm.sh <win10|win81|win7> <command> [args]: hands on a test VM's desktop session (libvirt session VMs, SSH
# through sp.sh). Commands:
#   start                 boots it, waits for SSH, sets up the screenshot/run tasks in the user's session
#   stop                  shuts it down (bounded, then forced)
#   put <file> <C:/path>  copies a file in
#   ps '<PowerShell>'     runs PowerShell from a file (Windows 10's OpenSSH drops double quotes from commands)
#   run '<cmd line>'      starts a program in the user's session (a scheduled task), returns at once
#   keys '<SendKeys>' [title]  sends keystrokes (WScript.Shell SendKeys syntax) to the session's foreground window,
#                         or to the window whose title starts with [title] (activated first)
#   click <x> <y>         a left click at that screen position (in the user's session)
#   shot <name>           the user's screen to results/tvm-<v>-<name>.png
# Every remote call is bounded
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
v=$1; cmd=$2; shift 2
name=winboat-test-$v
case $v in win10) port=12610 ;; win81) port=12681 ;; win7) port=12607 ;; *) echo "usage: $0 win10|win81|win7 ..." >&2; exit 2 ;; esac
export SP_PORT=$port; [ $v = win7 ] && export SP_HTTP=1
V="virsh -c qemu:///session"
state() { $V domstate $name 2>/dev/null | head -1; }
up() { for _ in $(seq 1 40); do SP_TIMEOUT=10 "$T/sp.sh" 'echo up' 2>/dev/null | grep -q up && return 0; sleep 10; done; echo "$v: no SSH" >&2; return 1; }
psf() {
   local f; f=$(mktemp --suffix=.ps1); printf '%s\r\n' "$2" > "$f"
   SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/tvm-step.ps1 >/dev/null; rm -f "$f"
   SP_TIMEOUT=$1 "$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Public\tvm-step.ps1' | tr -d '\r'
}
case $cmd in
start)
   SP_TIMEOUT=60 "$T/sp.sh" --put "$T/tvm-click.ps1" C:/Users/Public/click.ps1 >/dev/null 2>&1 || true
   [ "$(state)" = running ] || { ssh-keygen -q -R "[127.0.0.1]:$port" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1; $V start $name >/dev/null; }
   up || exit 1
   psf 60 '$shot = @"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
`$b = [Windows.Forms.SystemInformation]::VirtualScreen
`$i = New-Object Drawing.Bitmap `$b.Width, `$b.Height
[Drawing.Graphics]::FromImage(`$i).CopyFromScreen(`$b.Left, `$b.Top, 0, 0, `$i.Size)
`$i.Save("C:\Users\Public\shot.png")
"@
Set-Content C:\Users\Public\shot.ps1 $shot
schtasks /Create /F /TN WinBoatShot /TR "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\shot.ps1" /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
$keys = @"
`$l = Get-Content C:\Users\Public\keys.txt
`$sh = New-Object -ComObject WScript.Shell
if (`$l[0]) { [void]`$sh.AppActivate(`$l[0]); Start-Sleep -Milliseconds 700 }
`$sh.SendKeys(`$l[1])
"@
Set-Content C:\Users\Public\keys.ps1 $keys
schtasks /Create /F /TN WinBoatClick /TR "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\click.ps1" /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
schtasks /Create /F /TN WinBoatKeys /TR "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\keys.ps1" /SC ONCE /ST 23:59 /RU spike /IT | Out-Null
"ready"'
   ;;
stop)
   $V shutdown $name >/dev/null 2>&1
   for _ in $(seq 1 36); do [ "$(state)" = "shut off" ] && { echo "$v: shut off"; exit 0; }; sleep 5; done
   $V destroy $name >/dev/null; echo "$v: forced off"
   ;;
put) SP_TIMEOUT=900 "$T/sp.sh" --put "$1" "$2" ;;
ps) psf 900 "$1" ;;
run)
   f=$(mktemp); printf '@start "" %s\r\n' "$1" > "$f"
   SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/tvm-run.cmd >/dev/null; rm -f "$f"
   psf 60 'schtasks /Create /F /TN WinBoatRun /TR C:\Users\Public\tvm-run.cmd /SC ONCE /ST 23:59 /RU spike /IT | Out-Null; schtasks /Run /TN WinBoatRun | Out-Null; "started"'
   ;;
keys)
   f=$(mktemp); printf '%s\r\n%s\r\n' "${2:-}" "$1" > "$f"
   SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/keys.txt >/dev/null; rm -f "$f"
   SP_TIMEOUT=30 "$T/sp.sh" 'schtasks /Run /TN WinBoatKeys' >/dev/null; sleep 2; echo "keys sent"
   ;;
click)
   f=$(mktemp); printf '%s %s\r\n' "$1" "$2" > "$f"
   SP_TIMEOUT=60 "$T/sp.sh" --put "$f" C:/Users/Public/click.txt >/dev/null; rm -f "$f"
   SP_TIMEOUT=30 "$T/sp.sh" 'schtasks /Run /TN WinBoatClick' >/dev/null; sleep 2; echo "clicked $1,$2"
   ;;
shot)
   SP_TIMEOUT=30 "$T/sp.sh" 'Remove-Item C:\Users\Public\shot.png -ErrorAction SilentlyContinue; schtasks /Run /TN WinBoatShot | Out-Null' >/dev/null
   for _ in $(seq 1 15); do SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\Users\Public\shot.png' 2>/dev/null | grep -q True && break; sleep 2; done
   sleep 2; SP_TIMEOUT=60 "$T/sp.sh" --get C:/Users/Public/shot.png "$T/results/tvm-$v-$1.png" >/dev/null && echo "$T/results/tvm-$v-$1.png"
   ;;
*) echo "unknown command $cmd" >&2; exit 2 ;;
esac
