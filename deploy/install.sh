#!/usr/bin/env bash
# =============================================================================
# install.sh — set up and update the sst-cam-firmware binary on a Jetson from a
# GitHub Release, idempotently and safely.
#
# This script is SELF-CONTAINED. It lives only in the repo (single source of
# truth — it is NOT duplicated as a release asset); pull it from the repo at the
# tag you're installing and pipe to bash (the exact line is in each release's
# notes):
#
#   curl -fsSL https://raw.githubusercontent.com/ScoutSportTechnology/sst-cam-firmware/<TAG>/deploy/install.sh \
#     | sudo bash -s -- --version <TAG>
#
# Or, from a repo checkout on the device:  sudo deploy/install.sh --version <TAG>
#
# On every run it:
#   0. ONE-TIME SETUP (idempotent): creates the `sst-cam` system user, the
#      /opt/sst-cam/bin dir, and installs + enables the systemd unit if missing.
#   1. Resolves a Release by tag (--version) OR by recorded binary digest
#      (--sha256), defaulting to `latest`.
#   2. Downloads the `sst_cam_firmware-<tag>-aarch64` asset; fails BEFORE
#      touching the service on a bad download, non-aarch64 ELF, platform
#      mismatch, or a sha256 that disagrees with the release notes.
#   3. No-op if the installed binary already matches (sha256).
#   4. Stops the service, atomically swaps the binary (backup kept), restarts.
#   5. Rolls back to the backup if the service fails to come up.
#
# GITHUB_TOKEN is OPTIONAL (public repo): set it only to lift the 60 req/hr
# unauthenticated GitHub API rate limit. It is sent on metadata requests only.
# =============================================================================
set -euo pipefail

# --- Configuration -----------------------------------------------------------
REPO="ScoutSportTechnology/sst-cam-firmware"
SERVICE="sst-cam-firmware"
SERVICE_USER="sst-cam"
INSTALL_ROOT="/opt/sst-cam"
INSTALL_DIR="${INSTALL_ROOT}/bin"
INSTALL_PATH="${INSTALL_DIR}/sst_cam_firmware"
BACKUP_PATH="${INSTALL_PATH}.bak"
UNIT_PATH="/etc/systemd/system/${SERVICE}.service"
# Config dir the firmware reads on boot. MUST match the compiled-in path in
# src/main.cpp (kConfigDir). The service runs as the non-root SERVICE_USER and
# self-writes default JSON here on first boot, so the dir must exist and be owned
# by that user — it lives under root-owned /etc, which the user cannot create on
# its own. deploy/install-test.sh guards this path against drift from main.cpp.
CONFIG_DIR="/etc/sst/cam/config"
API="https://api.github.com/repos/${REPO}"
EXPECTED_L4T_MAJOR=39   # JetPack 7.2

# Camera device-tree overlay to provision. jetson-io is broken on this board
# ("No DTB found"), so we pre-merge the chosen overlay into the device tree
# offline and point the bootloader at the result via an FDT line.
#
# NOT pinned to one camera: this is the OVERLAY NAME, overridable per device.
# Accepts a bare .dtbo basename (resolved under /boot) or an absolute path.
# Default is the dual-IMX477 overlay for the Orin Nano p3768 carrier (the stock
# NVIDIA imx477 driver drives the RPi-HQ-compatible ArduCAM 12MP IMX477; ArduCAM
# ships no JetPack-7.2 build). Override per device with --camera-overlay <name>
# or SST_CAMERA_OVERLAY, list choices with --list-camera-overlays, or skip
# camera provisioning entirely with --no-camera.
DEFAULT_CAMERA_OVERLAY="tegra234-p3767-camera-p3768-imx477-dual.dtbo"
CAMERA_OVERLAY="${SST_CAMERA_OVERLAY:-$DEFAULT_CAMERA_OVERLAY}"
PROVISION_CAMERA="yes"
EXTLINUX_CONF="/boot/extlinux/extlinux.conf"
REBOOT_NEEDED="no"

# Runtime shared-library packages the firmware dynamically links that the JetPack
# 7.2 base flash does NOT already ship. SOURCE OF TRUTH for this list:
# .devcontainer/sysroot/003_install_extra_pkgs.sh — the packages it ADDS to the
# build sysroot whose *runtime* is not in the L4T r39.2 sample rootfs. protobuf
# and opencv runtime ARE in the rootfs (that script says so) → not listed here;
# sdbus-c++ (BLE control plane) and gst-rtsp-server (app RTSP) are full adds and
# must be installed on the device or the binary fails to load (exit 127). Keep in
# sync with that script; deploy/install-test.sh guards against drift.
RUNTIME_DEPS=(libsdbus-c++1 libgstrtspserver-1.0-0)

VERSION="latest"        # tag selector (default)
WANT_SHA=""             # --sha256 selector / verifier

# --- Logging helpers ---------------------------------------------------------
log() { printf '[install] %s\n' "$*"; }
err() { printf '[install] ERROR: %s\n' "$*" >&2; }
die() {
  err "$*"
  exit 1
}

usage() {
  cat <<'EOF'
Usage: sudo ./install.sh [--version vX.Y.Z | --sha256 <hex>] [--no-setup]

Selects the release to install (default: latest):
  --version vX.Y.Z   Install a specific release tag (e.g. v0.1.0-beta.2).
  --sha256 <hex>     Install the release whose recorded binary digest matches
                     <hex> (also verifies the download against it).
  --no-setup         Skip the one-time setup (user/dir/systemd unit) check.
  -h, --help         Show this help.

Camera (device-tree overlay) provisioning:
  --camera-overlay <name|path>  Overlay to apply (a .dtbo basename resolved under
                     /boot, or an absolute path). Not pinned to one camera;
                     default: dual-IMX477 for the Orin Nano p3768 carrier.
  --no-camera        Skip camera overlay provisioning entirely.
  --list-camera-overlays  List camera .dtbo overlays available in /boot and exit.

The first run performs one-time setup automatically (creates the sst-cam user,
/opt/sst-cam, and the systemd unit). Re-running is safe and idempotent. Applying
a camera overlay requires a reboot, which the script will remind you to do.

Environment:
  GITHUB_TOKEN            Optional; lifts the GitHub API rate limit only.
  SST_SKIP_PLATFORM_CHECK Set to 1 to bypass the JetPack-7.2 device guard.
  SST_CAMERA_OVERLAY      Default camera overlay (same value as --camera-overlay).
EOF
}

# --- Argument parsing --------------------------------------------------------
DO_SETUP="yes"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      [ "$#" -ge 2 ] || die "--version requires an argument (e.g. v0.1.0-beta.2)"
      VERSION="$2"; shift 2 ;;
    --version=*) VERSION="${1#*=}"; shift ;;
    --sha256)
      [ "$#" -ge 2 ] || die "--sha256 requires a 64-hex digest argument"
      WANT_SHA="$2"; shift 2 ;;
    --sha256=*) WANT_SHA="${1#*=}"; shift ;;
    --no-setup) DO_SETUP="no"; shift ;;
    --no-camera) PROVISION_CAMERA="no"; shift ;;
    --camera-overlay)
      [ "$#" -ge 2 ] || die "--camera-overlay requires a .dtbo basename or path"
      CAMERA_OVERLAY="$2"; shift 2 ;;
    --camera-overlay=*) CAMERA_OVERLAY="${1#*=}"; shift ;;
    --list-camera-overlays)
      echo "Available camera overlays in /boot:"
      ls /boot/*camera*.dtbo 2>/dev/null | sed 's:.*/:  :;s:\.dtbo$::' || echo "  (none found)"
      exit 0 ;;
    -h | --help) usage; exit 0 ;;
    *) die "Unknown argument: $1 (try --help)" ;;
  esac
done

# Normalise / validate a provided digest.
if [ -n "$WANT_SHA" ]; then
  WANT_SHA="$(printf '%s' "$WANT_SHA" | tr '[:upper:]' '[:lower:]')"
  printf '%s' "$WANT_SHA" | grep -qE '^[0-9a-f]{64}$' \
    || die "--sha256 must be a 64-character hex string"
fi

# --- Preconditions -----------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "must run as root (use sudo)."
for cmd in curl jq sha256sum file systemctl install mv cp id cmp; do
  command -v "$cmd" >/dev/null 2>&1 || die "required command not found: $cmd"
done

API_VERSION_HEADER="X-GitHub-Api-Version: 2022-11-28"
auth_args=()
if [ -n "${GITHUB_TOKEN:-}" ]; then
  auth_args=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
  log "Using GITHUB_TOKEN for API requests (rate-limit relief)."
fi

api_get() {  # <url>
  curl -fsSL "${auth_args[@]}" \
    -H "Accept: application/vnd.github+json" \
    -H "$API_VERSION_HEADER" \
    "$1"
}

# --- One-time setup (idempotent) ---------------------------------------------
embedded_unit() {
  cat <<'UNIT'
[Unit]
Description=SST Cam firmware runtime
# Order after the Argus camera daemon so the capture pipeline isn't racing its
# socket at cold boot. Ordering-only: harmless if nvargus-daemon is absent.
After=network-online.target nvargus-daemon.service
Wants=network-online.target

[Service]
Type=simple
# Non-root system user. ensure_setup() grants it the video + render groups for
# camera + GPU/CUDA access; BlueZ/GPIO/NVENC may need further udev rules on the
# device.
User=sst-cam
Group=sst-cam
WorkingDirectory=/opt/sst-cam
ExecStart=/opt/sst-cam/bin/sst_cam_firmware
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
}

ensure_setup() {
  local changed="no"

  if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
    log "Setup: creating system user '${SERVICE_USER}' ..."
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    changed="yes"
  fi
  # Best-effort hardware-access groups; harmless if a group is absent.
  #   video  — cameras (/dev/video*, nvhost, VI/ISP capture nodes)
  #   render — GPU/CUDA via the DRM render node (/dev/dri/renderD*). The capture
  #            pipeline's nvvidconv/NvBufSurface runs CUDA in the firmware's own
  #            process; without render the non-root service gets
  #            cudaErrorNotSupported (status=801) and captures zero frames.
  for _grp in video render; do
    if getent group "$_grp" >/dev/null 2>&1; then
      usermod -aG "$_grp" "$SERVICE_USER" 2>/dev/null || true
    fi
  done

  if [ ! -d "$INSTALL_DIR" ]; then
    log "Setup: creating ${INSTALL_DIR} ..."
    mkdir -p "$INSTALL_DIR"
    changed="yes"
  fi
  chown -R "${SERVICE_USER}:${SERVICE_USER}" "$INSTALL_ROOT"

  # Config dir: the firmware self-writes default JSON here on first boot, but it
  # runs as the non-root SERVICE_USER and cannot create a dir under root-owned
  # /etc. Provision it (and its parents) here and hand ownership to that user.
  if [ ! -d "$CONFIG_DIR" ]; then
    log "Setup: creating ${CONFIG_DIR} ..."
    mkdir -p "$CONFIG_DIR"
    changed="yes"
  fi
  chown -R "${SERVICE_USER}:${SERVICE_USER}" "/etc/sst"

  if [ ! -f "$UNIT_PATH" ] || ! embedded_unit | cmp -s - "$UNIT_PATH"; then
    log "Setup: installing systemd unit -> ${UNIT_PATH} ..."
    embedded_unit > "$UNIT_PATH"
    systemctl daemon-reload
    changed="yes"
  fi
  systemctl enable "$SERVICE" >/dev/null 2>&1 || true

  if [ "$changed" = "yes" ]; then
    log "Setup complete."
  else
    log "Setup already in place."
  fi
}

# Provision the dual-IMX477 device-tree overlay so the sensors are detected.
# Idempotent: once the FDT line is wired to our merged DTB, every later run is a
# no-op (and crucially never re-merges onto an already-merged tree). Requires a
# reboot to take effect, signalled via REBOOT_NEEDED.
ensure_camera_overlay() {
  if [ "$PROVISION_CAMERA" != "yes" ]; then
    log "Camera: provisioning skipped (--no-camera)."
    return 0
  fi
  # Resolve the configured overlay: a bare basename is looked up under /boot, an
  # absolute path is used as-is. Existence here is the guard — no board/camera is
  # hardcoded; an absent overlay (wrong device, or the user wants none) is a
  # clean skip, not an error.
  local dtbo="$CAMERA_OVERLAY"
  case "$dtbo" in
    /*) : ;;
    */*) dtbo="$PWD/$dtbo" ;;
    *) dtbo="/boot/$dtbo" ;;
  esac
  case "$dtbo" in *.dtbo) : ;; *) dtbo="${dtbo}.dtbo" ;; esac
  if [ ! -f "$dtbo" ]; then
    log "Camera: overlay '${CAMERA_OVERLAY}' not found (looked at ${dtbo}) — skipping."
    log "        Pick one with --list-camera-overlays, or pass --no-camera."
    return 0
  fi
  if [ ! -f "$EXTLINUX_CONF" ]; then
    log "Camera: ${EXTLINUX_CONF} not found — skipping overlay."
    return 0
  fi

  local stem merged
  stem="$(basename "$dtbo" .dtbo)"
  merged="/boot/sst-cam-${stem}.dtb"

  # Idempotent + switch-safe: if THIS overlay's FDT line is already wired, done.
  if grep -q "FDT ${merged}\$" "$EXTLINUX_CONF"; then
    log "Camera: overlay '${stem}' already provisioned."
    return 0
  fi

  if ! command -v fdtoverlay >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1; then
    log "Camera: installing device-tree-compiler (fdtoverlay) ..."
    apt-get install -y --no-install-recommends device-tree-compiler >/dev/null 2>&1 || true
  fi
  command -v fdtoverlay >/dev/null 2>&1 || {
    log "Camera: fdtoverlay unavailable (install device-tree-compiler) — skipping overlay."
    return 0
  }

  # A pre-existing FDT line that we did NOT place (no /boot/sst-cam-*.dtb) means
  # the device has a custom DTB override — don't clobber it.
  if grep -qE '^\s*FDT ' "$EXTLINUX_CONF" && ! grep -qE '^\s*FDT /boot/sst-cam-.*\.dtb' "$EXTLINUX_CONF"; then
    log "Camera: a non-managed FDT override is already set in ${EXTLINUX_CONF} — skipping to avoid clobbering it."
    return 0
  fi

  # Always merge from a PRISTINE base DTB so switching overlays never stacks one
  # tree onto another. Capture that base once, on first provisioning, while the
  # live tree (/sys/firmware/fdt) still carries no overlay.
  local base_dtb="/boot/sst-cam-base.dtb"
  if grep -qE '^[[:space:]]*FDT /boot/sst-cam-.*\.dtb' "$EXTLINUX_CONF" && [ ! -f "$base_dtb" ]; then
    log "Camera: a managed FDT is set but base ${base_dtb} is missing — cannot re-merge safely."
    log "        Remove the FDT line from ${EXTLINUX_CONF}, reboot, then re-run."
    return 0
  fi

  log "Camera: provisioning overlay '${stem}' ..."
  if [ ! -f "$base_dtb" ] && ! cp /sys/firmware/fdt "$base_dtb" 2>/dev/null; then
    log "warning: could not capture base device tree — skipping camera overlay."
    return 0
  fi
  if ! fdtoverlay -i "$base_dtb" -o "$merged" "$dtbo" 2>/dev/null; then
    log "warning: fdtoverlay merge failed — cameras will not be available."
    return 0
  fi
  cp -a "$EXTLINUX_CONF" "${EXTLINUX_CONF}.bak-sstcam" 2>/dev/null || true
  # Drop any previously-managed FDT line, then point the bootloader at the new
  # merged DTB (insert after the primary label's LINUX line; the OVERLAYS
  # directive is NOT honoured by this board's boot flow).
  sed -i '\#^[[:space:]]*FDT /boot/sst-cam-.*\.dtb#d' "$EXTLINUX_CONF"
  sed -i "/LABEL primary/,/APPEND/ { /LINUX /a\\      FDT ${merged}
}" "$EXTLINUX_CONF"
  REBOOT_NEEDED="yes"
  log "Camera: overlay installed -> ${merged} (reboot required to load it)."
}

# Install the runtime shared-library packages the binary needs but a base JetPack
# flash lacks. Idempotent: dpkg-query skips already-installed packages, so the
# common (everything-present) path costs a few queries and no apt-get. Best-
# effort — the preflight ldd gate below is the hard guarantee.
ensure_runtime_deps() {
  if ! command -v apt-get >/dev/null 2>&1; then
    log "apt-get not found; skipping runtime-dep install (ensure these are present: ${RUNTIME_DEPS[*]})."
    return 0
  fi
  local pkg missing=()
  for pkg in "${RUNTIME_DEPS[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "install ok installed"; then
      missing+=("$pkg")
    fi
  done
  if [ "${#missing[@]}" -eq 0 ]; then
    log "Runtime deps present: ${RUNTIME_DEPS[*]}"
    return 0
  fi
  log "Installing runtime deps: ${missing[*]} ..."
  apt-get update -qq || log "warning: apt-get update failed; attempting install anyway"
  apt-get install -y --no-install-recommends "${missing[@]}" \
    || log "warning: failed to install ${missing[*]} — the preflight check will report any unresolved libs"
}

# Fail BEFORE touching the running service if the new binary has unresolved
# DT_NEEDED libraries on this device. Without this, install.sh would stop the
# service, swap in an unloadable binary (exit 127, crash-loop), then "roll back"
# to a backup carrying the SAME missing dep — leaving the service down either way.
check_runtime_libs() {  # <binary>
  command -v ldd >/dev/null 2>&1 || return 0
  local missing_libs
  missing_libs="$(ldd "$1" 2>/dev/null | awk '/not found/ {print $1}' | sort -u)"
  [ -n "$missing_libs" ] || return 0
  die "$(printf 'the new binary needs shared libraries that are missing on this device:\n%s\nInstall the packages that provide them, then re-run. Known runtime deps: %s\n(If the missing libs are sdbus-c++ / gst-rtsp-server: apt-get install -y %s)' \
    "$(printf '%s\n' "$missing_libs" | sed 's/^/  /')" "${RUNTIME_DEPS[*]}" "${RUNTIME_DEPS[*]}")"
}

# Print the reboot reminder on EVERY exit path (the no-op and success paths both
# exit 0 early), so a freshly-provisioned camera overlay is never silently
# pending. Defined + armed before setup runs so it fires no matter where we exit.
print_pending_actions() {
  if [ "${REBOOT_NEEDED:-no}" = "yes" ]; then
    log ""
    log "==> ACTION REQUIRED: a camera device-tree overlay was installed."
    log "==> REBOOT to load it:  sudo reboot"
    log "==> Until you reboot, the IMX477 cameras will not be detected."
  fi
}
trap print_pending_actions EXIT

if [ "$DO_SETUP" = "yes" ]; then
  ensure_setup
  ensure_camera_overlay
fi
# Runtime deps are about the binary, not host setup — install them even with
# --no-setup so an unloadable binary is never deployed.
ensure_runtime_deps

# Temp workspace on the SAME filesystem as INSTALL_PATH so the final mv is atomic.
mkdir -p "$INSTALL_DIR"
TMP_DIR="$(mktemp -d "${INSTALL_DIR%/}/.install.XXXXXX")"
# shellcheck disable=SC2317,SC2329  # invoked indirectly via the EXIT trap below
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

# --- Resolve the release JSON (by tag or by recorded sha256) ------------------
resolve_tag_by_sha() {  # <hex> -> echoes tag_name
  local want="$1" releases
  log "Searching releases for the binary with sha256 ${want} ..."
  releases="$(api_get "${API}/releases?per_page=100")" \
    || die "failed to list releases"
  printf '%s' "$releases" \
    | jq -r --arg s "$want" \
        '.[] | select((.body // "") | test("sha256:[[:space:]]*" + $s)) | .tag_name' \
    | head -n1
}

if [ -n "$WANT_SHA" ] && [ "$VERSION" = "latest" ]; then
  VERSION="$(resolve_tag_by_sha "$WANT_SHA")"
  [ -n "$VERSION" ] || die "no release found whose notes record sha256 ${WANT_SHA}"
  log "Digest ${WANT_SHA} -> release ${VERSION}"
fi

if [ "$VERSION" = "latest" ]; then
  release_url="${API}/releases/latest"
else
  release_url="${API}/releases/tags/${VERSION}"
fi

log "Resolving release '${VERSION}' from ${REPO} ..."
release_json="$(api_get "$release_url")" \
  || die "failed to query release '${VERSION}' (check network / tag exists)"

resolved_tag="$(printf '%s' "$release_json" | jq -r '.tag_name // empty')"
[ -n "$resolved_tag" ] || die "could not determine tag for release '${VERSION}'"
log "Resolved tag: ${resolved_tag}"

# Recorded digest from the release notes (the verified hand-off contract).
notes_sha="$(printf '%s' "$release_json" | jq -r '.body // ""' \
  | sed -nE 's/.*sha256:[[:space:]]*([0-9a-fA-F]{64}).*/\1/p' | head -n1 | tr '[:upper:]' '[:lower:]')"

# --- Find the aarch64 asset --------------------------------------------------
asset_url="$(printf '%s' "$release_json" \
  | jq -r '.assets[] | select(.name | endswith("-aarch64")) | .browser_download_url' \
  | head -n1)"
asset_name="$(printf '%s' "$release_json" \
  | jq -r '.assets[] | select(.name | endswith("-aarch64")) | .name' \
  | head -n1)"
[ -n "$asset_url" ] || die "no '*-aarch64' asset found on release ${resolved_tag}"
log "Found asset: ${asset_name}"

# --- Download the asset (public browser_download_url, no auth) ----------------
new_bin="${TMP_DIR}/sst_cam_firmware"
log "Downloading ${asset_name} ..."
curl -fsSL -o "$new_bin" "$asset_url" || die "download failed for ${asset_name}"
[ -s "$new_bin" ] || die "downloaded file is empty: ${asset_name}"

dl_sha="$(sha256sum "$new_bin" | awk '{print $1}')"

# --- Integrity: verify the download against the recorded / requested digest --
if [ -n "$notes_sha" ] && [ "$dl_sha" != "$notes_sha" ]; then
  die "sha256 mismatch: downloaded ${dl_sha} but release notes record ${notes_sha} (asset may have been swapped)."
fi
if [ -n "$WANT_SHA" ] && [ "$dl_sha" != "$WANT_SHA" ]; then
  die "sha256 mismatch: downloaded ${dl_sha} != requested ${WANT_SHA}."
fi
log "Verified sha256: ${dl_sha}"

# --- Validate it is an aarch64 ELF (still BEFORE touching the service) -------
file_desc="$(file -b "$new_bin")"
case "$file_desc" in
  *ELF*aarch64*)
    log "Validated aarch64 ELF: ${file_desc}" ;;
  *)
    die "downloaded asset is not an aarch64 ELF (got: ${file_desc})" ;;
esac
chmod +x "$new_bin"

# --- Platform guard: don't cross the JetPack boundary within a version line ---
# beta.1 targeted JetPack 6.2 (L4T r36, glibc 2.35); beta.2+ target JetPack 7.2
# (L4T r39.2, glibc 2.39). A 7.2 binary will not load on a 6.2 flash. The ELF
# check can't catch this (both aarch64) — read the device L4T major.
if [ "${SST_SKIP_PLATFORM_CHECK:-0}" != "1" ] && [ -r /etc/nv_tegra_release ]; then
  dev_l4t="$(sed -n 's/^# R\([0-9]\{1,\}\).*/\1/p' /etc/nv_tegra_release | head -n1)"
  if [ -n "$dev_l4t" ] && [ "$dev_l4t" != "$EXPECTED_L4T_MAJOR" ]; then
    die "platform mismatch: ${asset_name} targets JetPack 7.2 (L4T r${EXPECTED_L4T_MAJOR}.x) but this device reports L4T r${dev_l4t}.x. Flash JetPack 7.2, or set SST_SKIP_PLATFORM_CHECK=1 to override."
  fi
fi

# --- Idempotency: skip if already installed and identical --------------------
if [ -f "$INSTALL_PATH" ]; then
  cur_sum="$(sha256sum "$INSTALL_PATH" | awk '{print $1}')"
  if [ "$dl_sha" = "$cur_sum" ]; then
    log "Installed binary already matches ${resolved_tag} (sha256 ${dl_sha}). No-op."
    # A matching binary is useless if the unit is down (e.g. a prior crash-loop
    # left it inactive/failed). On a no-op, bring it back up if it isn't active.
    if ! systemctl is-active --quiet "$SERVICE"; then
      log "Service not active; starting ${SERVICE} ..."
      systemctl reset-failed "$SERVICE" >/dev/null 2>&1 || true
      systemctl start "$SERVICE" || log "warning: start returned non-zero"
    fi
    log "Service status: $(systemctl is-active "$SERVICE" 2>/dev/null || echo unknown)"
    log "Logs: journalctl -u ${SERVICE} -f"
    exit 0
  fi
fi

# --- Preflight: all dynamic deps resolvable BEFORE we touch the service -------
check_runtime_libs "$new_bin"

# --- Stop the service, swap atomically, keep a backup ------------------------
service_was_active="no"
if systemctl is-active --quiet "$SERVICE"; then
  service_was_active="yes"
fi

log "Stopping ${SERVICE} ..."
systemctl stop "$SERVICE" || log "warning: stop returned non-zero (service may not have been running)"

if [ -f "$INSTALL_PATH" ]; then
  log "Backing up current binary -> ${BACKUP_PATH}"
  cp -p "$INSTALL_PATH" "$BACKUP_PATH"
fi

log "Installing new binary -> ${INSTALL_PATH}"
mv -f "$new_bin" "$INSTALL_PATH"
chmod +x "$INSTALL_PATH"
chown "${SERVICE_USER}:${SERVICE_USER}" "$INSTALL_PATH" 2>/dev/null || true

# --- Start the service and verify --------------------------------------------
log "Starting ${SERVICE} ..."
start_failed="no"
systemctl start "$SERVICE" || start_failed="yes"

if [ "$start_failed" = "no" ] && systemctl is-active --quiet "$SERVICE"; then
  log "Update to ${resolved_tag} complete."
  log "Service status: $(systemctl is-active "$SERVICE")"
  log "Logs: journalctl -u ${SERVICE} -f"
  exit 0
fi

# --- Rollback on failure -----------------------------------------------------
err "${SERVICE} did not become active after update to ${resolved_tag}."
if [ -f "$BACKUP_PATH" ]; then
  err "Restoring previous binary from backup ..."
  mv -f "$BACKUP_PATH" "$INSTALL_PATH"
  chmod +x "$INSTALL_PATH"
  systemctl start "$SERVICE" || true
  if systemctl is-active --quiet "$SERVICE"; then
    err "Rolled back to previous binary; service is active again."
  else
    err "Rollback restart FAILED — service is down. Manual intervention required."
  fi
else
  err "No backup to roll back to (first install). The new binary failed to start —"
  err "this is expected if config (/etc/sst/cam/config/*.json) or cameras are missing."
fi

if [ "$service_was_active" = "yes" ]; then
  err "See 'journalctl -u ${SERVICE}' for the new binary's failure."
fi
log "Logs: journalctl -u ${SERVICE} -f"
exit 1
