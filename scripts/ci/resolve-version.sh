#!/usr/bin/env bash
# =============================================================================
# resolve-version.sh — compute the next SemVer tag for the SST maturity ladder.
#
# The single source of version math shared by the alpha / release-beta / promote
# workflows. Isolated here (instead of inline YAML) so it is unit-testable; the
# prerelease counter precedence is the one genuinely tricky bit.
#
#   resolve-version.sh alpha            base = bump(latest stable tag);
#                                       -> vBASE-alpha.(maxN+1)
#   resolve-version.sh beta  X.Y.Z      base = X.Y.Z (release branch name);
#                                       -> vX.Y.Z-beta.(maxN+1)
#   resolve-version.sh stable X.Y.Z     -> vX.Y.Z   (no suffix)
#
# Base bump (alpha) follows Conventional Commits since the latest *stable* tag:
#   feat!: / BREAKING CHANGE -> major   feat: -> minor   fix:/perf: -> patch
#   docs/chore/ci/test/refactor-only    -> released=false (mint nothing)
# With no stable tag the base bumps from an implicit v0.0.0 (so a first `feat:`
# yields base 0.1.0). This explicit v0.0.0 baseline sidesteps the historical
# "No releasable since v0.0.0" full-history-scan anomaly.
#
# Overrides (seeding / manual dispatch), both env-supplied:
#   IN_VERSION=vX.Y.Z   force the base version (skip stable-tag scan + bump).
#                       In alpha mode the rung+counter still apply, so a clean
#                       repo seeds deterministically to vX.Y.Z-alpha.1.
#   IN_BUMP=major|minor|patch   force the alpha base bump level (skip commit scan).
#
# Output: `tag=<...>` and `released=<true|false>` on stdout, and appended to
# $GITHUB_OUTPUT when set (workflow step-output consumption). When released is
# false no tag line carries meaning — callers must gate on released.
#
# Numeric prerelease precedence (`-alpha.10` after `-alpha.9`, not lexical) comes
# from `git tag --sort=-v:refname`, which orders SemVer prerelease identifiers.
# =============================================================================
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: resolve-version.sh <alpha|beta|stable> [X.Y.Z]
  alpha            next alpha for the conventional-commit-bumped base
  beta  X.Y.Z      next beta for base X.Y.Z (release branch version)
  stable X.Y.Z     the stable tag vX.Y.Z
env: IN_VERSION=vX.Y.Z (force base), IN_BUMP=major|minor|patch (force alpha bump)
EOF
}

emit() {
  local tag="$1" released="$2"
  echo "tag=${tag}"
  echo "released=${released}"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    { echo "tag=${tag}"; echo "released=${released}"; } >>"$GITHUB_OUTPUT"
  fi
}

# Latest non-prerelease vX.Y.Z (a bare semver with no `-suffix`).
latest_stable() {
  git tag -l 'v*' --sort=-v:refname \
    | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' \
    | head -1 || true
}

# Highest existing counter N for v<base>-<rung>.N (0 when none exist).
# Only strictly-formed counters (N all digits) are considered: a malformed
# immutable tag like v<base>-alpha.x or v<base>-alpha.10.2 must never reach the
# `$((n + 1))` arithmetic (would abort under `set -u`) or mis-sort the counter
# (`${top##*.}` would strip the wrong dot-field). Such tags are filtered out, so
# one typo'd/hand-cut tag cannot permanently brick the prerelease line.
max_counter() {
  local base="$1" rung="$2" top
  local re_base="${base//./\\.}"
  top="$(git tag -l "v${base}-${rung}."'*' --sort=-v:refname \
    | grep -E "^v${re_base}-${rung}\.[0-9]+\$" \
    | head -1 || true)"
  [ -z "$top" ] && { echo 0; return; }
  echo "${top##*.}"
}

valid_base() { echo "$1" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; }

# Echo "<base>|<released>" for alpha mode given the latest stable tag.
bump_base() {
  local latest="$1"
  local lv="${latest#v}"; lv="${lv:-0.0.0}"
  local MA="${lv%%.*}" rest="${lv#*.}" MI PA
  MI="${rest%%.*}"; PA="${rest##*.}"

  local bump="${IN_BUMP:-}"
  if [ -n "$bump" ]; then
    case "$bump" in major|minor|patch) ;; *)
      echo "::error::invalid IN_BUMP '$bump' (want major|minor|patch)" >&2; exit 2 ;;
    esac
  else
    local range log
    if [ -n "$latest" ] && git rev-parse -q --verify "refs/tags/${latest}" >/dev/null 2>&1; then
      range="${latest}..HEAD"
    else
      range="HEAD"
    fi
    log="$(git log --format='%s%n%b' "${range}" 2>/dev/null || true)"
    if echo "$log" | grep -qiE '^BREAKING[ -]CHANGE' || echo "$log" | grep -qE '^[a-z]+(\(.+\))?!:'; then
      bump=major
    elif echo "$log" | grep -qE '^feat(\(.+\))?:'; then
      bump=minor
    elif echo "$log" | grep -qE '^(fix|perf)(\(.+\))?:'; then
      bump=patch
    fi
  fi

  if [ -z "$bump" ]; then echo "|false"; return; fi
  case "$bump" in
    major) MA=$((MA + 1)); MI=0; PA=0 ;;
    minor) MI=$((MI + 1)); PA=0 ;;
    patch) PA=$((PA + 1)) ;;
  esac
  echo "${MA}.${MI}.${PA}|true"
}

mode="${1:-}"
case "$mode" in
  alpha)
    base=""
    if [ -n "${IN_VERSION:-}" ]; then
      echo "${IN_VERSION}" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' \
        || { echo "::error::invalid IN_VERSION '${IN_VERSION}' (want vX.Y.Z)" >&2; exit 1; }
      base="${IN_VERSION#v}"
    else
      out="$(bump_base "$(latest_stable)")"
      base="${out%%|*}"
      [ "${out##*|}" = "true" ] || { emit "" false; exit 0; }
    fi
    n="$(max_counter "$base" alpha)"
    emit "v${base}-alpha.$((n + 1))" true
    ;;
  beta)
    base="${2:-}"
    valid_base "$base" || { echo "::error::beta needs a valid X.Y.Z base (got '${base:-}')" >&2; usage; exit 1; }
    n="$(max_counter "$base" beta)"
    emit "v${base}-beta.$((n + 1))" true
    ;;
  stable)
    base="${2:-}"
    valid_base "$base" || { echo "::error::stable needs a valid X.Y.Z base (got '${base:-}')" >&2; usage; exit 1; }
    emit "v${base}" true
    ;;
  -h | --help)
    usage; exit 0
    ;;
  *)
    echo "::error::unknown mode '${mode:-}'" >&2; usage; exit 2
    ;;
esac
