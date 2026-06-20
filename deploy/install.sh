#!/usr/bin/env bash
# =============================================================================
# install.sh — update the sst-cam-firmware binary on a Jetson from a GitHub
# Release, idempotently and safely.
#
# Resolves a Release asset (named sst_cam_firmware-<tag>-aarch64) from the
# PUBLIC repo ScoutSportTechnology/sst-cam-firmware via the GitHub API, then
# performs a safe swap. GITHUB_TOKEN is OPTIONAL: the repo is public, so the
# asset downloads over its public browser_download_url with no auth. A token is
# only useful to lift the 60 req/hr unauthenticated GitHub API rate limit, and
# is sent on the metadata request only — never on the asset download.
#
#   1. Fail early (BEFORE touching the running service) on a failed download or
#      a file that is not a valid aarch64 ELF.
#   2. If the installed binary already matches the new one (sha256) -> no-op.
#   3. systemctl stop  <service>
#   4. Atomic swap: download to a temp path on the SAME filesystem, `mv` into
#      place; keep a backup of the previous binary.
#   5. systemctl start <service>
#   6. If the service fails to become active, restore the backup, restart, and
#      exit non-zero. Otherwise print `systemctl is-active`.
#
# Usage:
#   sudo ./install.sh                          # latest release (public, no token)
#   sudo ./install.sh --version v1.2.3
#   sudo GITHUB_TOKEN=ghp_xxx ./install.sh     # optional: avoids API rate limit
# =============================================================================
set -euo pipefail

# --- Configuration -----------------------------------------------------------
REPO="ScoutSportTechnology/sst-cam-firmware"
SERVICE="sst-cam-firmware"
INSTALL_DIR="/opt/sst-cam/bin"
INSTALL_PATH="${INSTALL_DIR}/sst_cam_firmware"
BACKUP_PATH="${INSTALL_PATH}.bak"
API="https://api.github.com/repos/${REPO}"

VERSION="latest"

# --- Logging helpers ---------------------------------------------------------
log() { printf '[install] %s\n' "$*"; }
err() { printf '[install] ERROR: %s\n' "$*" >&2; }
die() {
  err "$*"
  exit 1
}

# --- Argument parsing --------------------------------------------------------
usage() {
  cat <<'EOF'
Usage: sudo ./install.sh [--version vX.Y.Z]

Options:
  --version vX.Y.Z   Install a specific release tag (default: latest).
  -h, --help         Show this help.

Environment:
  GITHUB_TOKEN       Optional. The repo is public, so releases download without
                     auth. Set a token only to avoid the 60 req/hr
                     unauthenticated GitHub API rate limit.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      [ "$#" -ge 2 ] || die "--version requires an argument (e.g. v1.2.3)"
      VERSION="$2"
      shift 2
      ;;
    --version=*)
      VERSION="${1#*=}"
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      die "Unknown argument: $1 (try --help)"
      ;;
  esac
done

# --- Preconditions (fail BEFORE touching the service) ------------------------
for cmd in curl jq sha256sum file systemctl install mv cp; do
  command -v "$cmd" >/dev/null 2>&1 || die "required command not found: $cmd"
done

API_VERSION_HEADER="X-GitHub-Api-Version: 2022-11-28"

# GITHUB_TOKEN is optional (public repo). When set, send it on API metadata
# requests only — it lifts the unauthenticated rate limit. The asset download
# uses the public browser_download_url and never needs auth.
auth_args=()
if [ -n "${GITHUB_TOKEN:-}" ]; then
  auth_args=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
  log "Using GITHUB_TOKEN for API requests (rate-limit relief)."
fi

# Temp workspace on the SAME filesystem as INSTALL_PATH so the final mv is
# atomic. Created under INSTALL_DIR's parent; cleaned up on exit.
mkdir -p "$INSTALL_DIR"
TMP_DIR="$(mktemp -d "${INSTALL_DIR%/}/.install.XXXXXX")"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

# --- Resolve the release JSON ------------------------------------------------
if [ "$VERSION" = "latest" ]; then
  release_url="${API}/releases/latest"
else
  release_url="${API}/releases/tags/${VERSION}"
fi

log "Resolving release '${VERSION}' from ${REPO} ..."
release_json="$(curl -fsSL \
  "${auth_args[@]}" \
  -H "Accept: application/vnd.github+json" \
  -H "$API_VERSION_HEADER" \
  "$release_url")" || die "failed to query release '${VERSION}' (check network / tag exists)"

resolved_tag="$(printf '%s' "$release_json" | jq -r '.tag_name // empty')"
[ -n "$resolved_tag" ] || die "could not determine tag for release '${VERSION}'"
log "Resolved tag: ${resolved_tag}"

# --- Find the aarch64 asset --------------------------------------------------
# Asset name convention: sst_cam_firmware-<tag>-aarch64 (published by
# release-alpha.yml and release-beta.yml). Public repo -> the browser download
# URL serves the raw asset with no auth.
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
curl -fsSL \
  -o "$new_bin" \
  "$asset_url" || die "download failed for ${asset_name}"

[ -s "$new_bin" ] || die "downloaded file is empty: ${asset_name}"

# --- Validate it is an aarch64 ELF (still BEFORE touching the service) -------
file_desc="$(file -b "$new_bin")"
case "$file_desc" in
  *ELF*aarch64* | *ELF*ARM\ aarch64*)
    log "Validated aarch64 ELF: ${file_desc}"
    ;;
  *)
    die "downloaded asset is not an aarch64 ELF (got: ${file_desc})"
    ;;
esac
chmod +x "$new_bin"

# --- Idempotency: skip if already installed and identical --------------------
if [ -f "$INSTALL_PATH" ]; then
  new_sum="$(sha256sum "$new_bin" | awk '{print $1}')"
  cur_sum="$(sha256sum "$INSTALL_PATH" | awk '{print $1}')"
  if [ "$new_sum" = "$cur_sum" ]; then
    log "Installed binary already matches ${resolved_tag} (sha256 ${new_sum}). No-op."
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
# new_bin already lives on the same filesystem as INSTALL_PATH (TMP_DIR under
# INSTALL_DIR), so mv is an atomic rename.
mv -f "$new_bin" "$INSTALL_PATH"
chmod +x "$INSTALL_PATH"

# --- Start the service and verify --------------------------------------------
log "Starting ${SERVICE} ..."
start_failed="no"
systemctl start "$SERVICE" || start_failed="yes"

# Give systemd a moment to settle, then check liveness.
if [ "$start_failed" = "no" ] && systemctl is-active --quiet "$SERVICE"; then
  log "Update to ${resolved_tag} complete."
  log "Service status: $(systemctl is-active "$SERVICE")"
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
  err "No backup available to roll back to (this may have been a first install)."
fi

if [ "$service_was_active" = "yes" ]; then
  err "Service was active before this run; see 'journalctl -u ${SERVICE}' for the new binary's failure."
fi
exit 1
