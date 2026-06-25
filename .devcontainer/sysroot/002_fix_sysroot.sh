#!/usr/bin/env bash
# Apply pkg-config and linker-compat fixes to a JetPack aarch64 sysroot.
# Usage: fix-sysroot.sh <sysroot_path>
set -euo pipefail

SYSROOT="${1:?Usage: fix-sysroot.sh <sysroot_path>}"
MULTIARCH="$SYSROOT/usr/lib/aarch64-linux-gnu"
USRLIB="$SYSROOT/usr/lib"

# ── OpenCV pkg-config ─────────────────────────────────────────────────────────

PC_OUT="$MULTIARCH/pkgconfig/opencv4.pc"

if [[ -f "$SYSROOT/usr/include/opencv4/opencv2/core.hpp" ]]; then
  INC_DIR="/usr/include/opencv4"
elif [[ -f "$SYSROOT/usr/local/include/opencv4/opencv2/core.hpp" ]]; then
  INC_DIR="/usr/local/include/opencv4"
else
  echo "WARN: OpenCV headers not found; defaulting includedir to /usr/include/opencv4" >&2
  INC_DIR="/usr/include/opencv4"
fi

LIBS=""
for mod in highgui videoio stitching video calib3d objdetect dnn features2d \
           imgcodecs photo ml flann imgproc core aruco bgsegm bioinspired \
           ccalib datasets dnn_objdetect dnn_superres dpm face freetype fuzzy \
           hdf hfs img_hash intensity_transform line_descriptor mcc optflow \
           phase_unwrapping plot quality rapid reg rgbd saliency shape stereo \
           structured_light surface_matching text tracking viz wechat_qrcode \
           xobjdetect xphoto ximgproc; do
  ls "$MULTIARCH/libopencv_${mod}.so"* >/dev/null 2>&1 && LIBS+=" -lopencv_${mod}"
done
[[ -n "$LIBS" ]] || echo "WARN: no libopencv_*.so files found in $MULTIARCH" >&2

VER="$(ls "$MULTIARCH/libopencv_core.so."* 2>/dev/null | head -n1 \
      | sed 's/.*\.so\.//' | sed 's/[a-zA-Z]*$//' || echo "unknown")"

mkdir -p "$(dirname "$PC_OUT")"
cat > "$PC_OUT" <<EOF
prefix=/usr
exec_prefix=\${prefix}
libdir=/usr/lib/aarch64-linux-gnu
includedir=${INC_DIR}

Name: OpenCV
Description: Open Source Computer Vision Library (NVIDIA JetPack)
Version: ${VER}
Libs: -L\${libdir}${LIBS}
Libs.private: -ldl -lm -lpthread -lrt
Cflags: -I\${includedir}
EOF
echo "[fix-sysroot] opencv4.pc written (ver=${VER})"

# ── Bootlin triplet compat ────────────────────────────────────────────────────
# Bootlin uses aarch64-buildroot-linux-gnu-* but the sysroot uses aarch64-linux-gnu paths.

mkdir -p "$USRLIB"
ln -sfn aarch64-linux-gnu "$USRLIB/aarch64-buildroot-linux-gnu"
[[ -d "$SYSROOT/lib/aarch64-linux-gnu" || -L "$SYSROOT/lib/aarch64-linux-gnu" ]] && \
  ln -sfn aarch64-linux-gnu "$SYSROOT/lib/aarch64-buildroot-linux-gnu"

# crt*.o linker stubs so the cross-linker finds startup files via /usr/lib
for f in crt1.o crti.o crtn.o Scrt1.o gcrt1.o grcrt1.o rcrt1.o; do
  [[ -e "$MULTIARCH/$f" ]] && ln -sfn "aarch64-linux-gnu/$f" "$USRLIB/$f" || true
done
echo "[fix-sysroot] crt stubs linked"

# ── Fix Debian alternatives (absolute symlinks → relative through sysroot) ────
# The cross-linker follows absolute symlink targets on the HOST filesystem, not
# inside the sysroot. Walk each chain through the sysroot and replace with a
# direct relative symlink.
for dir in "$MULTIARCH" "$SYSROOT/lib/aarch64-linux-gnu"; do
  [[ -d "$dir" ]] || continue
  shopt -s nullglob
  for link in "$dir"/*.so "$dir"/*.so.*; do
    [[ -L "$link" ]] || continue
    target=$(readlink "$link")
    [[ "$target" == /* ]] || continue  # already relative — skip
    current="$SYSROOT$target"
    for _ in 1 2 3 4 5 6 7 8; do
      [[ -L "$current" ]] || break
      next=$(readlink "$current")
      if [[ "$next" == /* ]]; then
        current="$SYSROOT$next"
      else
        current="$(dirname "$current")/$next"
        break
      fi
    done
    if [[ ! -e "$current" ]]; then
      echo "WARN: broken symlink chain: $link (stopped at $current)" >&2
      continue
    fi
    ln -sf "$(realpath --relative-to="$dir" "$current")" "$link"
  done
  shopt -u nullglob
done
echo "[fix-sysroot] absolute symlinks fixed"

# ── .so linker stubs from versioned .so.* ────────────────────────────────────
shopt -s nullglob
for sover in "$MULTIARCH"/*.so.*; do
  base=$(basename "$sover")
  stub="$MULTIARCH/${base%%.so.*}.so"
  [[ -e "$stub" ]] || ln -sf "$base" "$stub"
done
shopt -u nullglob
echo "[fix-sysroot] .so linker stubs created"

# ── Mirror libX.so from multiarch into /usr/lib ───────────────────────────────
shopt -s nullglob
for f in "$USRLIB"/lib*.so; do [[ -L "$f" ]] && rm -f "$f"; done
for f in "$MULTIARCH"/*.so; do
  ln -sf "aarch64-linux-gnu/$(basename "$f")" "$USRLIB/$(basename "$f")"
done
shopt -u nullglob
echo "[fix-sysroot] /usr/lib mirrors created"

echo "[fix-sysroot] Done."
