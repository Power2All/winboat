#!/bin/bash
# build.sh: OpenCL.dll, Khronos' OpenCL ICD loader (Apache-2.0), 64- and 32-bit, MinGW-w64 with static runtime
# libraries (no Visual C++ runtime needed). Windows 10 has no OpenCL.dll of its own (11 has it): with this one,
# Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack (OpenCLOn12, which the loader finds by its package family)
# runs OpenCL on the GPU there. gpu-driver.ps1 installs it where Windows has none. Sources: the release tag below,
# fetched into build/.
set -euo pipefail
cd "$(dirname "$0")"
TAG=v2026.05.29
mkdir -p build dist
for r in OpenCL-ICD-Loader OpenCL-Headers; do
    [ -d "build/$r" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" "https://github.com/KhronosGroup/$r.git" "build/$r"
done
for arch in x86_64:x64 i686:x86; do
    triple="${arch%%:*}-w64-mingw32" dir="${arch##*:}"
    cat > "build/$dir.cmake" <<TC
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER $triple-gcc)
set(CMAKE_RC_COMPILER $triple-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$triple)
TC
    cmake -S build/OpenCL-ICD-Loader -B "build/$dir" -DCMAKE_TOOLCHAIN_FILE="$PWD/build/$dir.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DOPENCL_ICD_LOADER_HEADERS_DIR="$PWD/build/OpenCL-Headers" -DBUILD_TESTING=OFF -DENABLE_OPENCL_LAYERINFO=OFF \
        -DCMAKE_C_FLAGS="-static-libgcc" -DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc -s -Wl,--no-insert-timestamp" >/dev/null
    cmake --build "build/$dir" -j"$(nproc)" >/dev/null
    mkdir -p "dist/$dir"
    cp "build/$dir/OpenCL.dll" "dist/$dir/OpenCL.dll"
    "$triple-objdump" -p "dist/$dir/OpenCL.dll" | grep 'DLL Name' | tr -s ' ' | sed "s/^/$dir:/"
done
cp build/OpenCL-ICD-Loader/LICENSE dist/OpenCL-license.txt
