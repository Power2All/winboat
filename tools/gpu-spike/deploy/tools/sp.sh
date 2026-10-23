#!/bin/bash
# sp.sh '<PowerShell command>' | --put <local> <remote> | --get <remote> <local>: the GPU spike VM over SSH (port 12222).
# Every call is bounded: SP_TIMEOUT seconds in all (default 900, exit 124 when hit), and a VM that stops answering
# drops the connection after a minute (keepalives)
. ~/winboat-gpu-spike/credentials
O=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o StrictHostKeyChecking=no -o UserKnownHostsFile=$HOME/winboat-gpu-spike/ssh/known_hosts -o LogLevel=ERROR -i $HOME/winboat-gpu-spike/ssh/id_ed25519)
# a failed copy says so (scp -q is silent, and a timeout looked like success to the callers that don't check)
copy() { timeout -k 10 "${SP_TIMEOUT:-900}" scp -q "${O[@]}" -P ${SP_PORT:-12222} "$1" "$2" || { r=$?; echo "sp.sh: copying $1 to $2 failed ($r)" >&2; exit $r; }; }
# SP_HTTP=1 (a plain-QEMU VM; Windows 7, whose OpenSSH hangs in SFTP and whose PowerShell 2.0 shell breaks scp's old
# protocol): the guest fetches a file from a short-lived HTTP server on the host's loopback (10.0.2.2 from QEMU's user
# network), and sends one back with an HTTP PUT (httpput.py)
if [ "${SP_HTTP:-}" = 1 ] && [ "$1" = --put ]; then
    port=$((18000 + RANDOM % 1000))
    timeout "${SP_TIMEOUT:-900}" python3 -m http.server --bind 127.0.0.1 --directory "$(dirname "$2")" $port >/dev/null 2>&1 &
    srv=$!; sleep 1
    timeout -k 10 "${SP_TIMEOUT:-900}" ssh "${O[@]}" -p ${SP_PORT:-12222} "$WIN_USER@127.0.0.1" \
        "(New-Object Net.WebClient).DownloadFile('http://10.0.2.2:$port/$(basename "$2")', '${3//\//\\}')"
    r=$?; kill $srv 2>/dev/null
    [ $r = 0 ] || echo "sp.sh: copying $2 to $3 failed ($r)" >&2
    exit $r
fi
if [ "${SP_HTTP:-}" = 1 ] && [ "$1" = --get ]; then
    port=$((19000 + RANDOM % 1000))
    timeout "${SP_TIMEOUT:-900}" python3 -I "$(dirname "$0")/httpput.py" $port "$3" &
    srv=$!; sleep 1
    timeout -k 10 "${SP_TIMEOUT:-900}" ssh "${O[@]}" -p ${SP_PORT:-12222} "$WIN_USER@127.0.0.1" \
        "(New-Object Net.WebClient).UploadData('http://10.0.2.2:$port/', 'PUT', [IO.File]::ReadAllBytes('${2//\//\\}')) | Out-Null"
    r=$?; wait $srv 2>/dev/null; kill $srv 2>/dev/null
    [ $r = 0 ] || echo "sp.sh: copying $2 to $3 failed ($r)" >&2
    exit $r
fi
case "$1" in
    --put) copy "$2" "$WIN_USER@127.0.0.1:$3" ;;
    --get) copy "$WIN_USER@127.0.0.1:$2" "$3" ;;
    *) timeout -k 10 "${SP_TIMEOUT:-900}" ssh "${O[@]}" -p ${SP_PORT:-12222} "$WIN_USER@127.0.0.1" "$@" ;;
esac
