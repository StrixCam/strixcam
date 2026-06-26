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

# 5. Config-dir provisioning: the firmware self-writes default JSON on first boot
#    as the non-root service user, so install.sh must create CONFIG_DIR and own
#    it to that user. Without this the service crash-loops on a fresh device.
config_dir="$(sed -nE 's/^CONFIG_DIR="([^"]+)".*/\1/p' "$INSTALL_SH" | head -n1)"
if [ -n "$config_dir" ]; then ok; else bad "CONFIG_DIR not defined in install.sh"; fi
# shellcheck disable=SC2016
if grep -q 'mkdir -p "$CONFIG_DIR"' "$INSTALL_SH"; then ok; else bad "install.sh must mkdir CONFIG_DIR"; fi
# the SST config subtree must be chowned to the service user (string-built path ok)
# shellcheck disable=SC2016
if grep -qE 'chown -R "\$\{SERVICE_USER\}:\$\{SERVICE_USER\}" "/etc/sst"' "$INSTALL_SH"; then
  ok
else
  bad "install.sh must chown the /etc/sst config subtree to SERVICE_USER"
fi

# 6. Drift guard: install.sh CONFIG_DIR must byte-match the compiled-in path in
#    src/main.cpp (kConfigDir) — a mismatch silently reintroduces this bug
#    (firmware reads one path, install.sh provisions another).
MAIN_CPP="${REPO_ROOT}/src/main.cpp"
if [ -f "$MAIN_CPP" ]; then
  main_dir="$(sed -nE 's/.*kConfigDir *= *"([^"]+)".*/\1/p' "$MAIN_CPP" | head -n1)"
  if [ -n "$main_dir" ] && [ "$main_dir" = "$config_dir" ]; then
    ok
  else
    bad "CONFIG_DIR ('$config_dir') != src/main.cpp kConfigDir ('$main_dir')"
  fi
else
  bad "src/main.cpp not found: $MAIN_CPP"
fi

# 7. GPU/CUDA access: install.sh must add the service user to the 'render' group.
#    The capture pipeline runs CUDA (nvvidconv/NvBufSurface) in the firmware's own
#    process; without render the non-root service gets cudaErrorNotSupported
#    (status=801) and captures zero frames.
if grep -qE 'video render' "$INSTALL_SH"; then ok; else bad "install.sh must grant SERVICE_USER the render group (GPU/CUDA)"; fi

# 8. Camera overlay provisioning: an idempotent, fdtoverlay-based merge that wires
#    the bootloader via an FDT line, merging from a captured pristine base DTB so
#    switching overlays never stacks one tree onto another.
if grep -q 'ensure_camera_overlay()' "$INSTALL_SH"; then ok; else bad "install.sh must define ensure_camera_overlay"; fi
if grep -q 'fdtoverlay -i' "$INSTALL_SH"; then ok; else bad "ensure_camera_overlay must merge via fdtoverlay"; fi
if grep -q 'sst-cam-base.dtb' "$INSTALL_SH"; then ok; else bad "ensure_camera_overlay must merge from a captured pristine base DTB"; fi

# 9. NOT pinned to one camera: the overlay must be configurable + skippable.
if grep -q -- '--no-camera' "$INSTALL_SH" && grep -q -- '--camera-overlay' "$INSTALL_SH"; then
  ok
else
  bad "install.sh must expose --camera-overlay/--no-camera (configurable, not pinned)"
fi
if grep -q 'SST_CAMERA_OVERLAY' "$INSTALL_SH"; then ok; else bad "install.sh must honour the SST_CAMERA_OVERLAY override"; fi

# 10. A camera overlay needs a reboot; install.sh must surface that on EVERY exit
#     (the no-op and success paths both exit 0 early) via the EXIT trap.
if grep -q 'trap print_pending_actions EXIT' "$INSTALL_SH"; then ok; else bad "install.sh must announce the pending reboot on every exit path"; fi

# 11. Device-identity provisioning: install.sh must give each unit a UNIQUE BLE
#     name. The firmware renders `sst-cam-NNNN` from device.json's serial_number;
#     its built-in default is the placeholder "00000000", so every un-provisioned
#     unit collides on `sst-cam-0000`. install.sh writes device.json with a unique
#     serial BEFORE first boot, and must NEVER clobber an existing one (mirrors the
#     firmware's EnsureDefault never-clobber rule → stable identity across re-runs).

# 11a. Static contract: the provisioning function exists and is wired into setup.
if grep -q 'ensure_device_identity()' "$INSTALL_SH"; then ok; else bad "install.sh must define ensure_device_identity"; fi
if grep -qE '^[[:space:]]*ensure_device_identity$' "$INSTALL_SH"; then ok; else bad "install.sh must call ensure_device_identity during setup"; fi
# shellcheck disable=SC2016
if grep -q 'DEVICE_JSON="${CONFIG_DIR}/device.json"' "$INSTALL_SH"; then ok; else bad "install.sh must target CONFIG_DIR/device.json (the firmware read path)"; fi
# the new device.json must be chowned to the service user (string-built path ok)
# shellcheck disable=SC2016
if grep -qE 'chown "\$\{SERVICE_USER\}:\$\{SERVICE_USER\}" "\$DEVICE_JSON"' "$INSTALL_SH"; then
  ok
else
  bad "install.sh must chown the provisioned device.json to SERVICE_USER"
fi

# 11b. Behavioural: extract generate_serial + ensure_device_identity from install.sh
#      and run them against a throwaway CONFIG_DIR (no root / device / network).
#      Verifies a fresh install writes a NON-placeholder serial and that a re-run
#      leaves an already-provisioned serial unchanged.
IDENTITY_SANDBOX="$(mktemp -d)"
trap 'rm -rf "$IDENTITY_SANDBOX"' EXIT
# Pull the function definitions out of install.sh so we can exercise the real
# logic in isolation. Extract the sentinel-bracketed region (a column-0 '}' inside
# the device.json here-doc makes a brace-based range unsafe).
sed -n '/# --- device-identity:begin/,/# --- device-identity:end/p' \
  "$INSTALL_SH" > "${IDENTITY_SANDBOX}/identity.sh"

serial_first=""
serial_second=""
if [ -s "${IDENTITY_SANDBOX}/identity.sh" ]; then
  # Run in a subshell: stub the globals the functions read, point DEVICE_JSON at
  # the sandbox, no-op the logger and chown, then drive a fresh install + re-run.
  read -r serial_first serial_second < <(
    CONFIG_DIR="${IDENTITY_SANDBOX}/config"
    DEVICE_JSON="${CONFIG_DIR}/device.json"
    # SERVICE_USER / log / chown are read by the sourced install.sh functions, not
    # by this harness directly — shellcheck can't see across the `.` so silence it.
    # shellcheck disable=SC2034
    SERVICE_USER="nobody"
    mkdir -p "$CONFIG_DIR"
    # shellcheck disable=SC2329
    log()   { :; }
    # shellcheck disable=SC2329
    chown() { :; }            # no privileges in the test harness
    # shellcheck source=/dev/null
    . "${IDENTITY_SANDBOX}/identity.sh"
    ensure_device_identity >/dev/null 2>&1
    s1="$(sed -nE 's/.*"serial_number":[[:space:]]*"([^"]+)".*/\1/p' "$DEVICE_JSON" | head -n1)"
    ensure_device_identity >/dev/null 2>&1          # re-run: must NOT clobber
    s2="$(sed -nE 's/.*"serial_number":[[:space:]]*"([^"]+)".*/\1/p' "$DEVICE_JSON" | head -n1)"
    printf '%s %s\n' "$s1" "$s2"
  )
else
  bad "could not extract generate_serial/ensure_device_identity from install.sh"
fi

# Fresh install wrote a serial at all.
if [ -n "$serial_first" ]; then ok; else bad "fresh install did not write a serial_number to device.json"; fi
# Fresh serial is NOT the firmware placeholder (the whole point of the fix).
if [ -n "$serial_first" ] && [ "$serial_first" != "00000000" ]; then
  ok
else
  bad "fresh install wrote the placeholder serial '$serial_first' (BLE name would collide on sst-cam-0000)"
fi
# Re-running install.sh must NOT change an already-provisioned serial.
if [ -n "$serial_first" ] && [ "$serial_first" = "$serial_second" ]; then
  ok
else
  bad "re-run changed the serial ('$serial_first' -> '$serial_second'); identity must be stable"
fi

# 11c. The provisioned serial must match what the firmware default schema declares
#      as the placeholder, so the test's "non-placeholder" assertion can't drift
#      from the real default. Read the placeholder straight from config-defaults.hpp.
DEFAULTS_HPP="${REPO_ROOT}/src/app/config/services/config_loader/config-defaults.hpp"
if [ -f "$DEFAULTS_HPP" ]; then
  placeholder="$(sed -nE 's/.*"serial_number":[[:space:]]*"([^"]+)".*/\1/p' "$DEFAULTS_HPP" | head -n1)"
  if [ -n "$placeholder" ] && [ "$serial_first" != "$placeholder" ]; then
    ok
  else
    bad "provisioned serial '$serial_first' equals the firmware placeholder '$placeholder' from config-defaults.hpp"
  fi
else
  bad "config-defaults.hpp not found: $DEFAULTS_HPP"
fi

# 12. Local-binary install path (the local validation loop): install.sh must
#     accept --binary, install a local file WITHOUT touching GitHub, and still
#     run the shared aarch64-ELF guard so a host build can't be installed.
if grep -q -- '--binary' "$INSTALL_SH"; then ok; else bad "install.sh must expose --binary for the local install loop"; fi
# shellcheck disable=SC2016
if grep -q 'if \[ -n "\$LOCAL_BINARY" \]; then' "$INSTALL_SH"; then
  ok
else
  bad "install.sh must branch on LOCAL_BINARY to skip the GitHub download"
fi
# The ELF/aarch64 validation must live OUTSIDE the download branch so it guards
# the --binary path too (rejecting an x86_64 host build).
if grep -q 'ELF\*aarch64' "$INSTALL_SH"; then ok; else bad "install.sh must keep the aarch64-ELF guard for both install paths"; fi

printf '\ndeploy/install-test.sh: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
