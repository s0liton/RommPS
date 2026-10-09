# Build image for the native PS5 app (RommPS): the host tools mihawk-99's
# PS5_VulkanTemplate stack expects (ps5/tools/bootstrap.sh --check). Arch,
# like the stack's own build host; amd64 only, emulated on arm64.
FROM --platform=linux/amd64 archlinux:latest

# pacman's download sandbox needs a seccomp call Docker's amd64 emulation lacks.
RUN sed -i 's/^#\?DisableSandbox.*/DisableSandbox/; /^\[options\]/a DisableSandbox' /etc/pacman.conf \
    && pacman -Syu --noconfirm --needed \
        base-devel git cmake clang lld llvm python python-numpy python-pillow python-mako \
        python-packaging python-yaml python-pyelftools glslang spirv-tools rsync curl zstd ninja meson \
        bison flex unzip wget which \
        spirv-llvm-translator libclc spirv-headers libdrm \
    && pacman -Scc --noconfirm

# The stack and the app are mounted here (tools/ps5-app-build.sh).
WORKDIR /stack
