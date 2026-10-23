# The dockur Windows image with a virglrenderer that has Venus (Vulkan over virtio-gpu) enabled. dockur's Debian
# package is built without it: there is no virgl_render_server, which runs the host side of Venus.
#
#   docker build -t winboat-windows-venus tools/gpu-spike/venus                # the VM image
#   docker build -t winboat-venus-test --target test tools/gpu-spike/venus     # + Mesa's Venus driver and vulkaninfo
#
# The VM then needs the device options too: VGA="virtio-vga-gl,venus=on,blob=on,hostmem=..." (dockur passes a
# custom VGA value as the -device string), and NVIDIA_DRIVER_CAPABILITIES with "graphics" for the Vulkan driver.

ARG DOCKUR_IMAGE=ghcr.io/dockur/windows:6.05
# The version dockur's Debian ships, so QEMU sees the same library ABI
ARG VIRGLRENDERER_VERSION=1.3.0

FROM debian:trixie AS build
ARG VIRGLRENDERER_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git meson ninja-build pkg-config python3 python3-yaml \
        libdrm-dev libepoxy-dev libgbm-dev libva-dev libvulkan-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "$VIRGLRENDERER_VERSION" https://gitlab.freedesktop.org/virgl/virglrenderer.git /src
WORKDIR /src
# Debian's layout, so the library replaces the packaged one and finds its render server
RUN meson setup build --buildtype=release --prefix=/usr --libdir=lib/x86_64-linux-gnu --libexecdir=libexec \
        -Dvenus=true -Dvideo=true \
    && ninja -C build \
    && DESTDIR=/out ninja -C build install

# Host-side check without a VM: Mesa's Venus driver (libvulkan_virtio.so) talks to virgl_test_server --venus.
# Plain trixie (the dockur image mixes Debian suites, so Mesa's packages don't install there).
#   docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all winboat-venus-test
FROM build AS test
RUN apt-get update && apt-get install -y --no-install-recommends libvulkan1 mesa-vulkan-drivers vulkan-tools \
    && rm -rf /var/lib/apt/lists/* \
    && cp -a /out/usr/. /usr/
COPY venus-check /usr/local/bin/
CMD ["venus-check"]

FROM ${DOCKUR_IMAGE}
# libvulkan1: the render server runs the host's Vulkan driver through the loader
RUN apt-get update && apt-get install -y --no-install-recommends libvulkan1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/usr/lib/x86_64-linux-gnu/ /usr/lib/x86_64-linux-gnu/
COPY --from=build /out/usr/libexec/ /usr/libexec/
COPY --from=build /out/usr/bin/virgl_test_server /usr/local/bin/
