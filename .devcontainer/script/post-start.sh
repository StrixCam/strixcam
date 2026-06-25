#!/usr/bin/env bash
# Runs every time the container starts, as the remote user.
# Sanity-check that the cross-compile toolchain and sysroot are in place; fail
# loudly if a rebuild dropped them rather than letting cmake fail cryptically.
set -euo pipefail

status=0
if [ ! -d "${CROSS_SYSROOT}" ]; then
    echo "[post-start] ERROR: sysroot missing at CROSS_SYSROOT=${CROSS_SYSROOT}" >&2
    status=1
fi
if [ ! -x "${BOOTLIN_TOOLCHAIN_BIN}/aarch64-buildroot-linux-gnu-gcc" ]; then
    echo "[post-start] ERROR: cross compiler missing in BOOTLIN_TOOLCHAIN_BIN=${BOOTLIN_TOOLCHAIN_BIN}" >&2
    status=1
fi
[ "${status}" -eq 0 ] && echo "[post-start] toolchain + sysroot OK"
exit "${status}"
