#!/bin/bash
# spike-up.sh [tag]: (re)starts the spike container (optionally on winboat-windows-spice:<tag>, clean shutdown first)
# and waits for the desktop. spike-restart.sh does it (bounded shutdown: the SSH session of Stop-Computer can hang
# until it times out, which stalled install-built.sh; compose's leftover container)
exec "$(dirname "$0")/spike-restart.sh" "$@"
