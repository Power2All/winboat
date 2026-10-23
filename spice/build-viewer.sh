#!/usr/bin/env bash
# Builds the bundled SPICE viewer (winboat-spice-viewer and spice-gtk) into spice/viewer (see
# spice/viewer-build/Dockerfile). Needs Docker or Podman.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="${RUNTIME:-$(command -v docker || command -v podman)}"
"$RUNTIME" build -t winboat-spice-viewer-build "$DIR/viewer-build"
rm -rf "$DIR/viewer" && mkdir -p "$DIR/viewer"
id=$("$RUNTIME" create winboat-spice-viewer-build)
trap '"$RUNTIME" rm "$id" >/dev/null' EXIT
"$RUNTIME" cp "$id:/out/." "$DIR/viewer/"
echo "SPICE viewer: $(ls "$DIR/viewer/bin" "$DIR/viewer/lib" | tr '\n' ' ')"
