#!/bin/bash
# testvm-install.sh <win10|win81|win11>...: fresh unattended installs (dockur, Microsoft's images; oem/install.bat:
# OpenSSH only), one at a time: up, wait for C:\OEM\spike-ready.txt (bounded, progress lines), stopped. The disks
# then become the virt-manager test VMs (testvm-define.sh)
set -uo pipefail
T=$(cd "$(dirname "$0")" && pwd)
for v in "$@"; do
   D=~/winboat-gpu-spike/$v
   port=$(grep -o '127.0.0.1:[0-9]*:22' $D/compose.yml | cut -d: -f2)
   name=$(grep -o 'container_name: .*' $D/compose.yml | cut -d' ' -f2)
   echo "== $v ($name, ssh $port)"
   ls $D/storage/data.img >/dev/null 2>&1 && { echo "$v: storage not empty, skipped"; continue; }
   (cd $D && docker compose up -d 2>&1 | tail -1)
   ssh-keygen -q -R "[127.0.0.1]:$port" -f ~/winboat-gpu-spike/ssh/known_hosts >/dev/null 2>&1
   ok=no
   for i in $(seq 1 240); do   # 2 h
      r=$(SP_PORT=$port SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\OEM\spike-ready.txt' 2>/dev/null | tr -d '\r')
      [ "$r" = True ] && { ok=yes; break; }
      echo "$v: waiting ($i, $(du -sh $D/storage 2>/dev/null | cut -f1))"; sleep 30
   done
   echo "$v: ready=$ok"
   SP_PORT=$port SP_TIMEOUT=20 "$T/sp.sh" 'Stop-Computer -Force' >/dev/null 2>&1
   sleep 60; docker stop -t 120 $name >/dev/null
   [ $ok = yes ] || exit 1
done
