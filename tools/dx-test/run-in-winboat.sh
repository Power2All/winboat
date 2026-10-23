#!/usr/bin/env bash
# Runs the DirectX test in the WinBoat VM, on Windows' desktop (through the guest server), and prints the report.
#
#   tools/dx-test/run-in-winboat.sh [test arguments, e.g. --only d3d9,d3d11-render]
#
# Windows has to be running. The programs reach Windows through the home folder WinBoat shares (\\host.lan\Data),
# run from Windows' temporary folder, and write their report next to this script: dist/results-<time>/
# (dxtest-report.html to open in a browser, .md, .json, and console.txt).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER="${WINBOAT_CONTAINER:-WinBoat}"
COMPOSE="${WINBOAT_COMPOSE:-$HOME/.winboat/docker-compose.yml}"
[ -f "$DIR/dist/winboat-dxtest.exe" ] || "$DIR/build.sh"

case "$DIR" in
    "$HOME"/*) ;;
    *) echo "The test has to be under the home folder (WinBoat shares it with Windows): $DIR" >&2; exit 1 ;;
esac
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$DIR/dist/results-$STAMP"
mkdir -p "$OUT"
SHARE='\\host.lan\Data\'"$(printf '%s' "${DIR#"$HOME"/}/dist" | tr / '\\')"
WOUT="$SHARE\\results-$STAMP"

RUNTIME=docker
command -v docker >/dev/null || RUNTIME=podman
PORT="$($RUNTIME port "$CONTAINER" 7148/tcp | head -1 | sed 's/.*://')"
if [ -z "$PORT" ] || ! curl -sf -m 5 "http://127.0.0.1:$PORT/health" >/dev/null; then
    echo "WinBoat's Windows isn't running (no guest server on port ${PORT:-?})" >&2
    exit 1
fi

# A batch file in the results folder does the work (one long cmd /c line trips over cmd's quote rules)
printf '@echo off\r\nmkdir "%%TEMP%%\\winboat-dxtest" 2>nul\r\ncopy /y "%s\\winboat-dxtest*.exe" "%%TEMP%%\\winboat-dxtest\\" >nul\r\n"%%TEMP%%\\winboat-dxtest\\winboat-dxtest.exe" --out "%s" %s > "%s\\console.txt" 2>&1\r\necho done> "%s\\done"\r\n' \
    "$SHARE" "$WOUT" "$*" "$WOUT" "$WOUT" > "$OUT/run.cmd"
# Windows' SMB client caches "not found" for a few seconds: wait until the new folder's batch file shows
CMDLINE="--headless powershell.exe -NoProfile -Command \"for (\$i = 0; \$i -lt 60 -and -not (Test-Path '$WOUT\\run.cmd'); \$i++) { Start-Sleep 1 }; cmd.exe /c '$WOUT\\run.cmd'\""

python3 -I - "$PORT" "$COMPOSE" "$CMDLINE" <<'EOF'
import json, re, sys, urllib.request
port, compose, args = sys.argv[1:4]
m = re.search(r'^\s*PASSWORD:\s*"?([^"\n]*)"?\s*$', open(compose).read(), re.M)
if not m:
    sys.exit('No PASSWORD in ' + compose)
body = {'path': r'C:\Windows\System32\conhost.exe', 'args': args, 'seamless': False, 'password': m.group(1)}
req = urllib.request.Request(f'http://127.0.0.1:{port}/desktop/launch', data=json.dumps(body).encode(),
                             headers={'Content-Type': 'application/json'}, method='POST')
with urllib.request.urlopen(req, timeout=60) as r:
    print('Started in Windows (process', json.loads(r.read())['pid'], ')')
EOF

echo "Running; the report goes to $OUT"
for _ in $(seq 1 600); do
    [ -f "$OUT/done" ] && break
    sleep 3
done
if [ ! -f "$OUT/done" ]; then
    echo "No result after 30 minutes; whatever was written so far is in $OUT" >&2
    exit 1
fi
# (--check names its report gpu-check)
REPORT="$OUT/dxtest-report"
[ -f "$REPORT.md" ] || REPORT="$OUT/gpu-check"
cat "$REPORT.md"
echo
echo "Report: $REPORT.html"
