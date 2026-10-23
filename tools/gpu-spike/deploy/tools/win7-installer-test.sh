#!/bin/bash
# win7-installer-test.sh <step>: the standalone setup (tools/standalone-installer/dist, newest) on the Windows 7 VM.
# Steps: boot (virtio-gpu, PLAIN_GPU=std: without; quits a running 7 VM first), install (setup /S, then Status), reboot (Status, screen to
# win7/screen.png), check (the installed GPU check in the desktop session: results/gpu-check-win7.md), uninstall
# (uninstall /S, then Status), status. Every remote call is bounded; Windows not answering 5 times in a row is given up
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd); . ~/winboat-gpu-spike/credentials
W=~/winboat-gpu-spike/win7
export SP_PORT=12322 SP_HTTP=1
SETUP=$(ls -t ~/Coding/winboat/tools/standalone-installer/dist/*-setup.exe | head -1)
DIR='C:\Program Files\WinBoat\GPU Driver'
PS='powershell -NoProfile -ExecutionPolicy Bypass -File'
vm7() { for p in $(pgrep -x qemu-system-x86); do grep -qa win7/storage /proc/$p/cmdline && return 0; done; return 1; }
quit7() { echo quit | timeout 5 socat - UNIX-CONNECT:$W/storage/monitor.sock >/dev/null 2>&1; for _ in $(seq 1 20); do vm7 || return 0; sleep 2; done; return 1; }
up() { for i in $(seq 1 30); do SP_TIMEOUT=10 "$T/sp.sh" "echo up" 2>/dev/null | grep -q up && { echo "SSH up"; return 0; }; echo "waiting for SSH ($i)"; sleep 8; done; echo "no SSH"; return 1; }
status() { SP_TIMEOUT=120 "$T/sp.sh" "$PS '$DIR\\scripts\\gpu-driver.ps1' -Action Status" | tr -d '\r'; }
# waits for a file in the VM (a desktop-session task's), giving up on 5 missed answers in a row
await() {
   local lost=0
   for i in $(seq 1 "$2"); do
      r=$(SP_TIMEOUT=15 "$T/sp.sh" "Test-Path '$1'" 2>/dev/null | tr -d '\r')
      [ "$r" = True ] && return 0
      if [ -z "$r" ]; then lost=$((lost + 1)); [ $lost -ge 5 ] && { echo "Windows doesn't answer: hung or bugchecked" >&2; return 2; }; else lost=0; fi
      echo "waiting ($i)"; sleep 4
   done
   return 1
}
ssh-keygen -q -R "[127.0.0.1]:12322" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
case "${1:-}" in
boot)
   quit7 || { echo "the 7 VM doesn't quit" >&2; exit 1; }
   rm -f $W/com1.log
   (env ${PLAIN_GPU:+PLAIN_GPU=$PLAIN_GPU} PLAIN_BIOS=1 PLAIN_MEM=4G PLAIN_VENUS=${PLAIN_VENUS:-0} PLAIN_SERIAL=$W/com1.log PLAIN_DIR=$W/storage setsid nohup "$T/plain-qemu.sh" >/dev/null 2>&1 &)
   sleep 30; up ;;
install)
   echo "setup: $(basename "$SETUP")"
   SP_TIMEOUT=300 "$T/sp.sh" --put "$SETUP" C:/Users/$WIN_USER/setup.exe || exit 1
   SP_TIMEOUT=600 "$T/sp.sh" "\$p = Start-Process C:\\Users\\$WIN_USER\\setup.exe -ArgumentList '/S' -Wait -PassThru; 'setup exit ' + \$p.ExitCode" | tr -d '\r'
   status ;;
reboot)
   SP_TIMEOUT=20 "$T/sp.sh" 'shutdown /r /t 1' >/dev/null 2>&1
   sleep 40; up || exit 1
   sleep 20; status
   timeout 20 gvnccapture 127.0.0.1:5 $W/screen.png >/dev/null 2>&1 && echo "screen: $W/screen.png" ;;
check)
   # the Start menu shortcut's command, in the desktop session, its report next to it
   SP_TIMEOUT=30 "$T/sp.sh" "Remove-Item -Recurse -Force C:\\build\\gc -ErrorAction SilentlyContinue; New-Item -ItemType Directory C:\\build\\gc | Out-Null
Set-Content C:\\build\\gc\\gc.cmd ('cd /d \"C:\\Program Files\\WinBoat\\GPU Check\"', 'winboat-dxtest.exe --check --out C:\\build\\gc > C:\\build\\gc\\console.txt 2>&1', 'echo done > C:\\build\\gc\\done') -Encoding ASCII
schtasks /Create /F /TN WinBoatGpuCheck /TR C:\\build\\gc\\gc.cmd /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoatGpuCheck | Out-Null" | tr -d '\r'
   await 'C:\build\gc\done' 150 || exit $?
   SP_TIMEOUT=30 "$T/sp.sh" 'Get-Content C:\build\gc\console.txt' | tr -d '\r' | tail -15
   SP_TIMEOUT=60 "$T/sp.sh" --get C:/build/gc/gpu-check.md "$T/results/gpu-check-win7.md" && echo "report: $T/results/gpu-check-win7.md" ;;
uninstall)
   # (_?= keeps the uninstaller in place, so it's waited for)
   SP_TIMEOUT=600 "$T/sp.sh" "Copy-Item '$DIR\\scripts\\gpu-driver.ps1' C:\\Users\\$WIN_USER\\gpu-driver.ps1; \$p = Start-Process '$DIR\\uninstall.exe' -ArgumentList '/S', '_?=$DIR' -Wait -PassThru; 'uninstall exit ' + \$p.ExitCode" | tr -d '\r'
   SP_TIMEOUT=120 "$T/sp.sh" "$PS C:\\Users\\$WIN_USER\\gpu-driver.ps1 -Action Status -PackageDir C:\\none" | tr -d '\r' ;;
status) status ;;
*) echo "usage: $0 boot|install|reboot|check|uninstall|status" >&2; exit 1 ;;
esac
