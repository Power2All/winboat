#!/bin/bash
# build.sh: amfrt64.dll (MinGW-w64; Arch: mingw-w64-gcc). The AMF SDK's public headers (amf/, MIT: amf/LICENSE.txt)
# and the Khronos Vulkan headers (vulkan/) are the only dependencies.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p dist
x86_64-w64-mingw32-windres amfrt.rc -O coff -o dist/amfrt.res
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wno-missing-field-initializers -Wno-unused-function -shared \
    -I. -Iamf -Ivulkan -o dist/amfrt64.dll amfrt.c vk_encoder.c dist/amfrt.res -lole32 -static-libgcc -Wl,--no-insert-timestamp
cp amf/LICENSE.txt dist/amfrt64-license.txt
x86_64-w64-mingw32-objdump -p dist/amfrt64.dll | grep -A6 'Ordinal/Name Pointer'
