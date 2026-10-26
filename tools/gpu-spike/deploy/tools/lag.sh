#!/bin/bash
# lag.sh boot <spice-gl|gtk|gtk-wl|sdl|egl-spice> | client <spice-gl|spice-gl-042|egl-spice> | test <seconds> | log | down
# The Windows 10 test VM on the host's stock QEMU (plain-qemu.sh) with one display option, for measuring it with
# lagmeter (docs/standalone-install.md, "Which display: measured"); every step bounded. SP_PORT=12222 runs "test" and
# "log" on the spike instead. lagtest.exe: tools/gpu-bench/build.sh; lagmeter: built here when missing.
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
L=$T/results/lag
mkdir -p "$L"
[ -x "$L/lagmeter" ] || gcc -O2 -Wall "$T/lagmeter.c" -o "$L/lagmeter" -lX11 -lXtst
G=~/winboat-gpu-spike
SOCK=${XDG_RUNTIME_DIR:-/run/user/1000}/lag-spice.sock
export SP_PORT=${SP_PORT:-12322}
. $G/credentials
sp() { SP_TIMEOUT=${TO:-60} "$T/sp.sh" "$@"; }
case $1 in
boot)
    pgrep -x qemu-system-x86 -a | grep -q win10/storage && { echo "already running"; exit 1; }
    rm -f "$SOCK"
    case $2 in
    spice-gl) D="-display none -spice unix=on,addr=$SOCK,disable-ticketing=on,gl=on,rendernode=/dev/dri/renderD128" ;;
    gtk|gtk-wl) D="-display gtk,gl=on,show-menubar=off" ;;
    sdl) D="-display sdl,gl=on" ;;
    egl-spice) D="-display egl-headless,rendernode=/dev/dri/renderD128 -spice unix=on,addr=$SOCK,disable-ticketing=on" ;;
    esac
    ssh-keygen -q -R "[127.0.0.1]:$SP_PORT" -f $G/ssh/known_hosts >/dev/null 2>&1
    BK=x11; [ "$2" = gtk-wl ] && BK=wayland
    (GDK_BACKEND=$BK EGL_PLATFORM=$BK SDL_VIDEODRIVER=x11 PLAIN_DIR=$G/win10/storage PLAIN_DISPLAY="$D" setsid nohup "$T/plain-qemu.sh" \
        > "$L/qemu-$2.log" 2>&1 &)
    for i in $(seq 1 60); do SP_TIMEOUT=10 "$T/sp.sh" "echo up" 2>/dev/null | grep -q up && { echo "SSH up"; exit 0; }; echo "waiting for SSH ($i)"; sleep 8; done
    echo "no SSH"; exit 1 ;;
client)
    case $2 in
    spice-gl) (GDK_BACKEND=x11 EGL_PLATFORM=x11 LD_LIBRARY_PATH=~/Coding/winboat/spice/viewer/lib setsid nohup \
        ~/Coding/winboat/spice/viewer/bin/winboat-spice-viewer --title lagclient "spice+unix://$SOCK" > "$L/client.log" 2>&1 &) ;;
    spice-gl-042) (GDK_BACKEND=x11 EGL_PLATFORM=x11 setsid nohup spicy --uri="spice+unix://$SOCK" > "$L/client.log" 2>&1 &) ;;
    egl-spice) (GDK_BACKEND=x11 EGL_PLATFORM=x11 setsid nohup spicy --uri="spice+unix://$SOCK" > "$L/client.log" 2>&1 &) ;;
    esac ;;
test)
    # lagtest in the signed-in desktop session (a scheduled task), 1920x1080
    TO=60 sp --put "$T/../../../gpu-bench/dist/lagtest.exe" C:/build/lagtest.exe >/dev/null || exit 1
    sp "Remove-Item C:\\build\\lagtest.log -ErrorAction SilentlyContinue
schtasks /Create /F /TN LagTest /TR 'C:\\build\\lagtest.exe $2 C:\\build\\lagtest.log 1920 1080' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN LagTest" ;;
log) sp "Get-Content C:\\build\\lagtest.log" ;;
down)
    TO=20 sp 'shutdown /s /t 1' >/dev/null 2>&1
    for i in $(seq 1 30); do pgrep -x qemu-system-x86 -a | grep -q win10/storage || { echo "VM off"; exit 0; }; sleep 3; done
    for p in $(pgrep -x qemu-system-x86); do grep -qa win10/storage /proc/$p/cmdline && kill $p; done
    echo "VM killed" ;;
esac
