# shellcheck shell=bash
# Shared clang-tidy cross-compilation arg-builder — the single source of truth
# for the CI `tidy` job (.github/workflows/release-*.yml) and the dev-side fix
# script (scripts/fix.sh). Source this file, then pass "${TIDY_EXTRA_ARGS[@]}"
# to clang-tidy so CI and the fix script can never drift on flags.
#
# JetPack 7.2: the cross toolchain is Ubuntu's gcc-13 aarch64 package, which
# installs the gcc tree under /usr/lib/gcc-cross/<triple>/<ver> with libstdc++
# headers under /usr/<triple>/include/c++/<ver>. Host clang-tidy (noble v18)
# must be pointed at that gcc install with a MATCHED triple so it finds
# libstdc++; the targetfs sysroot supplies GStreamer/OpenCV/system headers.
#
#   --target=aarch64-linux-gnu       the Ubuntu cross triple (no Bootlin buildroot).
#   --gcc-install-dir=<dir>          the exact gcc-cross version dir, so clang-18
#       resolves libstdc++ under the split Ubuntu cross layout (a bare
#       --gcc-toolchain does not reliably find /usr/lib/gcc-cross/...).
#   --sysroot=<targetfs>             GStreamer/OpenCV + system headers.
#   -Qunused-arguments               silence link-only flags clang-tidy ignores.

_tidy_triple="${CROSS_TRIPLE:-aarch64-linux-gnu}"
_tidy_gccsuf="${CROSS_GCC_SUFFIX:--13}"
_tidy_gccver="${_tidy_gccsuf#-}"
_tidy_gcc_install_dir="/usr/lib/gcc-cross/${_tidy_triple}/${_tidy_gccver}"
_tidy_sysroot="${CROSS_SYSROOT:-/l4t/targetfs}"

# shellcheck disable=SC2034  # consumed by the sourcing script
TIDY_EXTRA_ARGS=(
  --extra-arg=--target="${_tidy_triple}"
  --extra-arg=--gcc-install-dir="${_tidy_gcc_install_dir}"
  --extra-arg=--sysroot="${_tidy_sysroot}"
  --extra-arg=-Qunused-arguments
)
