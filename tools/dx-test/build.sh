#!/usr/bin/env bash
# Cross-compiles winboat-dxtest.exe (64-bit) and winboat-dxtest32.exe (32-bit: the 32-bit test groups) on Linux with
# MinGW-w64 (Arch: mingw-w64-gcc)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
mkdir -p dist
# The Vulkan headers (MinGW-w64 has none): Khronos' release, pinned
VKH_VERSION=1.3.296
VKH_SHA256=e204e0b3c19f622d197df945737f5db913d6621830999b8578d34e80a7c90585
VKH="dist/Vulkan-Headers-$VKH_VERSION"
if [ ! -f "$VKH/include/vulkan/vulkan.h" ]; then
    curl -fsSL -o dist/vulkan-headers.tar.gz "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$VKH_VERSION.tar.gz"
    echo "$VKH_SHA256  dist/vulkan-headers.tar.gz" | sha256sum -c --quiet
    tar -xzf dist/vulkan-headers.tar.gz -C dist
    rm dist/vulkan-headers.tar.gz
fi
# DXTEST_AGILITY=<SDK version>: winboat-dxtest-agility(32).exe, Direct3D 12 from that Agility SDK's D3D12Core.dll in a
# D3D12 folder beside them (not included)
AGILITY=() SUFFIX=""
[ -n "${DXTEST_AGILITY:-}" ] && AGILITY=(-DDXTEST_AGILITY="$DXTEST_AGILITY") SUFFIX=-agility
for target in x86_64:winboat-dxtest i686:winboat-dxtest32; do
    arch="${target%%:*}" name="${target##*:}"
    name="${name/dxtest/dxtest$SUFFIX}"
    "$arch-w64-mingw32-g++" "${AGILITY[@]}" -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Wno-cast-function-type \
        -Wno-unused-function -Wno-unused-variable -I "$VKH/include" -static dxtest.cpp -o "dist/$name.exe" \
        -ld3d9 -ld3d10 -ld3d11 -ldxgi -ld2d1 -ldwrite -ldxva2 -lgdi32 -luser32 -lole32 -loleaut32 -lshell32 -lopengl32 -lmfuuid -lstrmiids -lwmcodecdspuuid
    echo "Built $(pwd)/dist/$name.exe"
done
