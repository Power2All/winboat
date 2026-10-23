#!/bin/bash
# full-build.sh: build-driver.ps1 in the spike (scheduled task WinBoatBuildDriver: kernel and user-mode drivers, package)
T=$(dirname "$0")
# the AMF runtime (built here with MinGW) where build-driver.ps1 takes it from
~/Coding/winboat/tools/amf-runtime/build.sh >/dev/null
"$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\tools\amf-runtime | Out-Null' >/dev/null
for f in amfrt64.dll amfrt64-license.txt; do "$T/sp.sh" --put ~/Coding/winboat/tools/amf-runtime/dist/$f C:/build/tools/amf-runtime/$f; done
# the Media Foundation encoders, the same way
~/Coding/winboat/tools/mf-encoder/build.sh >/dev/null
"$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\tools\mf-encoder | Out-Null' >/dev/null
"$T/sp.sh" --put ~/Coding/winboat/tools/mf-encoder/dist/winboat_mft64.dll C:/build/tools/mf-encoder/winboat_mft64.dll
"$T/sp.sh" 'Remove-Item C:\build\build-driver.exit -ErrorAction SilentlyContinue; schtasks /Run /TN WinBoatBuildDriver' >/dev/null
for i in $(seq 1 240); do sleep 15; "$T/sp.sh" 'Test-Path C:\build\build-driver.exit' 2>/dev/null | grep -q True && break; echo "building ($((i * 15)) s)"; done
echo "build exit: $("$T/sp.sh" 'Get-Content C:\build\build-driver.exit' | tr -d '\r ')"
"$T/sp.sh" 'Select-String -Path C:\build\build-driver.log -Pattern ": error|: fatal|error LNK|Command failed|Built:" | Select-Object -First 20 | ForEach-Object { $_.Line }' | tr -d '\r'
