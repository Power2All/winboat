#!/bin/bash
# uservm-wait.sh: waits for the WinBoat VM's guest server to go down (a restart) and come back, then prints GPU status
p=$(docker port WinBoat 7148/tcp | head -1 | sed 's/.*://')
for _ in $(seq 1 40); do curl -s -m 3 -o /dev/null "http://127.0.0.1:$p/health" || break; sleep 3; done
echo "down"
for _ in $(seq 1 120); do curl -s -m 3 -o /dev/null "http://127.0.0.1:$p/health" && break; sleep 5; done
echo "up"
curl -s -m 60 "http://127.0.0.1:$p/gpu" | grep -o '"installed":{[^}]*}\|"device":{[^}]*}\|"rebootRequired":[a-z]*'
