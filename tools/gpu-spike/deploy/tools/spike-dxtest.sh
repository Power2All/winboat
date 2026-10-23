#!/bin/bash
# spike-dxtest.sh [args]: runs winboat-dxtest.exe in the spike's desktop session (ENVSET="A=1 B=2" adds environment),
# prints its console output and the report's tally; the report lands in ./results/dxtest-report.md. DXTEST_PLAINCMD=1
# starts it with cmd.exe instead of a headless console (conhost --headless is Windows 10 1809's: for Windows 8.1)
T=$(dirname "$0"); . ~/winboat-gpu-spike/credentials
D=~/Coding/winboat/tools/dx-test/dist
mkdir -p "$T/results"
{ printf '@echo off\r\ncd /d C:\\build\\dxtest\r\n'; for e in $ENVSET; do printf 'set %s\r\n' "$e"; done
  printf 'winboat-dxtest.exe --out C:\\build\\dxtest %s > C:\\build\\dxtest\\console.txt 2>&1\r\necho done > C:\\build\\dxtest\\done\r\n' "$*"; } > "$T/dxtest.cmd"
"$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\dxtest | Out-Null; Remove-Item C:\build\dxtest\done,C:\build\dxtest\console.txt -ErrorAction SilentlyContinue'
for f in $D/winboat-dxtest.exe $D/winboat-dxtest32.exe "$T/dxtest.cmd"; do "$T/sp.sh" --put "$f" "C:/build/dxtest/$(basename $f)"; done
RUNNER='conhost.exe --headless'; [ "${DXTEST_PLAINCMD:-}" = 1 ] && RUNNER='cmd.exe /c'
"$T/sp.sh" "schtasks /Create /F /TN WinBoatDxTest /TR '$RUNNER C:\\build\\dxtest\\dxtest.cmd' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoatDxTest | Out-Null"
for _ in $(seq 1 900); do "$T/sp.sh" 'Test-Path C:\build\dxtest\done' | grep -q True && break; sleep 4; done
"$T/sp.sh" 'Get-Content C:\build\dxtest\console.txt' | tr -d '\r'
R=dxtest-report; echo "$*" | grep -q -- --check && R=gpu-check
"$T/sp.sh" --get "C:/build/dxtest/$R.md" "$T/results/dxtest-report.md"
