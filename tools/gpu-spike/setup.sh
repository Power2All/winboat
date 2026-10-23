#!/usr/bin/env bash
# Creates a disposable Windows VM for testing virtio-gpu 3D drivers (Phase 3 spike).
# It is completely separate from a real WinBoat install: own directory, container name, storage and ports.
#
#   tools/gpu-spike/setup.sh        # writes ~/winboat-gpu-spike (or $SPIKE_DIR)
#   docker compose -f ~/winboat-gpu-spike/compose.yml up -d
#
# VENUS=1 also enables Vulkan over virtio-gpu: it builds the winboat-windows-venus image (tools/gpu-spike/venus,
# dockur with a Venus-enabled virglrenderer) and adds venus/blob/hostmem to the GPU device.
#
# Host ports (localhost only): 18006 web viewer, 13389 RDP, 12222 SSH
set -euo pipefail

SPIKE_DIR="${SPIKE_DIR:-$HOME/winboat-gpu-spike}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$SPIKE_DIR"/{storage,oem,ssh}

# SSH key the host uses to run commands in the VM
if [ ! -f "$SPIKE_DIR/ssh/id_ed25519" ]; then
    ssh-keygen -q -t ed25519 -N "" -C "winboat-gpu-spike" -f "$SPIKE_DIR/ssh/id_ed25519"
fi
cp "$SPIKE_DIR/ssh/id_ed25519.pub" "$SPIKE_DIR/oem/authorized_keys"

# Windows account (only reachable from localhost)
if [ ! -f "$SPIKE_DIR/credentials" ]; then
    printf 'WIN_USER=spike\nWIN_PASSWORD=%s\n' "$(head -c 18 /dev/urandom | base64 | tr -d '/+=')" > "$SPIKE_DIR/credentials"
    chmod 600 "$SPIKE_DIR/credentials"
fi
# shellcheck source=/dev/null
. "$SPIKE_DIR/credentials"

cp "$SCRIPT_DIR/install.bat" "$SPIKE_DIR/oem/install.bat"

IMAGE="ghcr.io/dockur/windows:6.05"
VENUS_ENV=""
if [ "${VENUS:-0}" = 1 ]; then
    docker build -t winboat-windows-venus "$SCRIPT_DIR/venus"
    IMAGE="winboat-windows-venus"
    # dockur passes a custom VGA value as the -device string. hostmem: the window host-visible Vulkan memory is
    # mapped through.
    VENUS_ENV='      VGA: "virtio-vga-gl,venus=on,blob=on,hostmem=4G"'
fi

GPU_DEVICE_REQUEST=""
GPU_ENV=""
if command -v nvidia-smi >/dev/null 2>&1; then
    # The toolkit mounts the host's NVIDIA libraries at the host's paths (/usr/lib on Arch, /usr/lib64 on Fedora),
    # but the container's Debian Mesa only looks for GBM backends in its own multiarch directory
    GPU_ENV='      GBM_BACKENDS_PATH: "/usr/lib/x86_64-linux-gnu/gbm:/usr/lib/gbm:/usr/lib64/gbm"'
    # NVIDIA hosts: injects the proprietary EGL/GBM libraries virglrenderer needs
    GPU_DEVICE_REQUEST='    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: all
              capabilities: [gpu, graphics, display, utility]'
fi

cat > "$SPIKE_DIR/compose.yml" <<EOF
name: winboat-gpu-spike
services:
  windows:
    image: $IMAGE
    container_name: WinBoatGPUSpike
    environment:
      VERSION: "11"
      RAM_SIZE: "8G"
      CPU_CORES: "4"
      DISK_SIZE: "128G"
      USERNAME: "$WIN_USER"
      PASSWORD: "$WIN_PASSWORD"
      USER_PORTS: "22"
      GPU: "Y"
$VENUS_ENV
$GPU_ENV
    devices:
      - /dev/kvm
      - /dev/dri
    cap_add:
      - NET_ADMIN
    ports:
      - 127.0.0.1:18006:8006
      - 127.0.0.1:13389:3389/tcp
      - 127.0.0.1:13389:3389/udp
      - 127.0.0.1:12222:22
    volumes:
      - ./storage:/storage
      - ./oem:/oem
    stop_grace_period: 2m
$GPU_DEVICE_REQUEST
EOF

echo "Spike VM prepared in $SPIKE_DIR"
echo "Start:  docker compose -f $SPIKE_DIR/compose.yml up -d"
echo "Watch:  http://127.0.0.1:18006"
echo "SSH:    ssh -i $SPIKE_DIR/ssh/id_ed25519 -p 12222 $WIN_USER@127.0.0.1   (after install finishes)"
