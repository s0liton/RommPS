# Build image for the PS5 payload: the ps5-payload-dev SDK plus the pacbrew
# static libraries (curl, OpenSSL, zlib). Runs on arm64 and amd64.
FROM ubuntu:24.04

ARG SDK_VERSION=v0.43
ARG PACBREW_VERSION=v0.40.3

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash ca-certificates wget unzip xz-utils make pkg-config python3 git \
        clang-18 lld-18 llvm-18 \
    && rm -rf /var/lib/apt/lists/*

# SDK
RUN wget -q -O /tmp/sdk.zip \
        https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /opt \
    && rm /tmp/sdk.zip \
    && chmod +x /opt/ps5-payload-sdk/bin/*

# The bundle carries its own SDK copy; only take the libraries and the
# *-config scripts so the SDK stays at ${SDK_VERSION}.
RUN wget -q -O /tmp/pb.tar.gz \
        https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/${PACBREW_VERSION}/ps5-payload-dev.tar.gz \
    && tar -xzf /tmp/pb.tar.gz -C / opt/ps5-payload-sdk/target/user \
    && tar -xzf /tmp/pb.tar.gz -C /tmp opt/ps5-payload-sdk/bin \
    && for f in /tmp/opt/ps5-payload-sdk/bin/prospero-*-config; do \
         [ -e "/opt/ps5-payload-sdk/bin/$(basename "$f")" ] || cp -a "$f" /opt/ps5-payload-sdk/bin/; \
       done \
    && rm -rf /tmp/pb.tar.gz /tmp/opt \
    && cp /opt/ps5-payload-sdk/target/user/homebrew/etc/ca-bundle.crt /opt/ca-bundle.crt

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
ENV PATH=/opt/ps5-payload-sdk/bin:${PATH}
# prospero-llvm-config honours $LLVM_CONFIG first
ENV LLVM_CONFIG=/usr/bin/llvm-config-18

# Fail early if static libcurl does not resolve.
RUN prospero-pkg-config --static --libs libcurl \
    && prospero-pkg-config --cflags libcurl

WORKDIR /src
