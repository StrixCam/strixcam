#!/usr/bin/env bash
# Install Ubuntu 24.04 (noble) aarch64 packages into the JetPack 7.2 sysroot
# that the L4T r39.2 BSP sample rootfs does not already provide as -dev:
#   - BlueZ peripheral support via sdbus-c++ 1.4.0 (BLE control plane)
#   - gst-rtsp-server 1.24.2 (companion-app RTSP stream)
#   - libprotobuf-dev 3.21.12 headers (runtime .so.32 already in the rootfs;
#     MUST match the host protoc installed in the Dockerfile so generated
#     .pb.cc ABI matches the runtime — noble ships 3.21.12 for both)
#   - OpenCV 4.6 -dev headers (noble stock; runtime .so.406 already present —
#     we deliberately stay on noble 4.6, NOT NVIDIA's 4.8, to match the base
#     sample-rootfs runtime every JP7.2 device ships with; CMakeLists pins .406)
#   - Cairo/Pango/HarfBuzz/etc -dev for the overlay rasterizer
#
# The package URL list lives in noble-deb-urls.txt next to this script. It was
# resolved by apt against the actual BSP rootfs (apt computes the exact dep-
# closed delta + version pins), so it is regenerable rather than hand-curated.
# To regenerate: import the assembled rootfs as an arm64 image and run
#   apt-get install --no-install-recommends --print-uris <pkgs>
# with the NVIDIA jetson repo pinned low (so OpenCV stays noble 4.6).
#
# Runs BEFORE 002_fix_sysroot.sh so the symlink-fix and .so linker-stub passes
# pick up everything that was just extracted.
#
# Usage: 003_install_extra_pkgs.sh <sysroot_path>
set -euo pipefail

SYSROOT="${1:?Usage: 003_install_extra_pkgs.sh <sysroot_path>}"
URLS_FILE="$(dirname "$(readlink -f "$0")")/noble-deb-urls.txt"

if [[ ! -f "$URLS_FILE" ]]; then
  echo "ERROR: package URL list not found: $URLS_FILE" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

while IFS= read -r url; do
  [[ -z "$url" || "$url" == \#* ]] && continue
  echo "[003] downloading $url"
  wget -q "$url"
done < "$URLS_FILE"

for deb in *.deb; do
  echo "[003] extracting $deb -> $SYSROOT"
  dpkg-deb -x "$deb" "$SYSROOT"
done

echo "[003] Done ($(ls -1 ./*.deb | wc -l) packages)."
