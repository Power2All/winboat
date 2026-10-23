# The dockur Windows image with SPICE GL scanouts that carry the buffer's format modifier (GL_SCANOUT2):
# spice-server 0.16 and a QEMU built against it. The modifier lets a SPICE client import the virtio-gpu (virgl)
# screen zero-copy where the GPU's buffers are tiled and the driver has no implicit modifiers (NVIDIA). dockur 6.05
# ships spice-server 0.15, which drops the modifier: the client then imports the buffer as linear and shows black.
# The client needs GL_SCANOUT2 support too (spice-gtk newer than 0.42).
#
#   docker build -t winboat-windows-spice -f tools/gpu-spike/spice/Dockerfile tools/gpu-spike
#   docker build -t winboat-windows-spice-venus --build-arg BASE_IMAGE=winboat-windows-venus \
#       -f tools/gpu-spike/spice/Dockerfile tools/gpu-spike                    # for the spike VM
#
# The VM then runs with GPU=N and DISPLAY=none (dockur's GPU=Y always adds egl-headless, which can't share the screen
# with SPICE GL) and ARGUMENTS="-device virtio-vga-gl -spice unix=on,addr=<socket>,disable-ticketing=on,gl=on,
# rendernode=/dev/dri/renderD128", the socket on a volume the host client can reach.

ARG BASE_IMAGE=ghcr.io/dockur/windows:6.05

FROM debian:trixie AS build
# dockur 6.05's QEMU version
ARG QEMU_VERSION=11.1.0
ARG SPICE_PROTOCOL_VERSION=0.14.5
ARG SPICE_VERSION=0.16.0
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils bzip2 meson ninja-build pkg-config python3 python3-venv \
        python3-pyparsing flex bison \
        libglib2.0-dev libpixman-1-dev zlib1g-dev libzstd-dev libssl-dev liblz4-dev \
        libepoxy-dev libgbm-dev libdrm-dev libvirglrenderer-dev \
        libaio-dev liburing-dev libfdt-dev libusb-1.0-0-dev libusbredirparser-dev libusbredirhost-dev \
        libgnutls28-dev libslirp-dev libseccomp-dev libcap-ng-dev libasound2-dev libjpeg-dev libpng-dev \
    && rm -rf /var/lib/apt/lists/*

# SPICE (Debian's layout, so the library replaces the image's)
RUN curl -fsSL "https://www.spice-space.org/download/releases/spice-protocol-${SPICE_PROTOCOL_VERSION}.tar.xz" \
        | tar -xJ -C / \
    && cd "/spice-protocol-${SPICE_PROTOCOL_VERSION}" && meson setup build --prefix=/usr && ninja -C build install
RUN curl -fsSL "https://www.spice-space.org/download/releases/spice-server/spice-${SPICE_VERSION}.tar.bz2" \
        | tar -xj -C / \
    && cd "/spice-${SPICE_VERSION}" \
    && meson setup build --buildtype=release --prefix=/usr --libdir=lib/x86_64-linux-gnu \
        -Dgstreamer=no -Dsasl=false -Dopus=disabled -Dsmartcard=disabled -Dmanual=false -Dtests=false \
    && ninja -C build install && DESTDIR=/out ninja -C build install

# QEMU with the features dockur's command lines use (KVM, tap and user networking, Linux AIO, USB, VNC with
# websocket, TPM, audio), built in rather than as modules
RUN curl -fsSL "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" | tar -xJ -C / && mv "/qemu-${QEMU_VERSION}" /src
WORKDIR /src
RUN ./configure --prefix=/usr --firmwarepath=/usr/share/qemu:/usr/share/seabios:/usr/lib/ipxe/qemu \
        --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-debug-info \
        --enable-kvm --enable-spice --enable-opengl --enable-virglrenderer \
        --enable-linux-aio --enable-linux-io-uring --enable-libusb --enable-usb-redir \
        --enable-vnc --enable-gnutls --enable-slirp --enable-seccomp --enable-cap-ng --enable-tpm \
        --enable-alsa --disable-pa --enable-png --enable-vnc-jpeg \
    && grep -q '^#define HAVE_SPICE_QXL_GL_SCANOUT2' build/config-host.h \
    && ninja -C build qemu-system-x86_64

FROM ${BASE_IMAGE}
COPY --from=build /out/usr/lib/x86_64-linux-gnu/libspice-server.so.1.* /usr/lib/x86_64-linux-gnu/
COPY --from=build /src/build/qemu-system-x86_64 /usr/bin/qemu-system-x86_64
# The image's own spice-server 0.15 is replaced, and every library the new binaries need must already be in the image
RUN ln -sf "$(cd /usr/lib/x86_64-linux-gnu && ls libspice-server.so.1.* | sort -V | tail -1)" \
        /usr/lib/x86_64-linux-gnu/libspice-server.so.1 \
    && readlink /usr/lib/x86_64-linux-gnu/libspice-server.so.1 \
    && if ldd /usr/bin/qemu-system-x86_64 | grep 'not found'; then exit 1; fi
