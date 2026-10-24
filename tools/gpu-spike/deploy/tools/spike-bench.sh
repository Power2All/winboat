#!/bin/bash
# spike-bench.sh [tag]: the GPU benchmarks (tools/gpu-bench) in the spike's desktop session: gpubench (Direct3D 11,
# OpenGL; Direct3D 11 on WARP for scale), presentbench (Direct3D 11/12 presents) and vkpresentbench (Vulkan) at 1080p
# and 1440p. Output in results/bench-<tag>.txt (default: the date)
T=$(dirname "$0"); . ~/winboat-gpu-spike/credentials
B=~/Coding/winboat/tools/gpu-bench/dist
TAG=${1:-$(date +%Y%m%d-%H%M)}
mkdir -p "$T/results"
{ printf '@echo off\r\ncd /d C:\\build\\bench\r\n'
  printf 'gpubench.exe > out.txt 2>&1\r\ngpubench.exe --warp --only d3d11 >> out.txt 2>&1\r\n'
  for s in "1920 1080" "2560 1440"; do
     printf 'echo presentbench d3d11 %s >> out.txt\r\npresentbench.exe d3d11 %s 5 >> out.txt 2>&1\r\n' "$s" "$s"
     printf 'echo presentbench d3d12 %s >> out.txt\r\npresentbench.exe d3d12 %s 5 >> out.txt 2>&1\r\n' "$s" "$s"
     printf 'echo vkpresentbench %s >> out.txt\r\nvkpresentbench.exe %s 5 >> out.txt 2>&1\r\n' "$s" "$s"
  done
  printf 'echo done > done\r\n'; } > "$T/bench.cmd"
SP_TIMEOUT=30 "$T/sp.sh" 'New-Item -ItemType Directory -Force C:\build\bench | Out-Null; Remove-Item C:\build\bench\done, C:\build\bench\out.txt -ErrorAction SilentlyContinue'
for f in $B/gpubench.exe $B/presentbench.exe $B/vkpresentbench.exe "$T/bench.cmd"; do SP_TIMEOUT=120 "$T/sp.sh" --put "$f" "C:/build/bench/$(basename $f)" || exit 1; done
SP_TIMEOUT=30 "$T/sp.sh" "schtasks /Create /F /TN WinBoatBench /TR 'cmd.exe /c C:\\build\\bench\\bench.cmd' /SC ONCE /ST 23:59 /RU $WIN_USER /IT | Out-Null; schtasks /Run /TN WinBoatBench | Out-Null"
for i in $(seq 1 120); do SP_TIMEOUT=15 "$T/sp.sh" 'Test-Path C:\build\bench\done' 2>/dev/null | grep -q True && break; echo "running ($i)"; sleep 5; done
SP_TIMEOUT=60 "$T/sp.sh" --get C:/build/bench/out.txt "$T/results/bench-$TAG.txt" && tr -d '\r' < "$T/results/bench-$TAG.txt"
