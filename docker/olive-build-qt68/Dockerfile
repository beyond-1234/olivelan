# Olive Linux build environment: Ubuntu 22.04 + Qt 6.8 LTS
#
# Build:
#   docker build -t olive-build-qt68:6.8 \
#     --build-arg QT_VERSION=6.8.3 \
#     -f docker/olive-build-qt68/Dockerfile docker/olive-build-qt68
#
# Then create/recreate the distrobox container:
#   RECREATE=1 bash scripts/create_olive_build_qt68.sh
#
# Qt is installed from the official Qt binary archives via aqtinstall, not from
# the Ubuntu 22.04 package (which is Qt 6.4.2 and no longer supported: Qt 6.6
# moved QZipReader's private header to QtCore/private).

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG QT_VERSION=6.8.3
ARG AQT_VERSION=3.2.0
ARG QT_ARCH=linux_gcc_64
ARG QT_DIR_NAME=gcc_64
ARG OCIO_VERSION=2.5.1
ARG FFMPEG_VERSION=6.1.1

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    ccache \
    cmake \
    curl \
    git \
    ninja-build \
    pkg-config \
    python3 \
    python3-pip \
    sudo \
    zlib1g-dev \
    libssl-dev \
    libdbus-1-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libgl1-mesa-dev \
    libglu1-mesa-dev \
    libegl1-mesa-dev \
    libx11-dev \
    libx11-xcb1 \
    libxcb1 \
    libxcb-cursor0 \
    libxcb-glx0 \
    libxcb-icccm4 \
    libxcb-image0 \
    libxcb-keysyms1 \
    libxcb-randr0 \
    libxcb-render-util0 \
    libxcb-shape0 \
    libxcb-shm0 \
    libxcb-sync1 \
    libxcb-util1 \
    libxcb-xfixes0 \
    libxcb-xinerama0 \
    libxcb-xinput0 \
    libxcb-xkb1 \
    libxkbcommon-dev \
    libxkbcommon-x11-0 \
    libxshmfence1 \
    libsm6 \
    libice6 \
    libnss3 \
    libasound2 \
    libwayland-client0 \
    libwayland-cursor0 \
    libwayland-egl1 \
    libwayland-server0 \
    autoconf \
    automake \
    libtool \
    nasm \
    yasm \
    libbz2-dev \
    liblzma-dev \
    libx264-dev \
    libx265-dev \
    libvpx-dev \
    libaom-dev \
    libdav1d-dev \
    libmp3lame-dev \
    libopus-dev \
    libvorbis-dev \
    portaudio19-dev \
    libva-dev \
    libva-drm2 \
    libdrm-dev \
    libimath-dev \
    libexpat1-dev \
    libpystring-dev \
    libminizip-dev \
    libyaml-cpp-dev \
    file \
    squashfs-tools \
    patchelf \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir \
    aqtinstall==${AQT_VERSION}

# Qt 6.8 LTS binary distribution. The base archive already includes LinguistTools
# (lrelease/lupdate) plus Core/Gui/Widgets/OpenGL/OpenGLWidgets/Concurrent/DBus.
RUN aqt install-qt linux desktop "${QT_VERSION}" "${QT_ARCH}" \
    -O /opt/Qt

# Ubuntu 22.04 ships OpenColorIO 2.1.x, below the 2.2 build floor. Build the
# pinned 2.5.1 engine here instead of silently linking the old distro package.
RUN curl -fsSL "https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/refs/tags/v${OCIO_VERSION}.tar.gz" \
      -o /tmp/ocio.tar.gz \
    && mkdir -p /tmp/ocio-src /tmp/ocio-build \
    && tar -xzf /tmp/ocio.tar.gz -C /tmp/ocio-src --strip-components=1 \
    && cmake -S /tmp/ocio-src -B /tmp/ocio-build \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DOCIO_BUILD_APPS=OFF \
         -DOCIO_BUILD_PYTHON=OFF \
         -DOCIO_BUILD_TESTS=OFF \
         -DOCIO_BUILD_GPU_TESTS=OFF \
         -DOCIO_BUILD_DOCS=OFF \
         -DOCIO_INSTALL_EXT_PACKAGES=MISSING \
    && cmake --build /tmp/ocio-build -j"$(nproc)" \
    && cmake --install /tmp/ocio-build \
    && ldconfig \
    && rm -rf /tmp/ocio.tar.gz /tmp/ocio-src /tmp/ocio-build

# Ubuntu 22.04 ships FFmpeg 4.4. Olive requires FFmpeg >= 6.1, both at build time
# and for the ABI it links against, so pin and build 6.1.1 from source.
RUN curl -fsSL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
      -o /tmp/ffmpeg.tar.xz \
    && mkdir -p /tmp/ffmpeg-src \
    && tar -xJf /tmp/ffmpeg.tar.xz -C /tmp/ffmpeg-src --strip-components=1 \
    && cd /tmp/ffmpeg-src \
    && ./configure \
         --prefix=/usr/local \
         --enable-shared \
         --disable-static \
         --disable-doc \
         --disable-debug \
         --disable-autodetect \
         --enable-gpl \
         --enable-zlib \
         --enable-bzlib \
         --enable-lzma \
         --enable-libx264 \
         --enable-libx265 \
         --enable-libvpx \
         --enable-libdav1d \
         --enable-libopus \
         --enable-libvorbis \
         --enable-libmp3lame \
         --enable-vaapi \
         --enable-libdrm \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-src

# Distrobox bind-mounts /run/user/<uid>, so the container can use the host's
# PulseAudio/PipeWire server. Without an ALSA bridge plugin PortAudio only
# enumerates the HDMI hardware devices and silently pins the default output to
# HDMI 0. Keep the bridge in the image so `pulse` is always selectable.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libasound2-plugins \
    && rm -rf /var/lib/apt/lists/*

ENV QT_ROOT=/opt/Qt/${QT_VERSION}/${QT_DIR_NAME} \
    CMAKE_PREFIX_PATH=/opt/Qt/${QT_VERSION}/${QT_DIR_NAME};/usr/local \
    LD_LIBRARY_PATH=/usr/local/lib:/opt/Qt/${QT_VERSION}/${QT_DIR_NAME}/lib \
    PATH=/opt/Qt/${QT_VERSION}/${QT_DIR_NAME}/bin:${PATH}

RUN test -x "${QT_ROOT}/bin/qmake" \
    && "${QT_ROOT}/bin/qmake" -query QT_VERSION \
    && test -f /usr/local/include/OpenColorIO/OpenColorABI.h \
    && test -f /usr/local/lib/cmake/OpenColorIO/OpenColorIOConfig.cmake \
    && cmake --version \
    && gcc --version | head -1

CMD ["/bin/bash"]
