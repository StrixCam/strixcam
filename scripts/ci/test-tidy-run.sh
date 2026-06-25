#!/usr/bin/env bash
# =============================================================================
# Tests for tidy-run.sh's file-selection / sharding logic (R7 / R-SELECT).
#
# A wrong selection FAILS OPEN — a TU that no shard picks up goes unlinted while
# the gate stays green. So the load-bearing invariants are asserted here:
#   * every TU is covered by exactly one shard (union == full, pairwise disjoint)
#   * the split is deterministic (same input -> same slices across runners)
#   * full mode lints the canonical list; _old/ is always excluded
#   * a changed TU absent from compile_commands.json FAILS, never skips silently
#
# The selection function is SOURCED and called directly over synthetic temp
# repos (no cmake/clang-tidy). The DB-membership case exercises the real main
# body with `cmake`/`clang-tidy` stubbed as no-ops on PATH — the same technique
# resolve-version-test.sh uses to stub `git`.
#
# Run: scripts/ci/test-tidy-run.sh   (exit 0 = all pass)
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIDY_RUN="${SCRIPT_DIR}/tidy-run.sh"

# Source the runner to get select_tidy_files; the main body is guarded by
# BASH_SOURCE==$0 so sourcing does NOT build or lint.
# shellcheck source=./tidy-run.sh disable=SC1091
source "${TIDY_RUN}"

pass=0
fail=0

expect_eq() { # label, got, want
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf '  FAIL: %s\n        got:  %s\n        want: %s\n' "$1" "$2" "$3"
  fi
}

expect_ok() { # label, actual-bool ("true"/"false")
  if [ "$2" = "true" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf '  FAIL: %s\n' "$1"
  fi
}

# Build a throwaway tree with a known TU layout and cd into it. Echoes the dir.
new_tree() {
  local d
  d="$(mktemp -d)"
  mkdir -p "$d/src/a" "$d/src/b" "$d/tests" "$d/src/_old"
  : >"$d/src/a/one.cpp"
  : >"$d/src/a/two.cc"
  : >"$d/src/b/three.cpp"
  : >"$d/tests/four.cpp"
  : >"$d/src/a/Zeta.cpp"   # uppercase: C-collation sorts it before lowercase,
                           # a UTF-8 locale does not — exercises the sort pin
  : >"$d/src/a/header.hpp" # not a TU — must be ignored
  : >"$d/src/a/header.h"   # not a TU — must be ignored
  : >"$d/src/_old/legacy.cpp" # deprecated tree — must be excluded
  echo "$d"
}

# The expected list in the script's own collation (LC_ALL=C) — the round-robin
# assigns by index in THIS order, so every comparison below must use it too.
t="$(new_tree)"
want="$(printf '%s\n' src/a/Zeta.cpp src/a/one.cpp src/a/two.cc src/b/three.cpp tests/four.cpp | LC_ALL=C sort)"

# --- full mode: the 5 real TUs, C-sorted, no headers, no _old/ ----------------
got="$(cd "$t" && select_tidy_files full)"
expect_eq "full -> 5 TUs C-sorted, headers + _old excluded" "$got" "$want"

# --- ordering is locale-stable: the script pins `LC_ALL=C sort`, so the TU
# --- order (and thus every shard's bin assignment) is identical regardless of
# --- the ambient locale. Cross-runner shard agreement (R7) rests on this — a
# --- regression dropping the pin would let two runners under different locales
# --- bin a TU differently, opening a silent coverage hole.
got_utf="$(cd "$t" && LC_ALL=en_US.UTF-8 select_tidy_files full)"
expect_eq "full order is C-collated regardless of ambient locale" "$got_utf" "$want"

# --- shard union == full, for several shard counts (C-collated union) ---------
for n in 1 2 3 4 8; do
  union="$(cd "$t" && for ((i = 0; i < n; i++)); do select_tidy_files shard "$i" "$n"; done | LC_ALL=C sort)"
  expect_eq "shard union(n=$n) == full" "$union" "$want"
done

# --- shards pairwise disjoint (no TU double-linted) ---------------------------
dups="$(cd "$t" && for i in 0 1 2; do select_tidy_files shard "$i" 3; done | LC_ALL=C sort | uniq -d)"
expect_eq "shards disjoint (n=3): no duplicates" "$dups" ""

# --- determinism: same slice twice -> identical -------------------------------
a="$(cd "$t" && select_tidy_files shard 1 3)"
b="$(cd "$t" && select_tidy_files shard 1 3)"
expect_eq "shard split deterministic" "$a" "$b"

# --- total=1 collapses to full ------------------------------------------------
got="$(cd "$t" && select_tidy_files shard 0 1)"
expect_eq "shard 0/1 == full" "$got" "$want"

# --- bad args rejected (fail-open guard must be loud, not silent) -------------
if ( cd "$t" && select_tidy_files shard 3 3 ) >/dev/null 2>&1; then
  expect_ok "shard index==total rejected" "false"
else
  expect_ok "shard index==total rejected" "true"
fi

if ( cd "$t" && select_tidy_files shard x 3 ) >/dev/null 2>&1; then
  expect_ok "non-integer shard index rejected" "false"
else
  expect_ok "non-integer shard index rejected" "true"
fi

if ( cd "$t" && select_tidy_files bogus ) >/dev/null 2>&1; then
  expect_ok "unknown mode rejected" "false"
else
  expect_ok "unknown mode rejected" "true"
fi

# --- DB-membership: a TU absent from compile_commands.json FAILS (finding D) ---
# Run the real main body with cmake/clang-tidy stubbed as no-ops so the build is
# skipped and our hand-written compile_commands.json survives.
stubdir="$(mktemp -d)"
cat >"$stubdir/cmake" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
cat >"$stubdir/clang-tidy" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
chmod +x "$stubdir/cmake" "$stubdir/clang-tidy"

db_tree() { # echoes a fresh tree with one or both TUs registered in the DB
  local d
  d="$(mktemp -d)"
  mkdir -p "$d/src" "$d/tests" "$d/build/test"
  : >"$d/src/foo.cpp"
  : >"$d/src/bar.cpp"
  echo "$d"
}

# Only foo.cpp in the DB; bar.cpp is in the tree but unwired -> must FAIL.
d="$(db_tree)"
cat >"$d/build/test/compile_commands.json" <<JSON
[{"directory":"$d","command":"c++ -c src/foo.cpp","file":"$d/src/foo.cpp"}]
JSON
if ( cd "$d" && PATH="$stubdir:$PATH" "$TIDY_RUN" full ) >/dev/null 2>&1; then
  expect_ok "unwired TU absent from compile DB -> FAIL" "false"
else
  expect_ok "unwired TU absent from compile DB -> FAIL" "true"
fi

# Both TUs in the DB -> passes (clang-tidy stubbed to no-op).
d="$(db_tree)"
cat >"$d/build/test/compile_commands.json" <<JSON
[{"directory":"$d","command":"c++ -c src/foo.cpp","file":"$d/src/foo.cpp"},
 {"directory":"$d","command":"c++ -c src/bar.cpp","file":"$d/src/bar.cpp"}]
JSON
if ( cd "$d" && PATH="$stubdir:$PATH" "$TIDY_RUN" full ) >/dev/null 2>&1; then
  expect_ok "all TUs in compile DB -> pass" "true"
else
  expect_ok "all TUs in compile DB -> pass" "false"
fi

# -----------------------------------------------------------------------------
printf '\ntidy-run selection self-test: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
