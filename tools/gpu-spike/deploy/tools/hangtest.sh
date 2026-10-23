#!/bin/bash
# hangtest.sh [freeze seconds]: spin.exe renders in the spike while its GPU process is frozen (SIGSTOP), then resumed
T=$(dirname "$0")
F=${1:-25}
$T/sp.sh "Remove-Item C:\build\spin.txt -EA SilentlyContinue; schtasks /Create /F /TN WinBoatSpin /TR 'C:\build\spin.cmd' /SC ONCE /ST 23:59 /RU spike /IT | Out-Null; schtasks /Run /TN WinBoatSpin | Out-Null"
sleep 8
pid=$(docker exec WinBoatGPUSpike pgrep -f "^/usr/libexec/vhost-user-gpu" | head -1)
docker exec WinBoatGPUSpike kill -STOP $pid; echo "$(date +%T) GPU process $pid frozen for $F s"
for i in $(seq 1 $((F / 5))); do sleep 5; echo "$(date +%T) frozen; spin: $($T/sp.sh 'Get-Content C:\build\spin.txt -Tail 1' 2>&1 | tr -d '\r')"; done
docker exec WinBoatGPUSpike kill -CONT $pid; echo "$(date +%T) resumed"
for i in $(seq 1 12); do sleep 5; l=$($T/sp.sh "Get-Content C:\build\spin.txt -Tail 1" 2>/dev/null | tr -d "\r\000"); echo "$(date +%T) spin: $l"; [ "$l" = exit ] && break; done
$T/sp.sh 'Get-Content C:\build\spin.txt' | tr -d '\r' | grep -v "frames, device" | head -12
$T/sp.sh 'Get-WinEvent -FilterHashtable @{LogName="System"; StartTime=(Get-Date).AddMinutes(-5)} -EA SilentlyContinue | ? { $_.Id -eq 4101 -or $_.ProviderName -match "Display|dxgkrnl" } | Select -First 5 | % { "{0} {1} {2}: {3}" -f $_.TimeCreated.ToString("HH:mm:ss"), $_.Id, $_.ProviderName, ($_.Message -replace "\s+"," ").Substring(0,[Math]::Min(150,$_.Message.Length)) }; C:\build\kmdlog.exe' 2>&1 | tr -d '\r' | grep -v "^\s*$" | tail -6
