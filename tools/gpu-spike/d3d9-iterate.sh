#!/usr/bin/env bash
# One development round for the D3D9 driver (d3d9umd) in the GPU spike VM:
# push the changed Mesa sources, build viogpu_d3d9.dll, deploy it next to the installed driver (with call tracing),
# run the Direct3D 9 checks in an RDP RemoteApp session, and show the results, driver messages and trace.
#
#   [TRACE=0] tools/gpu-spike/d3d9-iterate.sh [rdp|rdp32|console|console32]
#
# TRACE=0 deploys without call tracing (for performance numbers); the default traces every DDI call.
#
# Needs dev-build.ps1 and dev-deploy-d3d9.ps1 in C:\OEM with their scheduled tasks (WinBoatDevBuild,
# WinBoatDeployD3D9) and UserModeDriverName[0] already pointing at viogpu_d3d9.dll (dev-deploy-d3d9.ps1, then a
# device restart or reboot).
set -euo pipefail

MODE="${1:-rdp}"
SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no
    -o "UserKnownHostsFile=$SPIKE_DIR/ssh/known_hosts" -o LogLevel=ERROR -i "$SPIKE_DIR/ssh/id_ed25519")
vm() { ssh "${SSH_OPTS[@]}" -p 12222 "$WIN_USER@127.0.0.1" "$@"; }

# Runs a scheduled task and waits for it to finish. The C:\OEM\run-*.cmd wrappers write the exit code to <name>.exit
# (not to the log: appending can fail while something reads it).
run_task() {
    local task="$1" name="$2"
    vm "Remove-Item C:\\build\\$name.exit -ErrorAction SilentlyContinue; schtasks /Run /TN $task | Out-Null"
    until vm "if (Test-Path C:\\build\\$name.exit) { exit 0 } else { exit 1 }" 2>/dev/null; do
        sleep 5
    done
}

"$SCRIPT_DIR/push-dev.sh" mesa
arch=amd64 suffix=""
if [[ "$MODE" == *32 ]]; then
    arch=x86 suffix=_x86
fi
target="src/gallium/targets/d3d9umd/viogpu_d3d9$suffix.dll"
vm "schtasks /Create /F /TN WinBoatDevBuild /TR \"C:\\OEM\\run-dev-build.cmd -Arch $arch -Targets $target\" /RU SYSTEM /RL HIGHEST /SC ONCE /ST 23:59 | Out-Null"
run_task WinBoatDevBuild dev-build
errors=$(vm '$e = Get-Content C:\build\dev-build.log | Select-String "error C\d+|error LNK\d+|fatal error|ERROR:"; $e | ForEach-Object { $_.Line -replace "^.*?src\\gallium\\", "" } | Select-Object -First 30')
if [ -n "$errors" ]; then
    echo "$errors"
    exit 1
fi
echo "Built $target"

trace_arg=""
if [ "${TRACE:-1}" != 0 ]; then
    trace_arg=" -Trace"
fi
vm "schtasks /Create /F /TN WinBoatDeployD3D9 /TR \"C:\\OEM\\run-deploy-d3d9.cmd$trace_arg\" /RU SYSTEM /RL HIGHEST /SC ONCE /ST 23:59 | Out-Null"
run_task WinBoatDeployD3D9 deploy-d3d9

GPUCHECK_ARGS="--only d3d9+present-d3d9" "$SCRIPT_DIR/../gpu-check/run-in-vm.sh" "$MODE" > "$SPIKE_DIR/d3d9-iterate.log" 2>&1 || true
grep -vE "Driver message" "$SPIKE_DIR/d3d9-iterate.log" | sed -n '/## Direct3D 9$/,/^## Direct3D 12/p;/## On-screen presentation: Direct3D 9/,$p' | grep -E "^- " || true
if [ "${TRACE:-1}" = 0 ]; then
    exit 0
fi
echo "--- trace (last 60 lines)"
vm 'Get-Content C:\viogpu_d3d9_trace.log -ErrorAction SilentlyContinue | Select-String -NotMatch "GetCaps" | Select-Object -Last 60 | ForEach-Object { $_.Line }'
