#!/bin/bash
# install-restart.sh [tag]: installs the spike's last built package, restarts the spike (progress lines for guard.sh),
# prints the adapter
T=$(dirname "$0")
SP_TIMEOUT=600 "$T/sp.sh" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\OEM\install-driver.ps1' 2>&1 | tr -d '\r' | tail -1
"$T/spike-up.sh" "$@"
SP_TIMEOUT=60 "$T/sp.sh" '(Get-CimInstance Win32_VideoController | Select-Object -First 1 Name,DriverVersion,Status | Format-Table -HideTableHeaders | Out-String).Trim()' | tr -d '\r'
