# The dockur Windows image with SPICE GL scanouts that carry the buffer's format modifier (GL_SCANOUT2):
# spice-server 0.16 and a QEMU built against it. The modifier lets a SPICE client import the virtio-gpu (virgl)
# screen zero-copy where the GPU's buffers are tiled and the driver has no implicit modifiers (NVIDIA). dockur 6.05
# ships spice-server 0.15, which drops the modifier: the client then imports the buffer as linear and shows black.
# The client needs GL_SCANOUT2 support too (spice-gtk newer than 0.42).
#
# patches/ holds QEMU fixes for the SPICE route, virglrenderer-patches/ virglrenderer's and nvidia-vaapi-patches/
# nvidia-vaapi-driver's (see each patch). QEMU is built against virglrenderer 1.3.0, the
# version the dockur image runs (Debian trixie's headers are 1.1.0): from 1.1.2 on, virglrenderer signals finished
# GPU work right away, where QEMU otherwise polls for it every 10 ms.
#
# WinBoat builds it on this computer the first time the SPICE display route is chosen (src/renderer/lib/virtualGpu.ts),
# with BASE_IMAGE set to the dockur image it uses:
#
#   docker build -t winboat-windows-spice:6.05-14 spice/image
#
# The VM then runs with GPU=N and DISPLAY=none (dockur's GPU=Y always adds egl-headless, which can't share the screen
# with SPICE GL) and ARGUMENTS="-device vhost-user-vga,chardev=winboat-gpu -chardev socket,id=winboat-gpu,
# path=/run/shm/winboat-gpu.sock -spice unix=on,addr=<socket>,disable-ticketing=on,gl=on,
# rendernode=/dev/dri/renderD128", the socket on a volume the host client can reach.
#
# The virtual GPU runs in its own process: QEMU's vhost-user-gpu, started by qemu-wrapper.sh (installed as
# qemu-system-x86_64) when the arguments ask for it. With QEMU's own virtio-vga-gl, virglrenderer's GL work ran on
# QEMU's main loop and held up the emulated sound card and the vCPUs while the guest drew a lot (crackling sound).
# "-device virtio-vga-gl" still works (VMs set up by older WinBoat versions).
#
# Vulkan (Venus): "-device vhost-user-vga,chardev=winboat-gpu,hostmem=<size>" gives the device a host-visible memory
# window (BAR 4), and the wrapper then starts vhost-user-gpu with --venus (QEMU patch 0008). The container needs
# NVIDIA_DRIVER_CAPABILITIES with "graphics" for the host's Vulkan driver.

ARG BASE_IMAGE=ghcr.io/dockur/windows:6.05

FROM debian:trixie AS build
# dockur 6.05's QEMU version
ARG QEMU_VERSION=11.1.0
ARG SPICE_PROTOCOL_VERSION=0.14.5
ARG SPICE_VERSION=0.16.0
ARG VIRGLRENDERER_VERSION=1.3.0
# Video for the guest (virgl's video through the host's VA-API): NVIDIA's VA-API driver and the codec headers it uses
ARG NVIDIA_VAAPI_VERSION=v0.0.14
ARG NV_CODEC_HEADERS_VERSION=n12.2.72.0
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils bzip2 meson ninja-build pkg-config python3 python3-venv \
        python3-pyparsing python3-yaml flex bison \
        libglib2.0-dev libpixman-1-dev zlib1g-dev libzstd-dev libssl-dev liblz4-dev \
        libepoxy-dev libgbm-dev libdrm-dev libvulkan-dev libva-dev libegl-dev libgstreamer-plugins-bad1.0-dev \
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

# virglrenderer: QEMU only needs its headers (the image has the library)
RUN curl -fsSL "https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/${VIRGLRENDERER_VERSION}/virglrenderer-${VIRGLRENDERER_VERSION}.tar.bz2" \
        | tar -xj -C / \
    && cd "/virglrenderer-${VIRGLRENDERER_VERSION}" \
    && meson setup build --buildtype=release --prefix=/usr --libdir=lib/x86_64-linux-gnu -Dtests=false \
    && ninja -C build install

# nvidia-vaapi-driver (VA-API on NVDEC), built here: Debian's package needs libraries newer than the dockur image's
# (and gstreamer's codec parsers, without which it has no VP9); with the fixes in nvidia-vaapi-patches/
COPY nvidia-vaapi-patches/ /nvidia-vaapi-patches/
RUN curl -fsSL "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/${NV_CODEC_HEADERS_VERSION}.tar.gz" \
        | tar -xz -C / \
    && make -C "/nv-codec-headers-${NV_CODEC_HEADERS_VERSION}" install PREFIX=/usr \
    && curl -fsSL "https://github.com/elFarto/nvidia-vaapi-driver/archive/refs/tags/${NVIDIA_VAAPI_VERSION}.tar.gz" \
        | tar -xz -C / \
    && cd "/nvidia-vaapi-driver-${NVIDIA_VAAPI_VERSION#v}" \
    && for p in /nvidia-vaapi-patches/*.patch; do patch -p1 --forward < "$p" || exit 1; done \
    && meson setup build --buildtype=release --prefix=/usr --libdir=lib/x86_64-linux-gnu \
    && ninja -C build && DESTDIR=/nvva-out ninja -C build install \
    && test -f /nvva-out/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so

# QEMU with the features dockur's command lines use (KVM, tap and user networking, Linux AIO, USB, VNC with
# websocket, TPM, audio), built in rather than as modules
RUN curl -fsSL "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" | tar -xJ -C / && mv "/qemu-${QEMU_VERSION}" /src
WORKDIR /src
COPY patches/ /patches/
RUN for p in /patches/*.patch; do patch -p1 --forward < "$p" || exit 1; done
RUN ./configure --prefix=/usr --firmwarepath=/usr/share/qemu:/usr/share/seabios:/usr/lib/ipxe/qemu \
        --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-debug-info \
        --enable-kvm --enable-spice --enable-opengl --enable-virglrenderer \
        --enable-linux-aio --enable-linux-io-uring --enable-libusb --enable-usb-redir \
        --enable-vnc --enable-gnutls --enable-slirp --enable-seccomp --enable-cap-ng --enable-tpm \
        --enable-alsa --disable-pa --enable-png --enable-vnc-jpeg --enable-tools \
    && grep -q '^#define HAVE_SPICE_QXL_GL_SCANOUT2' build/config-host.h \
    && grep -q "VIRGL_RENDERER_ASYNC_FENCE_CB" /usr/include/virgl/virglrenderer.h \
    && ninja -C build qemu-system-x86_64 contrib/vhost-user-gpu/vhost-user-gpu

# The virglrenderer library the image runs (the GPU process uses it): the same version, with the fixes in
# virglrenderer-patches/ (see each patch). Built after QEMU, so a change here doesn't rebuild QEMU.
COPY virglrenderer-patches/ /virglrenderer-patches/
RUN cd "/virglrenderer-${VIRGLRENDERER_VERSION}" \
    && for p in /virglrenderer-patches/*.patch; do patch -p1 --forward < "$p" || exit 1; done \
    && rm -rf build && meson setup build --buildtype=release --prefix=/usr --libdir=lib/x86_64-linux-gnu -Dtests=false \
        --libexecdir=libexec -Dvenus=true -Dvideo=true \
    && ninja -C build && DESTDIR=/virgl-out ninja -C build install \
    && test -x /virgl-out/usr/libexec/virgl_render_server

FROM ${BASE_IMAGE}
COPY --from=build /out/usr/lib/x86_64-linux-gnu/libspice-server.so.1.* /usr/lib/x86_64-linux-gnu/
COPY --from=build /virgl-out/usr/lib/x86_64-linux-gnu/libvirglrenderer.so.1.* /usr/lib/x86_64-linux-gnu/
# Video: VA-API on NVIDIA GPUs (the VA-API drivers of AMD/Intel GPUs, Mesa's and Intel's, are not in the image yet)
COPY --from=build /nvva-out/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so /usr/lib/x86_64-linux-gnu/dri/
# and gstreamer's codec parsers, which its VP9 decoding parses frame headers with: from the build stage (the dockur
# image's newer X11 libraries keep apt from installing gstreamer-plugins-bad)
COPY --from=build /usr/lib/x86_64-linux-gnu/libgstcodecparsers-1.0.so.0* /usr/lib/x86_64-linux-gnu/libgstbase-1.0.so.0* \
     /usr/lib/x86_64-linux-gnu/libgstreamer-1.0.so.0* /usr/lib/x86_64-linux-gnu/
# Venus (Vulkan): virglrenderer's render server runs the host's Vulkan driver through the loader
COPY --from=build /virgl-out/usr/libexec/virgl_render_server /usr/libexec/virgl_render_server
# libxext6: NVIDIA's Vulkan driver (libGLX_nvidia) needs it, and the dockur image lacks it; libdw1t64: gstreamer's
RUN apt-get update && apt-get install -y --no-install-recommends libvulkan1 libxext6 libdw1t64 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/qemu-system-x86_64 /usr/libexec/qemu-system-x86_64
COPY --from=build /src/build/contrib/vhost-user-gpu/vhost-user-gpu /usr/libexec/vhost-user-gpu
COPY qemu-wrapper.sh /usr/bin/qemu-system-x86_64
# The image's own spice-server 0.15 is replaced, and every library the new binaries need must already be in the image
# (the wrapper's mode isn't trusted: WinBoat ships this folder inside its package)
RUN chmod 755 /usr/bin/qemu-system-x86_64 \
    && ln -sf "$(cd /usr/lib/x86_64-linux-gnu && ls libspice-server.so.1.* | sort -V | tail -1)" \
        /usr/lib/x86_64-linux-gnu/libspice-server.so.1 \
    && readlink /usr/lib/x86_64-linux-gnu/libspice-server.so.1 \
    && ln -sf "$(cd /usr/lib/x86_64-linux-gnu && ls libvirglrenderer.so.1.* | sort -V | tail -1)" \
        /usr/lib/x86_64-linux-gnu/libvirglrenderer.so.1 \
    && readlink /usr/lib/x86_64-linux-gnu/libvirglrenderer.so.1 \
    && if ldd /usr/libexec/qemu-system-x86_64 /usr/libexec/vhost-user-gpu | grep 'not found'; then exit 1; fi \
    && qemu-system-x86_64 --version \
    && dpkg --compare-versions "$(dpkg-query -W -f='${Version}' libvirglrenderer1)" ge 1.1.2
