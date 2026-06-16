FROM nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1

# ── Install build tools (as root) ───────────────────────────────────────────────
# Add Kitware repo to get CMake >= 3.24 (Ubuntu 22.04 ships 3.22).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        wget \
        curl \
        git \
        sudo \
        gpg \
        lbzip2 \
        ninja-build \
        python3-pip \
        pipx \
        pkg-config \
        clangd \
        clang-format \
        clang-tidy-14 \
        protobuf-compiler \
    && wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc \
       | gpg --dearmor - > /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
       > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*

# Node.js 22.x via NodeSource — needed to run Claude Code. Pinned to setup_22.x
# rather than setup_lts.x to prevent silent Node major-version bumps on rebuild.
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

# ── 001: Unpack toolchain + sysroot tarballs from the NGC base image ─────────
COPY .devcontainer/sysroot/001_unpack_l4t.sh /tmp/001_unpack_l4t.sh
RUN bash /tmp/001_unpack_l4t.sh /l4t && rm /tmp/001_unpack_l4t.sh

# ── 003: Install Ubuntu jammy arm64 packages missing from L4T (sdbus-c++,
#         gst-rtsp-server). Runs BEFORE fix-sysroot so the new .so files are
#         picked up by the symlink-fix pass. wget + dpkg-deb are already
#         installed in step 1 above.
COPY .devcontainer/sysroot/003_install_extra_pkgs.sh /tmp/003_install_extra_pkgs.sh
RUN bash /tmp/003_install_extra_pkgs.sh /l4t/targetfs \
    && rm /tmp/003_install_extra_pkgs.sh

# ── 002: Fix sysroot pkg-config, symlinks, crt stubs ─────────────────────────
COPY .devcontainer/sysroot/002_fix_sysroot.sh /tmp/002_fix_sysroot.sh
RUN bash /tmp/002_fix_sysroot.sh /l4t/targetfs && rm /tmp/002_fix_sysroot.sh

# ── Non-root user ───────────────────────────────────────────────────────────────
# UID/GID default to 1000 to line up with the host user, so the bind-mounted
# workspace and ~/.conan2 cache keep consistent ownership across host/container.
ARG USER_UID=1000
ARG USER_GID=$USER_UID
RUN groupadd --gid ${USER_GID} vscode \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m vscode \
    && echo 'vscode ALL=(root) NOPASSWD:ALL' > /etc/sudoers.d/vscode \
    && chmod 0440 /etc/sudoers.d/vscode

USER vscode

# Conan, installed under the user's home via pipx (was root + system pipx before).
ENV PATH="/home/vscode/.local/bin:${PATH}"
RUN pipx install conan

# Install Claude Code globally in a user-writable npm prefix so it can self-update
# without sudo. NPM_CONFIG_PREFIX directs all `npm install -g` to this directory.
# The .claude config dir is a named volume (see docker-compose.yml) so sessions
# survive container rebuilds.
ENV NPM_CONFIG_PREFIX=/home/vscode/.npm-global
ENV PATH="/home/vscode/.npm-global/bin:${PATH}"
RUN mkdir -p /home/vscode/.npm-global /home/vscode/.claude \
    && npm install -g @anthropic-ai/claude-code

# ── Export cross-compile paths as env vars ─────────────────────────────────────
ENV CROSS_SYSROOT=/l4t/targetfs
ENV BOOTLIN_TOOLCHAIN_BIN=/l4t/aarch64--glibc--stable-2022.08-1/bin

WORKDIR /workspace
