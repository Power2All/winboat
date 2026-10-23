#!/bin/bash
# build.sh: winboat_mft64.dll (MinGW-w64), the Media Foundation encoders, with the AMF runtime's Vulkan Video encoder
# (../amf-runtime/vk_encoder.c and its Vulkan headers)
set -euo pipefail
cd "$(dirname "$0")"
A=../amf-runtime
mkdir -p dist
x86_64-w64-mingw32-windres mft.rc -O coff -o dist/mft.res
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wno-missing-field-initializers -c -I$A -I$A/vulkan -o dist/vk_encoder.o $A/vk_encoder.c
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -shared -fno-exceptions -fno-rtti -I$A -o dist/winboat_mft64.dll mft.cpp \
    dist/vk_encoder.o dist/mft.res -lmfplat -lmfuuid -lole32 -loleaut32 -luuid -ld3d11 -static \
    -Wl,--no-insert-timestamp
x86_64-w64-mingw32-objdump -p dist/winboat_mft64.dll | grep -A8 'Ordinal/Name Pointer' | grep Dll
