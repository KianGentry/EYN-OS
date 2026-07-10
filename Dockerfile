FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    binutils \
    make \
    nasm \
    gdb \
    bash \
    python3 \
    coreutils \
    util-linux \
    grub-pc-bin \
    grub-common \
    xorriso \
    dosfstools \
    mtools \
    qemu-system-x86 \
    libgtk-3-0 \
    xwayland \
    pipewire \
    libsdl2-2.0-0 \
    ca-certificates \
    git \
    gcc-multilib \
    libc6-dev-i386 \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]