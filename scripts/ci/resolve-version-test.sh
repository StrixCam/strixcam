#!/usr/bin/env bash
# =============================================================================
# Tests for resolve-version.sh. Each case builds a throwaway git repo (real
# commits + tags) so the suite exercises the actual `git tag --sort=-v:refname`
# precedence the script relies on — not a stubbed approximation.
#
# Run: scripts/ci/resolve-version-test.sh   (exit 0 = all pass)
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESOLVE="${SCRIPT_DIR}/resolve-version.sh"

pass=0
fail=0

# Make a fresh git repo in a temp dir and cd into it. Echoes the dir.
new_repo() {
  local d
  d="$(mktemp -d)"
  git -C "$d" init -q
  git -C "$d" config user.email t@t.t
  git -C "$d" config user.name t
  git -C "$d" config commit.gpgsign false
  echo "$d"
}

commit() { # repo, subject
  git -C "$1" commit -q --allow-empty -m "$2"
}

tag() { # repo, tagname
  git -C "$1" tag "$2"
}

# run <repo> <mode> [base]  -> echoes the `tag=` value; honours IN_VERSION/IN_BUMP env
run() {
  local repo="$1"; shift
  ( cd "$repo" && "$RESOLVE" "$@" ) | sed -n 's/^tag=//p'
}
run_released() {
  local repo="$1"; shift
  ( cd "$repo" && "$RESOLVE" "$@" ) | sed -n 's/^released=//p'
}

expect_eq() { # label, got, want
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf '  FAIL: %s\n        got:  %s\n        want: %s\n' "$1" "$2" "$3"
  fi
}

expect_fail() { # label, repo, mode, [base]
  local label="$1" repo="$2"; shift 2
  if ( cd "$repo" && "$RESOLVE" "$@" ) >/dev/null 2>&1; then
    fail=$((fail + 1)); printf '  FAIL: %s (expected non-zero exit)\n' "$label"
  else
    pass=$((pass + 1))
  fi
}

# --- alpha: no tags + feat -> v0.1.0-alpha.1 (implicit v0.0.0 base) -----------
r="$(new_repo)"; commit "$r" "feat: thing"
expect_eq "alpha no-tags feat -> v0.1.0-alpha.1" "$(run "$r" alpha)" "v0.1.0-alpha.1"

# --- alpha: existing alpha.1 + another feat -> alpha.2 ------------------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-alpha.1; commit "$r" "feat: b"
expect_eq "alpha second feat -> v0.1.0-alpha.2" "$(run "$r" alpha)" "v0.1.0-alpha.2"

# --- alpha: fix-only since last stable -> patch base -------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "fix: y"
expect_eq "alpha fix since stable -> v0.1.1-alpha.1" "$(run "$r" alpha)" "v0.1.1-alpha.1"

# --- alpha: breaking -> major base -------------------------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "feat!: drop field"
expect_eq "alpha breaking -> v1.0.0-alpha.1" "$(run "$r" alpha)" "v1.0.0-alpha.1"

# --- alpha: BREAKING CHANGE body trailer -> major base -----------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0
git -C "$r" commit -q --allow-empty -m "feat: x" -m "BREAKING CHANGE: wire"
expect_eq "alpha BREAKING-CHANGE body -> v1.0.0-alpha.1" "$(run "$r" alpha)" "v1.0.0-alpha.1"

# --- alpha: docs/chore-only -> released=false, no tag ------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "docs: readme"
expect_eq "alpha docs-only released" "$(run_released "$r" alpha)" "false"
expect_eq "alpha docs-only tag empty" "$(run "$r" alpha)" ""

# --- alpha: numeric precedence -alpha.10 > -alpha.9 -> .11 -------------------
r="$(new_repo)"; commit "$r" "feat: a"
for n in 1 2 9 10; do tag "$r" "v0.1.0-alpha.$n"; done
expect_eq "alpha numeric precedence -> alpha.11" "$(run "$r" alpha)" "v0.1.0-alpha.11"

# --- alpha: IN_VERSION seed -> exactly that base, alpha.1 --------------------
r="$(new_repo)"; commit "$r" "docs: nothing releasable"
expect_eq "alpha IN_VERSION seed -> v0.1.0-alpha.1" \
  "$(IN_VERSION=v0.1.0 run "$r" alpha)" "v0.1.0-alpha.1"

# --- alpha: IN_BUMP=major override (skip scan) -------------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "docs: x"
expect_eq "alpha IN_BUMP=major -> v1.0.0-alpha.1" \
  "$(IN_BUMP=major run "$r" alpha)" "v1.0.0-alpha.1"

# --- beta: no beta tags -> beta.1 --------------------------------------------
r="$(new_repo)"; commit "$r" "feat: a"
expect_eq "beta first -> v0.1.0-beta.1" "$(run "$r" beta 0.1.0)" "v0.1.0-beta.1"

# --- beta: existing beta.1 -> beta.2 -----------------------------------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-beta.1
expect_eq "beta second -> v0.1.0-beta.2" "$(run "$r" beta 0.1.0)" "v0.1.0-beta.2"

# --- beta: counter is per-base (alpha tags don't bleed in) -------------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-alpha.5
expect_eq "beta ignores alpha counter -> v0.1.0-beta.1" "$(run "$r" beta 0.1.0)" "v0.1.0-beta.1"

# --- beta: counter is per-BASE (other-base beta tags don't bleed in) ---------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.2.0-beta.3
expect_eq "beta ignores other-base counter -> v0.1.0-beta.1" "$(run "$r" beta 0.1.0)" "v0.1.0-beta.1"

# --- malformed counter: non-numeric N must be ignored, not crash -------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-alpha.2; tag "$r" v0.1.0-alpha.x
expect_eq "alpha ignores non-numeric .x counter -> v0.1.0-alpha.3" "$(run "$r" alpha)" "v0.1.0-alpha.3"

# --- malformed counter: only a non-numeric tag exists -> start at .1 ---------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-alpha.beta
expect_eq "alpha only-malformed counter -> v0.1.0-alpha.1" "$(run "$r" alpha)" "v0.1.0-alpha.1"

# --- malformed counter: multi-dot tag must not mis-sort the counter ----------
r="$(new_repo)"; commit "$r" "feat: a"
tag "$r" v0.1.0-alpha.9; tag "$r" v0.1.0-alpha.10.2
expect_eq "alpha ignores multi-dot alpha.10.2 -> v0.1.0-alpha.10" "$(run "$r" alpha)" "v0.1.0-alpha.10"

# --- beta malformed counter ignored ------------------------------------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-beta.4; tag "$r" v0.1.0-beta.x
expect_eq "beta ignores non-numeric .x -> v0.1.0-beta.5" "$(run "$r" beta 0.1.0)" "v0.1.0-beta.5"

# --- alpha: perf: since last stable -> patch base ----------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "perf: faster"
expect_eq "alpha perf -> v0.1.1-alpha.1" "$(run "$r" alpha)" "v0.1.1-alpha.1"

# --- alpha: IN_BUMP=minor / =patch override paths ----------------------------
r="$(new_repo)"; commit "$r" "feat: base"; tag "$r" v0.1.0; commit "$r" "docs: x"
expect_eq "alpha IN_BUMP=minor -> v0.2.0-alpha.1" "$(IN_BUMP=minor run "$r" alpha)" "v0.2.0-alpha.1"
expect_eq "alpha IN_BUMP=patch -> v0.1.1-alpha.1" "$(IN_BUMP=patch run "$r" alpha)" "v0.1.1-alpha.1"

# --- alpha: invalid IN_BUMP -> non-zero exit ---------------------------------
r="$(new_repo)"; commit "$r" "feat: a"
if ( cd "$r" && IN_BUMP=hotfix "$RESOLVE" alpha ) >/dev/null 2>&1; then
  fail=$((fail + 1)); printf '  FAIL: invalid IN_BUMP (expected non-zero)\n'
else
  pass=$((pass + 1))
fi

# --- alpha: IN_VERSION + IN_BUMP both set -> IN_VERSION wins (documented) -----
r="$(new_repo)"; commit "$r" "feat: a"
expect_eq "alpha IN_VERSION beats IN_BUMP -> v0.2.0-alpha.1" \
  "$(IN_VERSION=v0.2.0 IN_BUMP=major run "$r" alpha)" "v0.2.0-alpha.1"

# --- released=true is emitted for alpha/beta/stable real bumps ----------------
r="$(new_repo)"; commit "$r" "feat: a"
expect_eq "alpha emits released=true" "$(run_released "$r" alpha)" "true"
expect_eq "beta emits released=true" "$(run_released "$r" beta 0.1.0)" "true"
expect_eq "stable emits released=true" "$(run_released "$r" stable 0.1.0)" "true"

# --- GITHUB_OUTPUT side-effect: both keys written ----------------------------
r="$(new_repo)"; commit "$r" "feat: a"
gho="$(mktemp)"
( cd "$r" && GITHUB_OUTPUT="$gho" "$RESOLVE" alpha ) >/dev/null
grep -qx 'tag=v0.1.0-alpha.1' "$gho" && grep -qx 'released=true' "$gho" \
  && pass=$((pass + 1)) || { fail=$((fail + 1)); printf '  FAIL: GITHUB_OUTPUT keys not written as expected\n'; }

# --- stable: emits bare vX.Y.Z -----------------------------------------------
r="$(new_repo)"; commit "$r" "feat: a"; tag "$r" v0.1.0-beta.2
expect_eq "stable -> v0.1.0" "$(run "$r" stable 0.1.0)" "v0.1.0"

# --- error: invalid mode -----------------------------------------------------
r="$(new_repo)"; commit "$r" "feat: a"
expect_fail "invalid mode exits non-zero" "$r" bogus

# --- error: beta with malformed base -----------------------------------------
expect_fail "beta malformed base exits non-zero" "$r" beta "release/0.1.0"

# --- error: stable with no base ----------------------------------------------
expect_fail "stable missing base exits non-zero" "$r" stable

# --- error: alpha with malformed IN_VERSION ----------------------------------
if ( cd "$r" && IN_VERSION=0.1.0 "$RESOLVE" alpha ) >/dev/null 2>&1; then
  fail=$((fail + 1)); printf '  FAIL: alpha malformed IN_VERSION (expected non-zero)\n'
else
  pass=$((pass + 1))
fi

echo
echo "resolve-version: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
