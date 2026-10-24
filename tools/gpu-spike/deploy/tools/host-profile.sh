#!/bin/bash
# host-profile.sh [seconds]: samples the spike's host GPU processes (vhost-user-gpu: virglrenderer's OpenGL path;
# virgl_render_server: Venus) with perf for a while (default 10 s) and writes the report for reading as the user:
# /tmp/claude-1000/host-profile.txt. Needs root (the processes are root's, in the spike's container): sudo
S=${1:-10}
OUT=/tmp/claude-1000
mkdir -p $OUT
command -v perf >/dev/null || { echo "perf isn't installed: sudo pacman -S --needed perf" >&2; exit 1; }
# The spike container's GPU processes, by their host PIDs (WinBoat's own VM has the same ones, in another container)
PIDS=$(docker top WinBoatGPUSpike -eo pid,comm | awk '$2 ~ /vhost-user-gpu|virgl_render/ {print $1}' | paste -sd,)
if [ -z "$PIDS" ]; then echo "no GPU processes in the spike's container (is it running?)" >&2; exit 1; fi
echo "profiling $PIDS for $S s"
perf record -F 2999 -g --call-graph dwarf,16384 -p "$PIDS" -o $OUT/host-perf.data -- sleep "$S"
echo "writing the report"
perf report -i $OUT/host-perf.data --stdio --no-children --percent-limit 0.5 --sort comm,dso,symbol > $OUT/host-profile.txt 2>/dev/null
perf report -i $OUT/host-perf.data --stdio --children --percent-limit 2 --sort symbol -g none > $OUT/host-profile-children.txt 2>/dev/null
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" $OUT/host-perf.data $OUT/host-profile.txt $OUT/host-profile-children.txt
echo "report: $OUT/host-profile.txt ($(wc -l < $OUT/host-profile.txt) lines)"
