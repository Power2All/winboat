#!/bin/bash
# new-patch.sh <repo> <commit>: the spike clone's work beyond tools/gpu-spike/patches/<repo>/*.patch, as a diff on
# stdout (a scratch clone at <commit> with the series applied, the clone's changed and untracked files over it)
set -euo pipefail
name=$1 commit=$2
SRC=$HOME/winboat-gpu-spike/src/$name
P=$HOME/Coding/winboat/tools/gpu-spike/patches/$name
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
git clone -q --no-checkout --shared "$SRC" "$T"
git -C "$T" checkout -q "$commit"
for p in "$P"/*.patch; do patch -d "$T" -p1 -s --no-backup-if-mismatch < "$p"; done
git -C "$T" add -A && git -C "$T" -c user.name=x -c user.email=x commit -q -m base
# files the clone changed or added (not ignored), its submodules left out
git -C "$SRC" ls-files -m -o --exclude-standard | while read -r f; do
   [ -f "$SRC/$f" ] || continue
   mkdir -p "$T/$(dirname "$f")"
   cp "$SRC/$f" "$T/$f"
done
git -C "$T" add -A
git -C "$T" diff --cached
