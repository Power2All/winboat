#!/bin/bash
# qmon.sh <dir> '<monitor command>'...: commands to plain-qemu.sh's QEMU monitor (<dir>/monitor.sock), output dropped;
# "shot <file.png>" saves a screenshot
D=$1; shift
for c in "$@"; do
   case "$c" in
      shot\ *) f=${c#shot }; echo "screendump ${f%.png}.ppm" | timeout 10 socat - UNIX-CONNECT:$D/monitor.sock >/dev/null; sleep 1
               magick "${f%.png}.ppm" "$f" && rm -f "${f%.png}.ppm" ;;
      *) echo "$c" | timeout 10 socat - UNIX-CONNECT:$D/monitor.sock >/dev/null ;;
   esac
   sleep 0.5
done
