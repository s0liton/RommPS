# Build image for the PS4: the ps4-payload-dev SDK plus its pacbrew static
# libraries (curl, OpenSSL, zlib) for the payload, and the OpenOrbis toolchain
# for the launcher package (platform/ps4/app). amd64 only, as OpenOrbis's
# packaging tools are x86-64 binaries; Docker emulates it on arm64.
FROM --platform=linux/amd64 ubuntu:24.04

ARG SDK_VERSION=v0.9
ARG PACBREW_VERSION=v0.3.3
ARG OPENORBIS_VERSION=v0.5.4

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash ca-certificates wget unzip xz-utils make pkg-config python3 git \
        clang-18 lld-18 llvm-18 \
    && rm -rf /var/lib/apt/lists/*

# SDK
RUN wget -q -O /tmp/sdk.zip \
        https://github.com/ps4-payload-dev/sdk/releases/download/${SDK_VERSION}/ps4-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /opt \
    && rm /tmp/sdk.zip \
    && chmod +x /opt/ps4-payload-sdk/bin/*

# Libraries land in the SDK's target/user/homebrew, next to the SDK itself.
RUN wget -q -O /tmp/pb.tar.gz \
        https://github.com/ps4-payload-dev/pacbrew/releases/download/${PACBREW_VERSION}/ps4-payload-dev.tar.gz \
    && tar -xzf /tmp/pb.tar.gz -C / opt/ps4-payload-sdk/target/user \
    && rm /tmp/pb.tar.gz \
    && cp /opt/ps4-payload-sdk/target/user/homebrew/etc/ca-bundle.crt /opt/ca-bundle.crt

# OpenOrbis, for the launcher package. PkgTool.Core is .NET Core 3.0: it wants
# ICU unless told not to, and OpenSSL 1.1, which only Ubuntu 20.04 still has.
RUN wget -q -O /tmp/libssl.deb \
        http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb \
    && dpkg -i /tmp/libssl.deb \
    && rm /tmp/libssl.deb
RUN wget -q -O /tmp/oo.tar.gz \
        https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/${OPENORBIS_VERSION}/toolchain-llvm-18.tar.gz \
    && mkdir -p /opt/openorbis \
    && tar -xzf /tmp/oo.tar.gz -C /opt/openorbis --strip-components=2 \
    && rm /tmp/oo.tar.gz \
    && chmod +x /opt/openorbis/bin/linux/*
ENV OO_PS4_TOOLCHAIN=/opt/openorbis
ENV DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# Every package carries sce_module/libc.prx, sce_module/libSceFios2.prx and
# sce_sys/about/right.sprx. OpenOrbis builds them from its source tree rather
# than shipping them, so build them here into /opt/openorbis/modules.
RUN ln -sf /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-18 /usr/local/bin/ld.lld \
    && git clone -q --depth 1 --branch ${OPENORBIS_VERSION} --filter=blob:none --sparse \
        https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain /tmp/oo-src \
    && git -C /tmp/oo-src sparse-checkout set src/modules \
    && mkdir -p /opt/openorbis/modules \
    && for m in libc libSceFios2 right; do \
         make -C /tmp/oo-src/src/modules/$m >/dev/null && \
         cp /tmp/oo-src/src/modules/$m/$m.*prx /opt/openorbis/modules/; \
       done \
    && rm -rf /tmp/oo-src \
    && ls -l /opt/openorbis/modules

ENV PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
ENV PATH=/opt/ps4-payload-sdk/bin:${PATH}
ENV LLVM_CONFIG=/usr/bin/llvm-config-18

# Fail early if static libcurl does not resolve.
RUN orbis-pkg-config --static --libs libcurl \
    && orbis-pkg-config --cflags libcurl

WORKDIR /src
