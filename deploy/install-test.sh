#!/usr/bin/env bash
# =============================================================================
# Tests for deploy/install.sh — guards the RUNTIME_DEPS contract that the binary
# needs at load time. The class of bug this prevents: a system library the build
# links (present in the sysroot) but absent from a base JetPack flash, so the
# deployed binary fails with "error while loading shared libraries" (exit 127).
#
# No device / apt / network required — these are static contract checks that run
# in CI's `ci-scripts` job alongside shellcheck.
#
# Run: deploy/install-test.sh   (exit 0 = all pass)
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_SH="${SCRIPT_DIR}/install.sh"
SYSROOT_URLS="${REPO_ROOT}/.devcontainer/sysroot/noble-deb-urls.txt"

pass=0
fail=0

ok()   { pass=$((pass + 1)); }
bad()  { fail=$((fail + 1)); printf '  FAIL: %s\n' "$1"; }

# Extract the RUNTIME_DEPS=( ... ) array contents from install.sh as words.
runtime_deps() {
  sed -nE '/^RUNTIME_DEPS=\(/,/\)/p' "$INSTALL_SH" \
    | tr '\n' ' ' \
    | sed -E 's/.*RUNTIME_DEPS=\(//; s/\).*//'
}

read -r -a DEPS <<<"$(runtime_deps)"

# 1. The list is non-empty (an empty list silently reintroduces the original bug).
if [ "${#DEPS[@]}" -gt 0 ]; then ok; else bad "RUNTIME_DEPS is empty"; fi

# 2. The two known device-absent runtime packages are present.
for required in libsdbus-c++1 libgstrtspserver-1.0-0; do
  found="no"
  for d in "${DEPS[@]}"; do [ "$d" = "$required" ] && found="yes"; done
  if [ "$found" = "yes" ]; then ok; else bad "RUNTIME_DEPS missing '$required'"; fi
done

# 3. Drift guard: every RUNTIME_DEPS entry must correspond to a package the build
#    sysroot actually pulls in (so the list can't name a phantom package, and a
#    sysroot rename trips this test). noble-deb-urls.txt URL-encodes '++' as
#    '%2b%2b'; the filename is "<pkg>_<version>_<arch>.deb".
if [ -f "$SYSROOT_URLS" ]; then
  for d in "${DEPS[@]}"; do
    enc="${d//+/%2b}"           # libsdbus-c++1 -> libsdbus-c%2b%2b1
    if grep -qiE "/${enc}_[0-9]" "$SYSROOT_URLS"; then
      ok
    else
      bad "RUNTIME_DEPS entry '$d' not found in sysroot manifest (drift?)"
    fi
  done
else
  bad "sysroot manifest not found: $SYSROOT_URLS"
fi

# 4. install.sh wires the preflight check in before the service is stopped — the
#    line ordering that makes the gate protective rather than cosmetic.
# grep patterns are literal source text, not strings to expand:
# shellcheck disable=SC2016
preflight_ln="$(grep -n 'check_runtime_libs "\$new_bin"' "$INSTALL_SH" | head -n1 | cut -d: -f1)"
# shellcheck disable=SC2016
stop_ln="$(grep -n 'Stopping ${SERVICE}' "$INSTALL_SH" | head -n1 | cut -d: -f1)"
if [ -n "$preflight_ln" ] && [ -n "$stop_ln" ] && [ "$preflight_ln" -lt "$stop_ln" ]; then
  ok
else
  bad "preflight check_runtime_libs must run before the service is stopped"
fi

printf '\ndeploy/install-test.sh: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
