# Build image for the Steam Deck app and service: Debian bookworm, so what it
# builds needs glibc 2.36 at most (SteamOS 3.9 has 2.43), with SDL3 built from
# a pinned release as a static library. SDL opens X11, Wayland, PipeWire and
# the rest at run time, so the app links none of them.
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config python3 python3-numpy python3-pil ca-certificates curl \
        clang-format-16 clang-tidy-16 cppcheck shellcheck file desktop-file-utils \
        libcurl4-openssl-dev libssl-dev zlib1g-dev \
        mesa-common-dev libgl-dev libegl-dev libopengl-dev libgles-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
        libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev \
    && rm -rf /var/lib/apt/lists/*

# Its licence texts are in platform/steamdeck/licenses/SDL3: a new tag wants them copied again.
ARG SDL_TAG=release-3.2.10
RUN git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git /tmp/SDL \
    && cmake -S /tmp/SDL -B /tmp/SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/sdl3 \
    && cmake --build /tmp/SDL/build \
    && cmake --install /tmp/SDL/build \
    && rm -rf /tmp/SDL
ENV CMAKE_PREFIX_PATH=/opt/sdl3

# appimagetool and the AppImage runtime, pinned and unpacked, so packaging
# runs without FUSE and downloads nothing.
ARG APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
ARG RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUN curl -fsSL -o /tmp/appimagetool "$APPIMAGETOOL_URL" \
    && chmod +x /tmp/appimagetool \
    && cd /opt && /tmp/appimagetool --appimage-extract >/dev/null \
    && mv /opt/squashfs-root /opt/appimagetool \
    && ln -s /opt/appimagetool/AppRun /usr/local/bin/appimagetool \
    && rm /tmp/appimagetool \
    && curl -fsSL -o /opt/appimage-runtime-x86_64 "$RUNTIME_URL"
