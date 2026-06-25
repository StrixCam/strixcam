#!/usr/bin/env bash
# =============================================================================
# tidy-run.sh — the single source of truth for the CI `tidy` gate's build +
# file-selection + clang-tidy invocation, shared byte-for-byte by
# release-alpha.yml and release-beta.yml (R8 / R-DRIFT — one script, no drift).
#
# clang-tidy cost is dominated by TU COUNT: each of the ~95 translation units
# re-parses the heavy GStreamer/OpenCV headers, so a single-job full scan is
# ~21 min regardless of PR size. We keep FULL-TREE coverage (no diff-scope, no
# coverage hole, "green = clean tree" stays true) and instead SHARD the TU list
# across a runner matrix: each shard builds once, then lints its slice. N shards
# cut the lint wall-time by ~N while every TU is still linted on every PR.
#
# Usage (positional args, matching scripts/ci/resolve-version.sh):
#   tidy-run.sh full                 # lint every TU (today's exact list)
#   tidy-run.sh shard <index> <n>    # lint the index-th of n round-robin slices
#                                    #   index in [0, n), n >= 1
#
# The file-selection logic is a SOURCED function `select_tidy_files` that touches
# neither cmake nor clang-tidy, so the self-test (scripts/ci/test-tidy-run.sh)
# can call it directly over a synthetic repo — no production-only "dry-run"
# branch (finding J). Sourcing this script does NOT run the build (the main body
# is guarded by the BASH_SOURCE==$0 check at the bottom).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The canonical TU list: src/ + tests/ .cpp/.cc, excluding the deprecated _old/
# Python-prototype tree. The round-robin split assigns each TU to a shard by its
# INDEX in this sorted list, so every shard runner MUST compute a byte-identical
# order — `LC_ALL=C sort` pins byte-collation so the order is locale-independent
# (a UTF-8 vs C collation skew on one leg would silently move a TU into a
# different, or no, bin and the gate would go green with a coverage hole).
_all_tidy_tus() {
  find src tests \( -name '*.cpp' -o -name '*.cc' \) -not -path '*/_old/*' | LC_ALL=C sort
}

# select_tidy_files <mode> [index] [n]  -> newline-separated TU list on stdout.
# Pure selection: no cmake, no clang-tidy, no I/O beyond `find`. Round-robin
# (NR-mod-n) slicing guarantees the union of all n shards is the full list and
# the shards are pairwise disjoint — asserted by the self-test.
select_tidy_files() {
  local mode="${1:-}"
  case "$mode" in
  full)
    _all_tidy_tus
    ;;
  shard)
    local index="${2:-}" total="${3:-}"
    if ! [[ "$index" =~ ^[0-9]+$ && "$total" =~ ^[0-9]+$ ]]; then
      echo "tidy-run: shard needs integer <index> <total>, got '$index' '$total'" >&2
      return 2
    fi
    if [ "$total" -lt 1 ] || [ "$index" -ge "$total" ]; then
      echo "tidy-run: shard index $index out of range for total $total" >&2
      return 2
    fi
    # NF guard skips the lone empty line `find` yields on an empty tree; the
    # per-TU counter `n` advances only on real entries so the slice is even.
    _all_tidy_tus | awk -v idx="$index" -v tot="$total" 'NF { if ((n++ % tot) == idx) print }'
    ;;
  *)
    echo "tidy-run: unknown mode '${mode:-<none>}' (want: full | shard <index> <n>)" >&2
    return 2
    ;;
  esac
}

main() {
  local mode="${1:-}"
  if [ -z "$mode" ]; then
    echo "usage: tidy-run.sh full | tidy-run.sh shard <index> <n>" >&2
    exit 2
  fi

  # Select FIRST — it is cheap (`find` only) and validates the shard args, so a
  # bad invocation fails before the expensive build rather than being masked by
  # `set -e` in the assignment and slipping through as an empty (benign) shard.
  local files
  if ! files="$(select_tidy_files "$@")"; then
    echo "tidy-run: file selection failed for args: $*" >&2
    exit 2
  fi

  # An empty selection needs no build: a full scan with zero TUs is a degenerate
  # tree (real breakage); an empty SHARD just means another shard owns the work.
  if [ -z "$files" ]; then
    if [ "$mode" = "full" ]; then
      echo "tidy-run: no C++ translation units found to tidy" >&2
      exit 1
    fi
    echo "tidy-run: shard has no TUs to lint (covered by other shards)"
    exit 0
  fi

  # Build is UNCONDITIONAL and identical for every mode/shard: configure the test
  # preset to emit compile_commands.json, then generate the protobuf headers
  # (sst_proto = protoc only, seconds) so every TU that includes *.pb.h has them
  # and clang-tidy does not cascade phantom findings off a broken AST.
  cmake --preset test
  cmake --build --preset test --target sst_proto

  # DB-membership guard (R-NEWFILE / finding D): a selected TU that exists in the
  # working tree but is ABSENT from compile_commands.json is a wiring hole (a new
  # .cpp not yet added to a CMake target) — fail loudly, never silently skip a TU
  # the gate is supposed to cover. A file gone from the tree (deleted/renamed
  # away but still in an old diff) is simply dropped.
  local db="build/test/compile_commands.json"
  local final=() missing=()
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    [ -e "$f" ] || continue
    if grep -Fq -- "/$f\"" "$db" 2>/dev/null; then
      final+=("$f")
    else
      missing+=("$f")
    fi
  done <<<"$files"

  if [ "${#missing[@]}" -gt 0 ]; then
    {
      echo "tidy-run: ${#missing[@]} changed TU(s) present in the tree but ABSENT from"
      echo "          ${db} — not wired into a CMake target. Refusing to skip them:"
      printf '            %s\n' "${missing[@]}"
    } >&2
    exit 1
  fi

  if [ "${#final[@]}" -eq 0 ]; then
    # A full scan with zero TUs is a degenerate tree (a real breakage); an empty
    # SHARD just means another shard owns the work — benign.
    if [ "$mode" = "full" ]; then
      echo "tidy-run: no C++ translation units found to tidy" >&2
      exit 1
    fi
    echo "tidy-run: shard has no TUs to lint (covered by other shards)"
    exit 0
  fi

  echo "tidy-run: linting ${#final[@]} TU(s) [mode=${mode}]"
  # Cross-toolchain flags: scripts/tidy-args.sh (shared with scripts/fix.sh).
  # Sourced via SCRIPT_DIR so it resolves regardless of cwd.
  # shellcheck source=../tidy-args.sh disable=SC1091
  source "${SCRIPT_DIR}/../tidy-args.sh"
  printf '%s\n' "${final[@]}" | xargs clang-tidy -p build/test "${TIDY_EXTRA_ARGS[@]}"
}

# Run the build/lint pipeline only when executed, not when sourced (the self-test
# sources this file to call select_tidy_files directly).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
