#!/usr/bin/env bash
# Runs every time the container starts, as the remote user.
# Sanity-check that the cross-compile toolchain and sysroot are in place; fail
# loudly if a rebuild dropped them rather than letting cmake fail cryptically.
set -euo pipefail

status=0
if [ ! -d "${CROSS_SYSROOT:-}" ]; then
    echo "[post-start] ERROR: sysroot missing at CROSS_SYSROOT=${CROSS_SYSROOT:-<unset>}" >&2
    status=1
fi
# JetPack 7.2: the cross compiler is the Ubuntu gcc-13 aarch64 package on PATH
# (e.g. aarch64-linux-gnu-gcc-13), not a Bootlin bin dir.
_cc="${CROSS_TRIPLE:-aarch64-linux-gnu}-gcc${CROSS_GCC_SUFFIX:--13}"
if ! command -v "${_cc}" >/dev/null 2>&1; then
    echo "[post-start] ERROR: cross compiler '${_cc}' not found on PATH" >&2
    status=1
fi
[ "${status}" -eq 0 ] && echo "[post-start] toolchain (${_cc}) + sysroot OK"
exit "${status}"
