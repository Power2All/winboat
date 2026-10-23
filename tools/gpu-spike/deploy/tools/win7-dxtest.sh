#!/bin/bash
# win7-dxtest.sh [dx-test args] (ENVSET="A=1 B=2" adds environment): dx-test in the Windows 7 VM's desktop session (running: win7-try.sh), with the
# EWDK's app-local Universal CRT (Windows 7 has none; tools/standalone-installer/ucrt): the 64-bit one next to
# winboat-dxtest.exe, the 32-bit one with winboat-dxtest32.exe in 32\ (no HLSL compiler without KB4019990: the SDK's is Windows 8's). The report in results/dxtest-report-win7.md
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd); . ~/winboat-gpu-spike/credentials
export SP_PORT=12322 SP_HTTP=1
D=~/Coding/winboat/tools/dx-test/dist U=~/Coding/winboat/tools/standalone-installer/ucrt
{ printf '@echo off\r\ncd /d C:\\build\\dxtest\r\n'; for e in ${ENVSET:-}; do printf 'set %s\r\n' "$e"; done
  printf 'winboat-dxtest.exe --out C:\\build\\dxtest %s > C:\\build\\dxtest\\console.txt 2>&1\r\necho done > C:\\build\\dxtest\\done\r\n' "$*"; } > "$T/dxtest7.cmd"
SP_TIMEOUT=30 "$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\dxtest\32 | Out-Null; Remove-Item C:\build\dxtest\done,C:\build\dxtest\console.txt -ErrorAction SilentlyContinue' >/dev/null
put() { echo "put $2"; SP_TIMEOUT=120 "$T/sp.sh" --put "$1" "$2" || exit 1; }
put $D/winboat-dxtest.exe C:/build/dxtest/winboat-dxtest.exe
put $D/winboat-dxtest32.exe C:/build/dxtest/32/winboat-dxtest32.exe
put "$T/dxtest7.cmd" C:/build/dxtest/dxtest.cmd
# the CRT's DLLs once (a transfer each)
have=$(SP_TIMEOUT=30 "$T/sp.sh" 'Get-ChildItem C:\build\dxtest -Recurse -Filter *.dll | ForEach-Object { $_.Directory.Name + "/" + $_.Name }' | tr -d '\r')
for f in $U/x64/*.dll; do grep -qx "dxtest/$(basename $f)" <<< "$have" || put $f C:/build/dxtest/$(basename $f); done
for f in $U/x86/*.dll; do grep -qx "32/$(basename $f)" <<< "$have" || put $f C:/build/dxtest/32/$(basename $f); done
SP_TIMEOUT=30 "$T/sp.sh" "schtasks /Create /F /TN WinBoatDxTest /TR 'C:\\build\\dxtest\\dxtest.cmd' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoatDxTest | Out-Null"
# (Windows not answering 5 times in a row: hung or bugchecked, given up)
lost=0
for i in $(seq 1 300); do
   r=$(SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\build\dxtest\done' 2>/dev/null | tr -d '\r')
   [ "$r" = True ] && break
   if [ -z "$r" ]; then lost=$((lost + 1)); [ $lost -ge 5 ] && { echo "Windows doesn't answer: hung or bugchecked" >&2; exit 2; }; else lost=0; fi
   echo "running ($i)"; sleep 4
done
SP_TIMEOUT=30 "$T/sp.sh" 'Get-Content C:\build\dxtest\console.txt' | tr -d '\r' | tail -${DXTEST_LINES:-8}
SP_TIMEOUT=60 "$T/sp.sh" --get C:/build/dxtest/dxtest-report.md "$T/results/dxtest-report-win7.md"
