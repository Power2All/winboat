# Builds the SPICE viewer WinBoat bundles: spice-gtk 0.43 (the first with GL_SCANOUT2, which NVIDIA needs) and
# winboat-spice-viewer (winboat-spice-viewer.c: the display in a plain window), on Ubuntu 22.04 so the binaries run
# on older glibc versions too. The result is /out/{bin,lib}: the viewer and the spice-gtk libraries (rpath
# $ORIGIN/../lib); everything else (GTK 3, GLib, GStreamer, libepoxy) comes from the host. patches/ holds spice-gtk
# fixes (see each patch). Built by spice/build-viewer.sh.
FROM ubuntu:22.04
ARG SPICE_PROTOCOL_VERSION=0.14.5
ARG SPICE_GTK_VERSION=0.43
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils bzip2 python3-pip ninja-build pkg-config gettext perl \
        python3-pyparsing python3-six libglib2.0-dev libgtk-3-dev libpixman-1-dev libjpeg-dev libopus-dev \
        liblz4-dev libssl-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libepoxy-dev libdrm-dev \
        libwayland-dev wayland-protocols libjson-glib-dev libcap-ng-dev \
    && rm -rf /var/lib/apt/lists/* && pip3 install meson==0.63.3
RUN curl -fsSL "https://www.spice-space.org/download/releases/spice-protocol-${SPICE_PROTOCOL_VERSION}.tar.xz" | tar -xJ -C / \
    && cd "/spice-protocol-${SPICE_PROTOCOL_VERSION}" && meson setup build --prefix=/usr && ninja -C build install
COPY patches/ /patches/
RUN curl -fsSL "https://www.spice-space.org/download/gtk/spice-gtk-${SPICE_GTK_VERSION}.tar.xz" | tar -xJ -C / \
    && cd "/spice-gtk-${SPICE_GTK_VERSION}" \
    && for p in /patches/*.patch; do patch -p1 --forward < "$p" || exit 1; done \
    && meson setup build --buildtype=release --prefix=/opt/viewer --libdir=lib \
        -Dusbredir=disabled -Dsmartcard=disabled -Dpolkit=disabled -Dwebdav=disabled -Dvapi=disabled \
        -Dintrospection=disabled -Dgtk_doc=disabled -Dsasl=disabled \
    && ninja -C build install
COPY winboat-spice-viewer.c /src/
RUN mkdir -p /out/bin /out/lib \
    && gcc -O2 -Wall -o /out/bin/winboat-spice-viewer /src/winboat-spice-viewer.c \
        $(PKG_CONFIG_PATH=/opt/viewer/lib/pkgconfig pkg-config --cflags --libs spice-client-gtk-3.0 x11) \
        -Wl,-rpath,'$ORIGIN/../lib' \
    && cp -P /opt/viewer/lib/libspice-client-*.so* /out/lib/
