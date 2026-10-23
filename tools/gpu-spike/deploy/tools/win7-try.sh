#!/bin/bash
# win7-try.sh: the Windows 7 VM's kernel driver swapped for win7/package/viogpu3d.sys (booted on std VGA, so the
# installed one doesn't load; W7_UMD=1: the user-mode drivers too), then booted with virtio-gpu: SSH waited for (bounded), the screen captured to
# $SHOT (default win7/screen.png) and COM1's last lines shown. The VM stays up (a bluescreen stays on screen)
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
W=~/winboat-gpu-spike/win7
export SP_PORT=12322 SP_HTTP=1
SHOT=${SHOT:-$W/screen.png}
vm7() { for p in $(pgrep -x qemu-system-x86); do grep -qa win7/storage /proc/$p/cmdline && return 0; done; return 1; }
quit7() { echo quit | timeout 5 socat - UNIX-CONNECT:$W/storage/monitor.sock >/dev/null 2>&1; for _ in $(seq 1 20); do vm7 || return 0; sleep 2; done; return 1; }
boot7() {
   rm -f $W/com1.log
   (env "$@" PLAIN_BIOS=1 PLAIN_MEM=4G PLAIN_VENUS=0 PLAIN_SERIAL=$W/com1.log PLAIN_DIR=$W/storage setsid nohup "$T/plain-qemu.sh" >/dev/null 2>&1 &)
   sleep 30
   for i in $(seq 1 25); do SP_TIMEOUT=10 "$T/sp.sh" "echo up" 2>/dev/null | grep -q up && { echo "SSH up after $((30 + i * 8)) s"; return 0; }; echo "waiting for SSH ($i)"; sleep 8; done
   echo "no SSH"; return 1
}
# (the 8.1 VM answers on the same port with another host key)
ssh-keygen -q -R "[127.0.0.1]:12322" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
quit7 || { echo "the 7 VM doesn't quit" >&2; exit 1; }
boot7 PLAIN_GPU=std || exit 1
# the kernel driver, and with W7_UMD=1 the user-mode drivers too (_x86 ones to SysWOW64): nothing has them loaded now
files=$W/package/viogpu3d.sys; [ "${W7_UMD:-}" = 1 ] && files=$(ls $W/package/*.sys $W/package/*.dll)
for f in $files; do
   n=$(basename $f); case $n in *.sys) d='C:\Windows\System32\drivers' ;; *_x86.dll) d='C:\Windows\SysWOW64' ;; *) d='C:\Windows\System32' ;; esac
   SP_TIMEOUT=120 "$T/sp.sh" --put $f C:/Users/spike/$n || exit 1
   SP_TIMEOUT=60 "$T/sp.sh" "Copy-Item C:\\Users\\spike\\$n $d\\$n -Force; 'swapped: $n ' + (Get-Item $d\\$n).Length" | tr -d '\r'
done
SP_TIMEOUT=20 "$T/sp.sh" 'shutdown /s /t 1' >/dev/null 2>&1
for i in $(seq 1 30); do vm7 || break; echo "waiting for the shutdown ($i)"; sleep 3; done
quit7
boot7
timeout 20 gvnccapture 127.0.0.1:5 "$SHOT" >/dev/null 2>&1 && echo "screen: $SHOT"
echo "COM1 ($(stat -c %s $W/com1.log) bytes):"; tr -d '\r' < $W/com1.log | grep -a -v '^\s*$' | tail -${COM_LINES:-12} | cut -c1-170 || true
