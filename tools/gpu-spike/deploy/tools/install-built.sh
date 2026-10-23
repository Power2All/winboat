#!/bin/bash
# install-built.sh: installs the package the spike last built (C:\OEM\install-driver.ps1), then restarts the spike
T=$(dirname "$0")
"$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\OEM\install-driver.ps1' 2>&1 | tr -d '\r' | tail -1
"$T/spike-up.sh" | tail -1
"$T/sp.sh" '(Get-CimInstance Win32_VideoController | Select-Object -First 1 Name,DriverVersion | Format-Table -HideTableHeaders | Out-String).Trim()' | tr -d '\r'
