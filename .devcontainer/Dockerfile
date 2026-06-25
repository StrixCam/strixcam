# JetPack 7.2 (L4T r39.2) cross-compile dev container.
#
# NVIDIA publishes no 7.x jetpack-linux-aarch64-crosscompile-x86 container, so
# the base is plain ubuntu:24.04 (noble = the JetPack 7.2 userspace, glibc 2.39).
# The aarch64 sysroot is the L4T r39.2 BSP sample rootfs (fetched by 001), and
# the cross compiler is Ubuntu's gcc-13 aarch64 toolchain (matches noble glibc).
# Digest-pin once the build is proven, to harden against a mutable-tag overwrite.
FROM ubuntu:24.04

# ── Minimal tools for sysroot assembly ───────────────────────────────────────
# Kept tiny and FIRST so the expensive 001 BSP fetch+assemble layer below is not
# invalidated when the build-tools apt set (further down) changes.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        wget \
        curl \
        lbzip2 \
        qemu-user-static \
    && rm -rf /var/lib/apt/lists/*

# ── 001: Fetch L4T r39.2 BSP + sample rootfs and assemble the sysroot ────────
# L4T_LOCAL_CACHE lets a build mount pre-downloaded tarballs to skip the ~3 GB
# pull (set via build-arg/bind for local iteration; CI fetches fresh).
ARG L4T_LOCAL_CACHE=""
ENV L4T_LOCAL_CACHE=${L4T_LOCAL_CACHE}
COPY .devcontainer/sysroot/001_fetch_bsp_rootfs.sh /tmp/001_fetch_bsp_rootfs.sh
RUN bash /tmp/001_fetch_bsp_rootfs.sh /l4t && rm /tmp/001_fetch_bsp_rootfs.sh

# ── 003: Install Ubuntu noble arm64 -dev packages the sample rootfs lacks
#         (sdbus-c++, gst-rtsp-server, protobuf-dev, opencv, cairo/pango).
#         Runs BEFORE fix-sysroot so new .so files get the symlink-fix pass.
#         003 reads its sibling noble-deb-urls.txt, so copy both.
COPY .devcontainer/sysroot/003_install_extra_pkgs.sh .devcontainer/sysroot/noble-deb-urls.txt /tmp/sysroot/
RUN bash /tmp/sysroot/003_install_extra_pkgs.sh /l4t/targetfs

# ── 002: Fix sysroot pkg-config, symlinks, crt stubs ─────────────────────────
COPY .devcontainer/sysroot/002_fix_sysroot.sh /tmp/sysroot/002_fix_sysroot.sh
RUN bash /tmp/sysroot/002_fix_sysroot.sh /l4t/targetfs && rm -rf /tmp/sysroot

# ── Build tools (host + cross toolchain) ─────────────────────────────────────
# Placed AFTER sysroot assembly so churn here never re-triggers the 001 fetch.
# noble ships cmake 3.28 (>= 3.24); clang-tidy on noble is v18 (re-triaged, U7).
# build-essential gives host make + gcc/g++ that Conan needs to build deps.
RUN apt-get update && apt-get install -y --no-install-recommends \
        git \
        sudo \
        gpg \
        cmake \
        ninja-build \
        build-essential \
        python3-pip \
        pipx \
        pkg-config \
        clangd \
        clang-format \
        clang-tidy \
        protobuf-compiler \
        gcc-13-aarch64-linux-gnu \
        g++-13-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

# Node.js 22.x via NodeSource — needed to run Claude Code. Pinned to setup_22.x.
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

# ── Non-root user ───────────────────────────────────────────────────────────────
ARG USER_UID=1000
ARG USER_GID=$USER_UID
# ubuntu:24.04 ships a default 'ubuntu' user at uid/gid 1000 — remove it so the
# vscode user can take 1000 (lines up with the host user for bind-mount perms).
RUN (userdel -r ubuntu 2>/dev/null || true) \
    && (groupdel ubuntu 2>/dev/null || true) \
    && groupadd --gid ${USER_GID} vscode \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m vscode \
    && echo 'vscode ALL=(root) NOPASSWD:ALL' > /etc/sudoers.d/vscode \
    && chmod 0440 /etc/sudoers.d/vscode

USER vscode

# Conan, installed under the user's home via pipx.
ENV PATH="/home/vscode/.local/bin:${PATH}"
RUN pipx install conan

# Install Claude Code globally in a user-writable npm prefix.
ENV NPM_CONFIG_PREFIX=/home/vscode/.npm-global
ENV PATH="/home/vscode/.npm-global/bin:${PATH}"
RUN mkdir -p /home/vscode/.npm-global /home/vscode/.claude \
    && npm install -g @anthropic-ai/claude-code

# ── Export cross-compile paths as env vars ─────────────────────────────────────
# Single aarch64-linux-gnu triple via the Ubuntu gcc-13 cross package — no Bootlin.
ENV CROSS_SYSROOT=/l4t/targetfs
ENV CROSS_TRIPLE=aarch64-linux-gnu
ENV CROSS_GCC_SUFFIX=-13

WORKDIR /workspace
