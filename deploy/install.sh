#!/usr/bin/env bash
# =============================================================================
# install.sh — set up and update the sst-cam-firmware binary on a Jetson from a
# GitHub Release, idempotently and safely.
#
# This script is SELF-CONTAINED and published as a Release asset, so it can run
# straight off the release with no repo clone:
#
#   curl -fsSL https://github.com/ScoutSportTechnology/sst-cam-firmware/releases/latest/download/install.sh | sudo bash
#   curl -fsSL .../releases/latest/download/install.sh | sudo bash -s -- --version v0.1.0-beta.2
#   curl -fsSL .../releases/latest/download/install.sh | sudo bash -s -- --sha256 <hex>
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
API="https://api.github.com/repos/${REPO}"
EXPECTED_L4T_MAJOR=39   # JetPack 7.2

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

The first run performs one-time setup automatically (creates the sst-cam user,
/opt/sst-cam, and the systemd unit). Re-running is safe and idempotent.

Environment:
  GITHUB_TOKEN            Optional; lifts the GitHub API rate limit only.
  SST_SKIP_PLATFORM_CHECK Set to 1 to bypass the JetPack-7.2 device guard.
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
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
# Non-root system user. Hardware access (cameras, GPIO, NVENC, BlueZ) may need
# extra group membership / udev rules on the device — grant those to this user.
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
  # Best-effort camera access; harmless if the group is absent.
  if getent group video >/dev/null 2>&1; then
    usermod -aG video "$SERVICE_USER" 2>/dev/null || true
  fi

  if [ ! -d "$INSTALL_DIR" ]; then
    log "Setup: creating ${INSTALL_DIR} ..."
    mkdir -p "$INSTALL_DIR"
    changed="yes"
  fi
  chown -R "${SERVICE_USER}:${SERVICE_USER}" "$INSTALL_ROOT"

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

[ "$DO_SETUP" = "yes" ] && ensure_setup

# Temp workspace on the SAME filesystem as INSTALL_PATH so the final mv is atomic.
mkdir -p "$INSTALL_DIR"
TMP_DIR="$(mktemp -d "${INSTALL_DIR%/}/.install.XXXXXX")"
# shellcheck disable=SC2329  # invoked indirectly via the EXIT trap below
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
    log "Service status: $(systemctl is-active "$SERVICE" 2>/dev/null || echo unknown)"
    exit 0
  fi
fi

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
exit 1
