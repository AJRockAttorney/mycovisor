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

# arm-none-eabi-gdb is linked against libncursesw.so.5/libtinfo.so.5, which
# bookworm no longer ships (only the .so.6 sonames). gdb only relies on a
# narrow, stable slice of ncurses/tinfo for its CLI text UI, so symlinking
# the .6 libraries under the .5 names it expects is sufficient.
RUN ln -s /lib/x86_64-linux-gnu/libncursesw.so.6 /lib/x86_64-linux-gnu/libncursesw.so.5 \
    && ln -s /lib/x86_64-linux-gnu/libtinfo.so.6 /lib/x86_64-linux-gnu/libtinfo.so.5

# just: not packaged in bookworm, install the prebuilt binary
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh \
    | bash -s -- --to /usr/local/bin

WORKDIR /workspace
