#!/usr/bin/env bash
# Development shortcut: copies the files that differ from the pinned commit (changed and new ones) from the host's
# source clones into the GPU spike VM, without resetting anything. Then build in the VM, e.g. with
# build-driver.ps1 -UserModeOnly. Export the work as a patch in patches/<repo>/ before running sync-sources.sh.
#
#   tools/gpu-spike/push-dev.sh [mesa|mesa-venus|vkd3d-proton|kvm-guest-drivers-windows]...
#
# Only files whose content changed since the last push are sent, and they get the VM's current time: the host's
# timestamps can't be trusted to be newer than the VM's build outputs (the clocks disagree), and a stale-looking
# source is silently not recompiled.
set -euo pipefail

SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o "UserKnownHostsFile=$SPIKE_DIR/ssh/known_hosts"
    -o LogLevel=ERROR -i "$SPIKE_DIR/ssh/id_ed25519")

repos=("$@")
[ ${#repos[@]} -gt 0 ] || repos=(mesa kvm-guest-drivers-windows)
for name in "${repos[@]}"; do
    dir="$SPIKE_DIR/src/$name"
    # What the VM has: "<sha1>  <path>" per pushed file. Files pushed before stay candidates: one changed back to the
    # pinned version doesn't show in the diff anymore, but the VM still has the changed copy.
    state="$SPIKE_DIR/pushed-$name.sha1"
    touch "$state"
    current="$(mktemp)"
    { git -C "$dir" diff --name-only; git -C "$dir" ls-files --others --exclude-standard; cut -c43- "$state"; } |
        sort -u | while read -r f; do [ -f "$dir/$f" ] && (cd "$dir" && sha1sum "$f"); done > "$current"
    mapfile -t files < <(sort "$state" | comm -13 - <(sort "$current") | cut -c43-)
    if [ ${#files[@]} -eq 0 ]; then
        rm -f "$current"
        echo "$name: nothing changed"
        continue
    fi
    archive="$SPIKE_DIR/push-$name.tgz"
    tar -C "$dir" -czf "$archive" "${files[@]}"
    scp "${SSH_OPTS[@]}" -P 12222 "$archive" "$WIN_USER@127.0.0.1:C:/build/push.tgz" >/dev/null
    # -m: the files get the extraction time, so they are newer than anything built from their old content
    ssh "${SSH_OPTS[@]}" -p 12222 "$WIN_USER@127.0.0.1" "tar -xmzf C:\\build\\push.tgz -C C:\\build\\src\\$name"
    mv "$current" "$state"
    echo "$name: pushed ${#files[@]} changed files"
done
