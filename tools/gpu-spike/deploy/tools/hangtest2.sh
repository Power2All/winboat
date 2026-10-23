#!/bin/bash
T=$(dirname "$0"); . ~/winboat-gpu-spike/credentials
$T/sp.sh "Remove-Item C:\build\spin.txt -EA SilentlyContinue; schtasks /Run /TN WinBoatSpin | Out-Null"
sleep 8
pid=$(docker exec WinBoatGPUSpike pgrep -f "^/usr/libexec/vhost-user-gpu" | head -1)
docker exec WinBoatGPUSpike kill -STOP $pid; echo "$(date +%T) frozen"
sleep 15
docker exec WinBoatGPUSpike kill -CONT $pid; echo "$(date +%T) resumed"
sleep 4
$T/sp.sh 'C:\build\kmdlog.exe' 2>/dev/null | tr -d '\r\000' | grep -v "^\s*$" | tail -15
$T/sp.sh 'Get-Content C:\build\spin.txt' 2>/dev/null | tr -d '\r\000' | grep -v "frames, device 1" | head -8
