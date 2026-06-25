#!/usr/bin/env bash
# Build the JetPack 7.2 (L4T r39.2) target sysroot for cross-compilation.
#
# The NVIDIA jetpack-linux-aarch64-crosscompile-x86 container has no 7.x tag,
# so — unlike the 6.2 flow, which unpacked a targetfs baked into the base image —
# this fetches NVIDIA's public L4T r39.2 BSP + sample root filesystem and
# assembles the tegra-correct sysroot: Ubuntu 24.04 arm64 userspace plus the
# Jetson multimedia/Tegra libraries layered on by the BSP's apply_binaries.
#
# Usage: 001_fetch_bsp_rootfs.sh <l4t_dir>
#   Env:
#     L4T_LOCAL_CACHE  dir holding pre-downloaded tarballs (skips the curl)
set -euo pipefail

L4T="${1:?Usage: 001_fetch_bsp_rootfs.sh <l4t_dir>}"
REL="https://developer.download.nvidia.com/embedded/L4T/r39_Release_v2.0/release"
BSP_TBZ="Jetson_Linux_R39.2.0_aarch64.tbz2"
RFS_TBZ="Tegra_Linux_Sample-Root-Filesystem_R39.2.0_aarch64.tbz2"
CACHE="${L4T_LOCAL_CACHE:-}"

mkdir -p "$L4T"
cd "$L4T"

fetch() {  # <filename>
    local f="$1"
    if [[ -n "$CACHE" && -f "$CACHE/$f" ]]; then
        echo "[001] Using cached $f from $CACHE"
        cp "$CACHE/$f" "./$f"
    else
        echo "[001] Downloading $f ..."
        curl -fSL -o "./$f" "$REL/$f"
    fi
}

fetch "$BSP_TBZ"
fetch "$RFS_TBZ"

echo "[001] Unpacking BSP (Linux_for_Tegra/) ..."
tar -I lbzip2 -xf "$BSP_TBZ"
rm -f "$BSP_TBZ"

echo "[001] Unpacking sample rootfs into Linux_for_Tegra/rootfs ..."
tar -I lbzip2 -xf "$RFS_TBZ" -C Linux_for_Tegra/rootfs
rm -f "$RFS_TBZ"

# Layer the NVIDIA Tegra userspace (multimedia, NVMM, GStreamer plugins, the
# Jetson-built OpenCV) onto the sample rootfs. apply_binaries.sh is the NVIDIA-
# blessed step; with the sample rootfs extracted into ./rootfs (its default
# target) it needs no -r arg. It chroots aarch64 helpers, so qemu-user-static
# must be registered (it is, via the Dockerfile's qemu-user-static install).
# apply_binaries chroots into the arm64 rootfs to run dpkg/post-install scripts,
# which requires qemu-aarch64 registered in the HOST's binfmt_misc. Verify now so
# a missing registration surfaces as an actionable message instead of the cryptic
# "chroot: failed to run command 'dpkg': Exec format error" (exit 126) mid-apply.
echo "[001] Checking host can execute aarch64 binaries (binfmt) ..."
if ! chroot Linux_for_Tegra/rootfs /bin/true 2>/dev/null; then
    echo "ERROR: cannot execute aarch64 binaries inside the rootfs chroot." >&2
    echo "  The build host is missing qemu-aarch64 binfmt_misc registration." >&2
    echo "  Docker Desktop registers it automatically; CI uses docker/setup-qemu-action." >&2
    echo "  On a plain Linux host, register it once with:" >&2
    echo "    docker run --privileged --rm tonistiigi/binfmt --install arm64" >&2
    exit 1
fi

echo "[001] Applying NVIDIA Tegra binaries to the rootfs ..."
( cd Linux_for_Tegra && ./apply_binaries.sh )

# Expose the finished rootfs at the path the toolchain/Dockerfile expect.
ln -sfn "$L4T/Linux_for_Tegra/rootfs" "$L4T/targetfs"

echo "[001] Done. targetfs → $(readlink -f "$L4T/targetfs")"
