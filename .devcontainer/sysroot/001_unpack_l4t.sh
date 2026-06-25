#!/usr/bin/env bash
# Unpack JetPack toolchain and sysroot tarballs from the NGC base image.
# The sysroot is split across targetfs.tbz2.aa + targetfs.tbz2.ab in :6.1.
# Usage: 001_unpack_l4t.sh <l4t_dir>
set -euo pipefail

L4T="${1:?Usage: 001_unpack_l4t.sh <l4t_dir>}"
cd "$L4T"

if [[ -f toolchain.tar.bz2 ]]; then
    echo "[001] Unpacking toolchain..."
    tar -I lbzip2 -xf toolchain.tar.bz2
    rm toolchain.tar.bz2
fi

if [[ -f targetfs.tbz2.aa && -f targetfs.tbz2.ab ]]; then
    echo "[001] Unpacking split targetfs (.aa + .ab)..."
    cat targetfs.tbz2.aa targetfs.tbz2.ab | tar -I lbzip2 -x
    rm targetfs.tbz2.aa targetfs.tbz2.ab
elif [[ -f targetfs.tbz2 ]]; then
    echo "[001] Unpacking targetfs..."
    tar -I lbzip2 -xf targetfs.tbz2
    rm targetfs.tbz2
else
    echo "ERROR: no targetfs archive found in $L4T" >&2
    exit 1
fi

echo "[001] Done."
