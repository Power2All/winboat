#!/bin/bash
# spike-restart.sh [tag]: clean shutdown, recreate (optionally on winboat-windows-spice:<tag>), wait for explorer
cd ~/winboat-gpu-spike; SP=deploy/tools/sp.sh
[ -n "$1" ] && sed -i "s#image: winboat-windows-spice:[0-9a-z.-]*#image: winboat-windows-spice:$1#" compose.yml
grep image: compose.yml
docker image inspect "$(sed -n 's/.*image: //p' compose.yml | head -1)" >/dev/null || { echo "no such image"; exit 1; }
SP_TIMEOUT=60 $SP 'Write-VolumeCache -DriveLetter C' >/dev/null 2>&1; SP_TIMEOUT=30 $SP 'Stop-Computer -Force' >/dev/null 2>&1
for i in $(seq 1 60); do [ "$(docker inspect -f '{{.State.Running}}' WinBoatGPUSpike)" = false ] && break; echo "stopping $i"; sleep 3; done
# a VM killed on shutdown can leave compose's renamed container behind ("<id>_WinBoatGPUSpike", Created)
docker ps -a --format '{{.Names}}' | grep '_WinBoatGPUSpike$' | xargs -r timeout 60 docker rm >/dev/null
timeout 120 docker compose -f compose.yml up -d --force-recreate 2>&1 | tail -1
[ "$(docker inspect -f '{{.State.Running}}' WinBoatGPUSpike 2>/dev/null)" = true ] || { echo "container not running"; exit 1; }
for i in $(seq 1 60); do [ "$(SP_TIMEOUT=15 $SP '(Get-Process explorer -ErrorAction SilentlyContinue) -ne $null' 2>&1 | tr -d '\r' | tail -1)" = True ] && break; echo "waiting $i"; sleep 5; done
sleep 15; echo up
