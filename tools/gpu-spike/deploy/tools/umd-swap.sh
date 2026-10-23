#!/bin/bash
# umd-swap.sh [repos...]: pushes source changes (default: mesa mesa-venus vkd3d-proton) to the spike and runs
# build-driver.ps1 -UserModeOnly (scheduled task WinBoatUmdOnly: rebuilds the user-mode drivers and swaps them into
# the installed package), then prints the errors
T=$(dirname "$0")
~/Coding/winboat/tools/gpu-spike/push-dev.sh ${@:-mesa mesa-venus vkd3d-proton} | tail -3
"$T/sp.sh" 'Remove-Item C:\build\build-driver.exit -ErrorAction SilentlyContinue; schtasks /Run /TN WinBoatUmdOnly | Out-Null'
for _ in $(seq 1 240); do sleep 15; "$T/sp.sh" 'Test-Path C:\build\build-driver.exit' 2>/dev/null | grep -q True && break; done
echo "exit $("$T/sp.sh" 'Get-Content C:\build\build-driver.exit' | tr -d '\r ')"
"$T/sp.sh" 'Select-String -Path C:\build\build-driver.log -Pattern ": error|: fatal|error C[0-9]|Could not|Command failed|FAILED" | Select-Object -First 15 | ForEach-Object { $_.Line }' | tr -d '\r' | cut -c1-260
