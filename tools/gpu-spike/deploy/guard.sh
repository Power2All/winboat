#!/bin/bash
# guard.sh <max seconds> <stall seconds> <command...>: runs a command with a hard time limit and a heartbeat every
# 15 s (elapsed, output size, last line); kills it when its output hasn't changed for <stall seconds>. Prints the
# command's output at the end, and why it stopped.
MAX="$1"; STALL="$2"; shift 2
LOG=$(mktemp /tmp/claude-1000/guard.XXXXXX)
setsid bash -c "$*" > "$LOG" 2>&1 &
PID=$!
start=$(date +%s); last=-1; changed=$start; why="finished"
while kill -0 "$PID" 2>/dev/null; do
    sleep 5
    now=$(date +%s); size=$(stat -c %s "$LOG")
    [ "$size" != "$last" ] && { last=$size; changed=$now; }
    if [ $(((now - start) % 15)) -lt 5 ]; then
        echo "[guard] $((now - start))s, output ${size}B: $(tail -1 "$LOG" | tr -d '\r' | cut -c1-100)" >&2
    fi
    if [ $((now - changed)) -ge "$STALL" ]; then why="STALLED (no output for ${STALL}s): killed"; break; fi
    if [ $((now - start)) -ge "$MAX" ]; then why="TIME LIMIT (${MAX}s): killed"; break; fi
done
[ "$why" != "finished" ] && kill -- -"$PID" 2>/dev/null
wait "$PID" 2>/dev/null; rc=$?
cat "$LOG"; rm -f "$LOG"
echo "[guard] $why after $(($(date +%s) - start))s (exit $rc)" >&2
