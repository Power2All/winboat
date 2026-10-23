#!/bin/bash
# build-appimage.sh <version>: the WinBoat 0.9.2-gpu AppImage from the worktree (~/Coding/winboat-0.9.2-gpu), after
# syncing the driver package, gpu-driver.ps1 and spice/image from the main checkout
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
M=~/Coding/winboat; W=~/Coding/winboat-0.9.2-gpu
rsync -a --delete $M/guest_server/drivers/ $W/guest_server/drivers/
cp $M/guest_server/scripts/gpu-driver.ps1 $W/guest_server/scripts/
rsync -a --delete $M/spice/image/ $W/spice/image/
export PATH=$T/shim:$HOME/.bun/bin:$W/node_modules/.bin:$PATH
cd $W
sed -i "s/\"version\": \"0.9.2-gpu\.[0-9]*\"/\"version\": \"$1\"/" package.json
grep '"version"' package.json
mv package-lock.json /tmp/claude-1000/package-lock.json.aside
trap 'mv /tmp/claude-1000/package-lock.json.aside package-lock.json' EXIT
bash build-guest-server.sh
bun scripts/build.ts
test -f build/main/main.js || { echo "main not built"; exit 1; }
bunx electron-builder --linux AppImage
ls -la dist/winboat-$1-x86_64.AppImage
