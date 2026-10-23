#!/usr/bin/env bash
# Cross-compiles winboat-gpucheck.exe (64-bit) and winboat-gpucheck32.exe (32-bit, what most older apps and D3D9
# games are) on Linux with MinGW-w64 (Arch: mingw-w64-gcc)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p dist
for target in x86_64:winboat-gpucheck i686:winboat-gpucheck32; do
    arch="${target%%:*}" name="${target##*:}"
    "$arch-w64-mingw32-g++" -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type \
        -static gpucheck.cpp -o "dist/$name.exe" \
        -ld3d11 -ld3d12 -ld3d9 -ldxgi -ldwmapi -lopengl32 -lgdi32 -luser32 -lsetupapi -lcfgmgr32 -lole32
    echo "Built $(pwd)/dist/$name.exe"
done
