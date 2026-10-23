#!/bin/bash
# win81-test.sh [dx-test args]: the Windows 8.1 VM on the host's QEMU (plain-qemu.sh, COM1 to win81/com1.log; started
# if it isn't running), the package of win81-package.sh swapped in (w81swap.ps1) and Windows restarted, then dx-test
# (DXTEST_PLAINCMD: 8.1 has no headless console); the report in results/dxtest-report-win81-plain.md
set -uo pipefail
# the 8.1 VM's QEMU (by process name: a command line merely mentioning the disk isn't one)
vm81() { for p in $(pgrep -x qemu-system-x86); do grep -qa win81/storage/data.img /proc/$p/cmdline && return 0; done; return 1; }
T=$(cd "$(dirname "$0")" && pwd)
export SP_PORT=12322
waitssh() { for _ in $(seq 1 60); do SP_TIMEOUT=15 "$T/sp.sh" "echo up" 2>/dev/null | grep -q up && return 0; sleep 8; done; return 1; }
ssh-keygen -q -R "[127.0.0.1]:12322" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
if ! vm81; then
   rm -f ~/winboat-gpu-spike/win81/com1.log
   (PLAIN_SERIAL=$HOME/winboat-gpu-spike/win81/com1.log PLAIN_DIR=$HOME/winboat-gpu-spike/win81/storage setsid nohup \
      "$T/plain-qemu.sh" >/dev/null 2>&1 &)
   sleep 25
fi
waitssh || { echo "no SSH" >&2; exit 1; }
SP_TIMEOUT=60 "$T/sp.sh" 'Remove-Item -Recurse -Force C:\Users\spike\win81pkg -ErrorAction SilentlyContinue; New-Item -ItemType Directory C:\Users\spike\win81pkg | Out-Null'
for f in ~/winboat-gpu-spike/win81/package/*; do SP_TIMEOUT=120 "$T/sp.sh" --put "$f" "C:/Users/spike/win81pkg/$(basename "$f")"; done
SP_TIMEOUT=60 "$T/sp.sh" --put "$T/w81swap.ps1" C:/Users/spike/w81swap.ps1
SP_TIMEOUT=60 "$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\spike\w81swap.ps1 | Out-Null; shutdown /r /t 2'
sleep 30
waitssh || { echo "no SSH after the restart" >&2; exit 1; }
sleep 30
DXTEST_PLAINCMD=1 SP_TIMEOUT=120 "$T/spike-dxtest.sh" "$@" >/dev/null 2>&1
cp "$T/results/dxtest-report.md" "$T/results/dxtest-report-win81-plain.md"
grep -m1 passed "$T/results/dxtest-report.md"
grep '❌' "$T/results/dxtest-report.md" | cut -c1-170
