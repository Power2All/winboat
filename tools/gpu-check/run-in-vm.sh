#!/usr/bin/env bash
# Runs winboat-gpucheck.exe inside the GPU spike VM and copies the report back.
#
#   run-in-vm.sh console   # in the logged-in desktop session (scheduled task with /IT)
#   run-in-vm.sh rdp       # as an RDP RemoteApp, like WinBoat launches apps (opens a FreeRDP window briefly);
#                          # signs out the console session first, so run console checks before rdp ones
#   run-in-vm.sh ssh       # in the SSH session (no desktop, on-screen tests are skipped)
#   run-in-vm.sh rdp32     # any mode with a 32 suffix runs the 32-bit build (older apps and most D3D9 games)
#
# Extra FreeRDP arguments for the rdp mode can be passed after the mode, e.g. `run-in-vm.sh rdp /gfx:AVC444`.
# GPUCHECK_ARGS adds arguments to the check itself, e.g. GPUCHECK_ARGS="--only d3d9+present-d3d9" ("+" separates groups: FreeRDP splits on commas).
set -euo pipefail

MODE="${1:-console}"
shift || true
SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# console32/rdp32/ssh32 run the 32-bit build, which loads the drivers' 32-bit (WOW64) parts
NAME=winboat-gpucheck
if [[ "$MODE" == *32 ]]; then
    NAME=winboat-gpucheck32
fi
EXE="$SCRIPT_DIR/dist/$NAME.exe"
# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"

SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$SPIKE_DIR/ssh/known_hosts"
    -o LogLevel=ERROR -i "$SPIKE_DIR/ssh/id_ed25519")
vm() { ssh "${SSH_OPTS[@]}" -p 12222 "$WIN_USER@127.0.0.1" "$@"; }
vm_copy() { scp "${SSH_OPTS[@]}" -P 12222 "$@" >/dev/null; }

[ -f "$EXE" ] || "$SCRIPT_DIR/build.sh"

INCOMPLETE=""
STAMP="$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="C:\\gpucheck"
OUT="$REMOTE_DIR\\$MODE-$STAMP"
vm "New-Item -ItemType Directory -Force -Path '$OUT' | Out-Null"
vm_copy "$EXE" "$WIN_USER@127.0.0.1:C:/gpucheck/$NAME.exe"

# Waits for the complete report (it is rewritten after every group). Gives up when the check process is gone, e.g.
# because it crashed or the RDP session ended, or when the optional process (FreeRDP) has exited.
wait_for_report() {
    local pid="${1:-}"
    sleep 15
    for _ in $(seq 1 120); do
        local state
        # (PowerShell's output over SSH ends in CRLF)
        state=$(vm "if (Select-String -Path '$OUT\\gpucheck-report.json' -Pattern '\"complete\": true' -Quiet -ErrorAction SilentlyContinue) { 'complete' } elseif (Get-Process $NAME -ErrorAction SilentlyContinue) { 'running' } else { 'gone' }" | tr -d '\r')
        case "$state" in
            complete) return 0 ;;
            gone)
                echo "The check process is gone before finishing (crashed, killed or its session ended)" >&2
                return 1
                ;;
        esac
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            echo "FreeRDP exited before the report was complete" >&2
            return 1
        fi
        sleep 5
    done
    echo "No complete report after 10 minutes" >&2
    return 1
}

case "${MODE%32}" in
    ssh)
        vm "& '$REMOTE_DIR\\$NAME.exe' --out '$OUT' ${GPUCHECK_ARGS:-}" >/dev/null || true
        ;;
    console)
        # (query user exits with 1 even when it lists sessions)
        if ! vm "query user; exit 0" | grep -q -i " console "; then
            echo "Nobody is logged in on the console (an rdp run signs it out); restart the VM to auto-login again" >&2
            exit 1
        fi
        vm "schtasks /Create /TN WinBoatGPUCheck /TR '\"$REMOTE_DIR\\$NAME.exe\" --out \"$OUT\" ${GPUCHECK_ARGS:-}' /SC ONCE /ST 23:59 /IT /RU $WIN_USER /F | Out-Null; schtasks /Run /TN WinBoatGPUCheck | Out-Null"
        wait_for_report || INCOMPLETE=1
        ;;
    rdp)
        FREERDP="$(command -v xfreerdp3 || command -v xfreerdp)"
        # Windows client editions allow one session per user, and a RemoteApp can't take over the auto-logged-in
        # console desktop (it hangs at the logon screen). Sign the console out first, like a WinBoat VM where
        # AutoAdminLogon is off.
        vm "\$id = (query user 2>\$null | Select-String ' console ' | ForEach-Object { (\$_ -split '\\s+')[3] }); if (\$id) { logoff \$id; Start-Sleep 5 }"
        # FreeRDP drops backslashes from the RemoteApp command line, so pass the output path with forward slashes
        "$FREERDP" /v:127.0.0.1 /port:13389 "/u:$WIN_USER" "/p:$WIN_PASSWORD" /cert:ignore /sec:tls -wallpaper /log-level:WARN \
            "/app:program:$REMOTE_DIR\\$NAME.exe,cmd:--out ${OUT//\\//} ${GPUCHECK_ARGS:-}" "$@" > "$SPIKE_DIR/freerdp.log" 2>&1 &
        RDP_PID=$!
        wait_for_report "$RDP_PID" || { INCOMPLETE=1; echo "FreeRDP log: $SPIKE_DIR/freerdp.log" >&2; }
        # FreeRDP catches SIGTERM but can stay connected to a RemoteApp session that has already ended
        kill "$RDP_PID" 2>/dev/null || true
        sleep 2
        kill -9 "$RDP_PID" 2>/dev/null || true
        ;;
    *)
        echo "Unknown mode: $MODE (console, rdp or ssh, with a 32 suffix for the 32-bit build)" >&2
        exit 2
        ;;
esac

LOCAL_DIR="$SPIKE_DIR/reports/$MODE-$STAMP"
mkdir -p "$LOCAL_DIR"
if ! vm_copy "$WIN_USER@127.0.0.1:C:/gpucheck/$MODE-$STAMP/*" "$LOCAL_DIR/"; then
    echo "No report was written" >&2
    exit 1
fi
cat "$LOCAL_DIR/gpucheck-report.md"
echo
echo "Saved to $LOCAL_DIR"
[ -z "${INCOMPLETE:-}" ]
