# shellcheck shell=bash
# Shared clang-tidy cross-compilation arg-builder — the single source of truth
# for the CI `tidy` job (.github/workflows/ci.yml) and the dev-side fix script
# (scripts/fix.sh). Source this file, then pass "${TIDY_EXTRA_ARGS[@]}" to
# clang-tidy-14 so CI and the fix script can never drift on flags.
#
# Why these flags: host x86 clang-tidy must parse aarch64-buildroot cross TUs.
# clang auto-detects the Bootlin gcc layout (lib/gcc/<triple>/<ver> plus
# <triple>/include/c++/<ver>, both present in the toolchain tree) when pointed
# at the toolchain ROOT with a MATCHED target triple — and crucially keeps
# clang's own builtin/resource dir in the search path, so gcc's intrinsic
# headers (arm_neon.h) and a conflicting <stdint.h> don't collide with clang's.
#
#   --target=aarch64-buildroot-linux-gnu  the toolchain's REAL triple. Using
#       aarch64-linux-gnu defeats gcc auto-detection and libstdc++ goes missing
#       (the original "file not found" / exit-123 failure).
#   --gcc-toolchain=<root>                lets clang find lib/gcc/<triple>/<ver>.
#   --sysroot=<targetfs>                  targetfs supplies GStreamer/OpenCV and
#       system headers; libc/libstdc++ come from the toolchain tree above.
#   -Qunused-arguments                    silence link-only flags from the TU's
#       compile command that clang-tidy doesn't consume.

# Toolchain bin dir is exported by .devcontainer/Dockerfile. Default to the REAL
# path — note there is no "/toolchain/" segment (an earlier CI default had it
# wrong, a latent bug if the env var ever stopped being set).
_tidy_tc_bin="${BOOTLIN_TOOLCHAIN_BIN:-/l4t/aarch64--glibc--stable-2022.08-1/bin}"
_tidy_tc_root="$(dirname "${_tidy_tc_bin}")"
_tidy_sysroot="${CROSS_SYSROOT:-/l4t/targetfs}"

# shellcheck disable=SC2034  # consumed by the sourcing script
TIDY_EXTRA_ARGS=(
  --extra-arg=--target=aarch64-buildroot-linux-gnu
  --extra-arg=--gcc-toolchain="${_tidy_tc_root}"
  --extra-arg=--sysroot="${_tidy_sysroot}"
  --extra-arg=-Qunused-arguments
)
