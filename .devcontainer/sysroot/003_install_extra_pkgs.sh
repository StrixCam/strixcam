#!/usr/bin/env bash
# Install Ubuntu jammy aarch64 packages into the JetPack sysroot that are not
# part of the base L4T tarball:
#   - BlueZ peripheral support via sdbus-c++ (BLE control plane)
#   - gst-rtsp-server (companion-app RTSP stream)
#   - gstreamer1.0-plugins-bad (rtmpsink for platform RTMP/RTMPS push)
#   - libprotobuf (runtime + dev) for the sst-cam-proto BLE/WiFi contract.
#     Host protoc is installed separately in the Dockerfile; the target
#     libprotobuf MUST be the SAME upstream protobuf version (jammy ships
#     3.12.4 for both) so generated .pb.cc ABI matches the runtime.
#
# Runs BEFORE 002_fix_sysroot.sh so the symlink-fix and .so linker-stub passes
# pick up everything that was just extracted.
#
# Usage: 003_install_extra_pkgs.sh <sysroot_path>
set -euo pipefail

SYSROOT="${1:?Usage: 003_install_extra_pkgs.sh <sysroot_path>}"

PORTS="http://ports.ubuntu.com/ubuntu-ports"

# Pinned to Ubuntu 22.04 (jammy) arm64 — matches JetPack 6.x base.
PKGS=(
  # libsystemd-dev: pulls in the .pc file required by sdbus-c++.pc
  "pool/main/s/systemd/libsystemd-dev_249.11-0ubuntu3_arm64.deb"
  # sdbus-c++: BLE peripheral via BlueZ over D-Bus
  "pool/universe/s/sdbus-cpp/libsdbus-c++1_1.1.0-3_arm64.deb"
  "pool/universe/s/sdbus-cpp/libsdbus-c++-dev_1.1.0-3_arm64.deb"
  # gst-rtsp-server: companion-app RTSP server library
  "pool/universe/g/gst-rtsp-server1.0/libgstrtspserver-1.0-0_1.20.1-1_arm64.deb"
  "pool/universe/g/gst-rtsp-server1.0/libgstrtspserver-1.0-dev_1.20.1-1_arm64.deb"
  # gst-plugins-bad: ships rtmp2sink (RTMP/RTMPS push to YouTube/Twitch/etc) and
  # voaacenc (AAC for the silent audio track platforms require). flvmux/h264parse/
  # mp4mux/qtdemux/concat/splitmuxsink are in plugins-good (base sysroot).
  "pool/universe/g/gst-plugins-bad1.0/gstreamer1.0-plugins-bad_1.20.3-0ubuntu1.1_arm64.deb"
  "pool/universe/g/gst-plugins-bad1.0/libgstreamer-plugins-bad1.0-0_1.20.3-0ubuntu1.1_arm64.deb"
  # gst-plugins-ugly + libx264: x264enc software H.264 encoder. The Orin Nano has
  # no NVENC (nvv4l2h264enc is absent), so recording/preview/RTMP all encode on
  # CPU via x264enc, which lives in plugins-ugly (NOT plugins-bad) and links
  # libx264. Without this the pipelines build green (gst_parse_launch resolves
  # elements at runtime) but fail to reach PLAYING on-device.
  "pool/universe/g/gst-plugins-ugly1.0/gstreamer1.0-plugins-ugly_1.20.1-1_arm64.deb"
  "pool/universe/x/x264/libx264-163_0.163.3060+git5db6aa6-2build1_arm64.deb"
  # protobuf 3.12.4 runtime + dev headers for the sst-cam-proto contract.
  # Pinned to jammy 3.12.4 to match the host protoc installed in the Dockerfile
  # (generated-code ABI is keyed on the upstream protobuf version, 3012004).
  "pool/main/p/protobuf/libprotobuf23_3.12.4-1ubuntu7_arm64.deb"
  "pool/main/p/protobuf/libprotobuf-lite23_3.12.4-1ubuntu7_arm64.deb"
  "pool/main/p/protobuf/libprotobuf-dev_3.12.4-1ubuntu7_arm64.deb"
  # Cairo + Pango -dev: headers + .pc for the overlay rasterizer (U8). The
  # runtime .so already ship in the L4T base; these add the dev bits plus the
  # transitive -dev packages their headers/.pc reference (harfbuzz, freetype,
  # fontconfig, fribidi, png, pixman, brotli, graphite2, thai, datrie). The
  # firmware links the libs directly (CMakeLists) rather than via pango's full
  # X11 pkg-config chain.
  "pool/main/c/cairo/libcairo2-dev_1.16.0-5ubuntu2.1_arm64.deb"
  "pool/main/p/pango1.0/libpango1.0-dev_1.50.6+ds-2ubuntu1_arm64.deb"
  "pool/main/h/harfbuzz/libharfbuzz-dev_2.7.4-1ubuntu3.2_arm64.deb"
  "pool/main/f/freetype/libfreetype-dev_2.11.1+dfsg-1ubuntu0.3_arm64.deb"
  "pool/main/f/fontconfig/libfontconfig-dev_2.13.1-4.2ubuntu5_arm64.deb"
  "pool/main/f/fribidi/libfribidi-dev_1.0.8-2ubuntu3.1_arm64.deb"
  "pool/main/libp/libpng1.6/libpng-dev_1.6.37-3ubuntu0.5_arm64.deb"
  "pool/main/p/pixman/libpixman-1-dev_0.40.0-1ubuntu0.22.04.1_arm64.deb"
  "pool/main/b/brotli/libbrotli-dev_1.0.9-2build6_arm64.deb"
  "pool/main/g/graphite2/libgraphite2-dev_1.3.14-1build2_arm64.deb"
  "pool/main/libt/libthai/libthai-dev_0.1.29-1build1_arm64.deb"
  "pool/main/libd/libdatrie/libdatrie-dev_0.2.13-2_arm64.deb"
)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
for path in "${PKGS[@]}"; do
  url="${PORTS}/${path}"
  echo "[003] downloading $url"
  wget -q "$url"
done

for deb in *.deb; do
  echo "[003] extracting $deb -> $SYSROOT"
  dpkg-deb -x "$deb" "$SYSROOT"
done

echo "[003] Done."
