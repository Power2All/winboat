#!/bin/bash
# spike-run.sh '<cmd.exe command line>' [seconds]: runs it in the spike's desktop session (C:\build\run as the
# working directory), waits (default 120 s) and prints its output (stdout and stderr). SPIKE_RUN_ID=<id> runs it
# beside another one still running (its own task and output files in C:\build\<id>)
T=$(dirname "$0"); . ~/winboat-gpu-spike/credentials
ID=${SPIKE_RUN_ID:-run}
printf '@echo off\r\ncd /d C:\\build\\run\r\n%s > C:\\build\\%s\\out.txt 2>&1\r\necho exit %%ERRORLEVEL%% > C:\\build\\%s\\done\r\n' "$1" "$ID" "$ID" > "$T/$ID.cmd"
"$T/sp.sh" "New-Item -ItemType Directory -Force C:\\build\\run, C:\\build\\$ID | Out-Null; Remove-Item C:\\build\\$ID\\done,C:\\build\\$ID\\out.txt -ErrorAction SilentlyContinue"
"$T/sp.sh" --put "$T/$ID.cmd" "C:/build/$ID/$ID.cmd"
"$T/sp.sh" "schtasks /Create /F /TN WinBoat$ID /TR 'conhost.exe --headless C:\\build\\$ID\\$ID.cmd' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoat$ID | Out-Null"
for _ in $(seq 1 $(( ${2:-120} / 2 ))); do "$T/sp.sh" "Test-Path C:\\build\\$ID\\done" 2>/dev/null | grep -q True && break; sleep 2; done
"$T/sp.sh" "Get-Content C:\\build\\$ID\\out.txt -ErrorAction SilentlyContinue; Get-Content C:\\build\\$ID\\done -ErrorAction SilentlyContinue" | tr -d '\r'
