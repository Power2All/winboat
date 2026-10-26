#!/usr/bin/env bash
# Builds the GPU benchmarks with MinGW-w64 (Arch: mingw-w64-gcc): gpubench.exe (Direct3D 11, OpenGL), presentbench.exe
# (Direct3D 11/12 presents), lagtest.exe (a display path's frames
# and key-to-pixel latency, with tools/gpu-spike/deploy/tools/lagmeter.c) and vkpresentbench.exe (Vulkan presents; the Vulkan headers dx-test's build.sh fetches)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p dist
CXX="x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wno-missing-field-initializers"
$CXX gpubench.cpp -o dist/gpubench.exe -ld3d11 -ldxgi -luser32 -lgdi32 -lopengl32
$CXX presentbench.cpp -o dist/presentbench.exe -ld3d11 -ld3d12 -ldxgi -luser32
$CXX lagtest.cpp -o dist/lagtest.exe -ld3d11 -ldxgi -luser32
VKH=$(ls -d ../dx-test/dist/Vulkan-Headers-* 2>/dev/null | head -1)
[ -n "$VKH" ] || bash ../dx-test/build.sh >/dev/null
VKH=$(ls -d ../dx-test/dist/Vulkan-Headers-* | head -1)
$CXX -I "$VKH/include" vkpresentbench.cpp -o dist/vkpresentbench.exe -luser32  # (loads vulkan-1.dll itself)
ls -la dist/*.exe
