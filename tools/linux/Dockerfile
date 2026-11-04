# tools/linux/Dockerfile - build environment for the Linux and Steam Deck packages.
#
# Ubuntu 22.04 (glibc 2.35), so the binary runs on SteamOS 3 and on current desktop
# distributions. SDL2 is built from source as a static library that loads X11, Wayland,
# PipeWire, PulseAudio, ALSA and udev at run time, so the program needs no SDL installed.
#
#   docker build -t vssmb-linux tools/linux
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/src -w /src vssmb-linux sh tools/linux/build.sh
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config python3 ca-certificates curl cmake ninja-build binutils \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxinerama-dev \
        libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*
ARG SDL_VERSION=2.32.10
RUN curl -fsSL -o /tmp/sdl.tar.gz \
        https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz \
    && tar -xzf /tmp/sdl.tar.gz -C /tmp \
    && cmake -S /tmp/SDL2-${SDL_VERSION} -B /tmp/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/sdl2 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
    && cmake --build /tmp/sdl-build \
    && cmake --install /tmp/sdl-build \
    && rm -rf /tmp/sdl.tar.gz /tmp/SDL2-${SDL_VERSION} /tmp/sdl-build
ENV PATH=/opt/sdl2/bin:$PATH
