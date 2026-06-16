#!/usr/bin/env bash
# Dev-side auto-fix: apply clang-format + the fixit-bearing subset of clang-tidy
# BEFORE pushing, so CI's `tidy` job stays verify-only (it never writes back —
# write-back breaks fork PRs and fights author history).
#
# MUST run inside the devcontainer (cross toolchain env). It reuses the exact
# cross flags CI uses via scripts/tidy-args.sh — one arg set, no drift.
#
# Usage:
#   scripts/fix.sh            # staged C/C++ files only (the pre-commit path)
#   scripts/fix.sh --all      # the whole src/ + tests/ tree (bulk cleanup)
#
# clang-tidy --fix only auto-corrects checks that ship fixits (e.g.
# modernize-use-nodiscard, trailing-return, many readability-*). The noisy
# blockers (magic-numbers, easily-swappable-parameters, cognitive-complexity,
# branch-clone) have NO fixit and are left reported for you to resolve by hand —
# this script never masks them.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# shellcheck source=scripts/tidy-args.sh
source scripts/tidy-args.sh

BUILD_DIR="build/test"
if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
  echo "fix.sh: configuring the test preset (no compile_commands.json yet)…" >&2
  cmake --preset test >/dev/null
fi

mode="${1:-staged}"

collect_files() {
  if [ "${mode}" = "--all" ]; then
    find src tests \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' -o -name '*.h' \) \
      -not -path '*/_old/*'
  else
    # Staged-only: a full-tree --fix would silently mutate unstaged dirty files.
    git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(cpp|cc|hpp|h)$' || true
  fi
}

mapfile -t all_files < <(collect_files)
if [ "${#all_files[@]}" -eq 0 ]; then
  echo "fix.sh: no C/C++ files to fix." >&2
  exit 0
fi

# clang-format handles every header + TU; clang-tidy --fix only the TUs that
# appear in compile_commands.json (.cpp/.cc).
tu_files=()
for f in "${all_files[@]}"; do
  case "${f}" in
    *.cpp|*.cc) tu_files+=("${f}") ;;
  esac
done

echo "fix.sh: clang-format -i on ${#all_files[@]} file(s)…"
clang-format -i "${all_files[@]}"

if [ "${#tu_files[@]}" -gt 0 ]; then
  echo "fix.sh: clang-tidy-14 --fix on ${#tu_files[@]} translation unit(s)…"
  clang-tidy-14 -p "${BUILD_DIR}" --fix --fix-errors "${TIDY_EXTRA_ARGS[@]}" "${tu_files[@]}" || true
fi

echo "fix.sh: done. Unfixable findings (if any) remain reported — resolve them by hand."
