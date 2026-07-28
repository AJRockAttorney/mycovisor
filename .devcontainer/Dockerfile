FROM debian:bookworm-slim

# Arm's own release of the toolchain: gcc, gdb, objcopy, size, etc. all
# built from the same release so versions can't drift apart, and gdb
# ships under its real name (arm-none-eabi-gdb), matching the justfile.
# Bump ARM_TOOLCHAIN_VERSION to pick up a newer release.
ARG ARM_TOOLCHAIN_VERSION=13.3.rel1
ARG ARM_TOOLCHAIN_ARCHIVE=arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-x86_64-arm-none-eabi

RUN apt-get update && apt-get install -y --no-install-recommends \
    curl \
    ca-certificates \
    xz-utils \
    build-essential \
    cmake \
    ninja-build \
    git \
    clangd \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL \
    "https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_TOOLCHAIN_VERSION}/binrel/${ARM_TOOLCHAIN_ARCHIVE}.tar.xz" \
    -o /tmp/arm-toolchain.tar.xz \
    && mkdir -p /opt/arm-gnu-toolchain \
    && tar -xf /tmp/arm-toolchain.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1 \
    && rm /tmp/arm-toolchain.tar.xz

ENV PATH="/opt/arm-gnu-toolchain/bin:${PATH}"

# just: not packaged in bookworm, install the prebuilt binary
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh \
    | bash -s -- --to /usr/local/bin

WORKDIR /workspace
