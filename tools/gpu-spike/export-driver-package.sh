#!/usr/bin/env bash
# Copies the viogpu3d driver package built in the GPU spike VM (build-driver.ps1) into guest_server/drivers/viogpu3d,
# where build-guest-server.sh bundles it into WinBoat's guest payload. Also fills guest_server/drivers/spice with the
# SPICE display route's helpers: viogpuap.exe (applies the resolution the driver reports, from the same build) and the
# SPICE agent for Windows (vdagent-win, downloaded and checked against its published hash), and guest_server/drivers/
# gpucheck with the GPU check (tools/dx-test, built here by its build.sh; installed with the driver).
#
#   tools/gpu-spike/export-driver-package.sh
#
# The package is copied as built: each WinBoat VM signs it with its own certificate when the driver is installed
# (guest_server/scripts/gpu-driver.ps1), so no signing key is shipped.
set -euo pipefail

SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="$REPO_DIR/guest_server/drivers/viogpu3d"
# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$SPIKE_DIR/ssh/known_hosts"
    -o LogLevel=ERROR -i "$SPIKE_DIR/ssh/id_ed25519")
PACKAGE='C:/build/src/kvm-guest-drivers-windows/viogpu/viogpu3d/objfre_win10_amd64/amd64/viogpu3d'
VIOGPUAP='C:/build/src/kvm-guest-drivers-windows/viogpu/viogpuap/objfre_win10_amd64/amd64/viogpuap.exe'
SPICE_DEST="$REPO_DIR/guest_server/drivers/spice"
VDAGENT_URL='https://www.spice-space.org/download/windows/vdagent/vdagent-win-0.10.0/spice-vdagent-x64-0.10.0.msi'
VDAGENT_SHA256='77629435705bc27dd7d2525e9d2084f72dbab5fdbf310e812f91332fe18d00eb'

rm -rf "$DEST"
mkdir -p "$DEST"
for file in viogpu3d.inf viogpu3d.cat viogpu3d.sys \
            viogpu_wgl.dll viogpu_wgl_x86.dll viogpu_d3d9.dll viogpu_d3d9_x86.dll viogpu_d3d10.dll viogpu_d3d10_x86.dll viogpu_d3d12.dll viogpu_d3d12_x86.dll viogpu_d3d12-licenses.txt \
            viogpu_vk.dll viogpu_vk.json viogpu_vk_x86.dll viogpu_vk_x86.json \
            vulkan-1.dll vulkan-1_x86.dll vulkan-1-license.txt amfrt64.dll amfrt64-license.txt winboat_mft64.dll; do
    scp -q "${SSH_OPTS[@]}" -P 12222 "$WIN_USER@127.0.0.1:$PACKAGE/$file" "$DEST/"
done
# scp maps the Windows ACLs to odd modes
chmod 644 "$DEST"/*
# A catalog that isn't one (a spike shut down uncleanly right after signing left it zeroed) fails every install
if ! openssl pkcs7 -inform DER -in "$DEST/viogpu3d.cat" -noout 2>/dev/null; then
    echo "viogpu3d.cat isn't a catalog (zeroed by an unclean shutdown?): rebuild the package" >&2
    exit 1
fi
echo "Exported the driver package to $DEST:"
ls -l "$DEST"

rm -rf "$SPICE_DEST"
mkdir -p "$SPICE_DEST"
scp -q "${SSH_OPTS[@]}" -P 12222 "$WIN_USER@127.0.0.1:$VIOGPUAP" "$SPICE_DEST/"
curl -fsSL -o "$SPICE_DEST/spice-vdagent-x64.msi" "$VDAGENT_URL"
echo "$VDAGENT_SHA256  $SPICE_DEST/spice-vdagent-x64.msi" | sha256sum -c --quiet
chmod 644 "$SPICE_DEST"/*
echo "Exported the SPICE helpers to $SPICE_DEST:"
ls -l "$SPICE_DEST"

CHECK_DEST="$REPO_DIR/guest_server/drivers/gpucheck"
bash "$REPO_DIR/tools/dx-test/build.sh" >/dev/null
rm -rf "$CHECK_DEST"
mkdir -p "$CHECK_DEST"
cp "$REPO_DIR/tools/dx-test/dist/winboat-dxtest.exe" "$REPO_DIR/tools/dx-test/dist/winboat-dxtest32.exe" "$CHECK_DEST/"
chmod 644 "$CHECK_DEST"/*
echo "Exported the GPU check to $CHECK_DEST:"
ls -l "$CHECK_DEST"
