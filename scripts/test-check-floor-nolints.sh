#!/usr/bin/env bash
# Self-contained test for scripts/check-floor-nolints.sh. Builds throwaway git
# repos, adds a feature commit containing one NOLINT form, runs the guard against
# the base, and asserts the expected pass/fail. Covers the bypass classes a
# pattern-based gate must not let through (bare / wildcard / non-leading-list
# NOLINT, faked sanction marker) plus the legitimate-pass cases.
#
# Run locally inside or outside the devcontainer:  scripts/test-check-floor-nolints.sh
# Exits non-zero if any case regresses.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUARD="$SCRIPT_DIR/check-floor-nolints.sh"
[ -f "$GUARD" ] || { echo "guard not found: $GUARD" >&2; exit 2; }

failures=0

# run_case <name> <expected: pass|fail> <added-source-line>
run_case() {
    local name="$1" expected="$2" added="$3"
    local repo
    repo="$(mktemp -d)"
    (
        cd "$repo" || exit 2
        git init -q
        git config user.email t@t; git config user.name t
        git checkout -q -b main
        printf 'int base() { return 0; }\n' >a.cpp
        git add -A && git commit -qm base
        git checkout -q -b feat
        printf 'int base() { return 0; }\n%s\n' "$added" >a.cpp
        git add -A && git commit -qm feat
        bash "$GUARD" main >/dev/null 2>&1
    )
    local rc=$?
    rm -rf "$repo"
    local got="pass"; [ "$rc" -ne 0 ] && got="fail"
    if [ "$got" = "$expected" ]; then
        printf '  ok   %-44s (%s)\n' "$name" "$expected"
    else
        printf '  FAIL %-44s expected %s, got %s\n' "$name" "$expected" "$got" >&2
        failures=$((failures + 1))
    fi
}

echo "check-floor-nolints.sh test cases:"
# --- must FAIL (unsanctioned floor suppression in some form) ---
run_case "bare NOLINT"          fail 'void s(int a, int b) {}  // NOLINT'
run_case "bare NOLINTNEXTLINE"  fail '// NOLINTNEXTLINE'
run_case "bare NOLINTBEGIN"     fail '// NOLINTBEGIN'
run_case "wildcard NOLINT(*)"   fail 'void s(int a, int b) {}  // NOLINT(*)'
run_case "explicit floor"       fail 'void s(int a, int b) {}  // NOLINT(bugprone-easily-swappable-parameters)'
run_case "performance floor"    fail 'auto f() { return 1; }  // NOLINT(performance-unnecessary-value-param)'
run_case "non-leading in list"  fail 'void s(int a, int b) {}  // NOLINT(readability-magic-numbers,bugprone-easily-swappable-parameters)'
run_case "fake marker pre-NOLINT" fail 'const char* k = "floor-ok: nope";  // NOLINT(bugprone-easily-swappable-parameters)'

# --- must PASS (sanctioned, or not a floor suppression) ---
run_case "sanctioned floor"     pass 'void s(int a, int b) {}  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok: external order'
run_case "sanctioned in list"   pass 'void s(int a, int b) {}  // NOLINT(readability-magic-numbers,bugprone-easily-swappable-parameters) // floor-ok: reason'
run_case "named non-floor check" pass 'int x = 42;  // NOLINT(readability-magic-numbers)'
run_case "no nolint at all"     pass 'int y() { return 7; }'

if [ "$failures" -gt 0 ]; then
    echo "FAILED: $failures case(s) regressed" >&2
    exit 1
fi
echo "all cases passed"
