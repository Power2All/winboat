#!/bin/bash
# spike-run.sh '<cmd.exe command line>' [seconds]: runs it in the spike's desktop session (C:\build\run as the
# working directory), waits (default 120 s) and prints its output (stdout and stderr)
T=$(dirname "$0"); . ~/winboat-gpu-spike/credentials
printf '@echo off\r\ncd /d C:\\build\\run\r\n%s > C:\\build\\run\\out.txt 2>&1\r\necho exit %%ERRORLEVEL%% > C:\\build\\run\\done\r\n' "$1" > "$T/run.cmd"
"$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\run | Out-Null; Remove-Item C:\build\run\done,C:\build\run\out.txt -ErrorAction SilentlyContinue'
"$T/sp.sh" --put "$T/run.cmd" C:/build/run/run.cmd
"$T/sp.sh" "schtasks /Create /F /TN WinBoatRun /TR 'conhost.exe --headless C:\\build\\run\\run.cmd' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoatRun | Out-Null"
for _ in $(seq 1 $(( ${2:-120} / 2 ))); do "$T/sp.sh" 'Test-Path C:\build\run\done' 2>/dev/null | grep -q True && break; sleep 2; done
"$T/sp.sh" 'Get-Content C:\build\run\out.txt -ErrorAction SilentlyContinue; Get-Content C:\build\run\done -ErrorAction SilentlyContinue' | tr -d '\r'
