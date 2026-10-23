#!/usr/bin/env bash
# Prepares the viogpu3d sources on the host and copies them into the GPU spike VM (C:\build\src), where
# build-driver.ps1 builds them. Clones at pinned commits, then applies our patches from patches/<repo>/.
#
#   tools/gpu-spike/sync-sources.sh
#
# Local changes in the clones are discarded: the patches are the source of truth. The VM's build directories are
# kept, so a rebuild only recompiles what changed.
set -euo pipefail

SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SPIKE_DIR/src"

# name, URL, branch, pinned commit
REPOS=(
    "mesa https://gitlab.freedesktop.org/max8rr8/mesa.git viogpu_win 62f1e49a1eb3a408f748725326fe53447887f97d"
    "kvm-guest-drivers-windows https://github.com/max8rr8/kvm-guest-drivers-windows.git viogpu3d 9ed3aab11fb46e55dc835ff008008623b290a6cf"
    # The base of the Direct3D 12 driver (release v3.0.1; submodules at the commits it pins)
    "vkd3d-proton https://github.com/HansKristian-Work/vkd3d-proton.git v3.0.1 3b10bd7a7ec6a7347e616cf8bea59333afec2255"
    # The Vulkan driver (Venus) from a Mesa release of its own: it forwards ray tracing, mesh shaders and the
    # shader features of D3D12's shader models 6.6-6.8, which the Mesa above (August 2024) doesn't
    "mesa-venus https://gitlab.freedesktop.org/mesa/mesa.git mesa-26.2.4 96cb43121031992b85767f9c1be8f3f48e22b1d2"
)

# Fingerprint of a clone's working tree (tracked changes + untracked files), to detect work done since the last sync
tree_state() {
    { git -C "$1" diff; git -C "$1" ls-files --others --exclude-standard | sort | while read -r f; do
        echo "$f"; sha1sum < "$1/$f"; done; } | sha1sum | cut -d' ' -f1
}

mkdir -p "$SRC"
for repo in "${REPOS[@]}"; do
    read -r name url branch commit <<< "$repo"
    if [ ! -d "$SRC/$name/.git" ]; then
        git clone --branch "$branch" "$url" "$SRC/$name"
    fi
    # Resetting would throw away development work that isn't in a patch yet
    state_file="$SRC/$name/.git/winboat-synced-state"
    if [ -f "$state_file" ] && [ "$(cat "$state_file")" != "$(tree_state "$SRC/$name")" ] && [ -z "${FORCE:-}" ]; then
        echo "$name has changes since the last sync; export them as a patch first (or FORCE=1 to discard)" >&2
        exit 1
    fi
    git -C "$SRC/$name" checkout -q --force "$commit"
    git -C "$SRC/$name" clean -q -fd
    if [ -f "$SRC/$name/.gitmodules" ]; then
        git -C "$SRC/$name" submodule update -q --init --recursive --force
        git -C "$SRC/$name" submodule foreach -q --recursive 'git clean -q -fd'
    fi
    for patch in "$SCRIPT_DIR/patches/$name"/*.patch; do
        [ -e "$patch" ] || continue
        echo "Applying $(basename "$patch") to $name"
        # patch(1): git apply leaves files inside submodules alone
        patch -d "$SRC/$name" -p1 -s --no-backup-if-mismatch < "$patch"
    done
    tree_state "$SRC/$name" > "$state_file"
done

# Mesa's Win32 Vulkan WSI includes the DirectX headers: a meson subproject that would be git-cloned at configure
# time, and the VM has no git. Put the pinned release where each Mesa's wrap expects it (subprojects/ is git-ignored)
# mesa tag commit directory
DXHEADERS_PINS=(
    "mesa v1.613.1 de28d93dfa9ebf3e473127c1c657e1920a5345ee DirectX-Headers"
    "mesa-venus v1.619.1 9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e DirectX-Headers-1.619"
)
for pin in "${DXHEADERS_PINS[@]}"; do
    read -r mesa tag commit dir <<< "$pin"
    DXHEADERS="$SPIKE_DIR/src/$dir"
    if [ ! -d "$DXHEADERS/.git" ]; then
        git clone --depth 1 --branch "$tag" https://github.com/microsoft/DirectX-Headers.git "$DXHEADERS"
    fi
    [ "$(git -C "$DXHEADERS" rev-parse HEAD)" = "$commit" ] || { echo "$dir isn't at $commit" >&2; exit 1; }
    rm -rf "$SRC/$mesa/subprojects/DirectX-Headers-1.0"
    mkdir -p "$SRC/$mesa/subprojects/DirectX-Headers-1.0"
    git -C "$DXHEADERS" archive HEAD | tar -x -C "$SRC/$mesa/subprojects/DirectX-Headers-1.0"
done

# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$SPIKE_DIR/ssh/known_hosts"
    -o LogLevel=ERROR -i "$SPIKE_DIR/ssh/id_ed25519")

ARCHIVE="$SPIKE_DIR/src.tgz"
# vkd3d-proton: dxil-spirv's SPIRV-Tools and SPIRV-Cross are only for its CMake tools, not the Meson build
tar -C "$SRC" --exclude=.git --exclude=vkd3d-proton/subprojects/dxil-spirv/third_party/SPIRV-Tools \
    --exclude=vkd3d-proton/subprojects/dxil-spirv/third_party/SPIRV-Cross \
    -czf "$ARCHIVE" mesa mesa-venus kvm-guest-drivers-windows vkd3d-proton
scp "${SSH_OPTS[@]}" -P 12222 "$ARCHIVE" "$WIN_USER@127.0.0.1:C:/build/src.tgz" >/dev/null
# Windows' tar keeps the archive's timestamps, so files the patches didn't touch stay up to date for ninja/MSBuild
ssh "${SSH_OPTS[@]}" -p 12222 "$WIN_USER@127.0.0.1" \
    'New-Item -ItemType Directory -Force C:\build\src | Out-Null; tar -xzf C:\build\src.tgz -C C:\build\src; if ($LASTEXITCODE) { exit 1 }'
echo "Sources synced to C:\\build\\src"
