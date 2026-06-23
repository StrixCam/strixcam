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
  # Don't let a configure failure (e.g. Conan offline on a fresh clone) abort the
  # whole script — that would block a commit via the pre-commit hook. Degrade to
  # clang-format-only and warn; the dev can still commit (CI remains the gate).
  if ! cmake --preset test >/dev/null 2>&1; then
    echo "fix.sh: WARNING — cmake configure failed (network/Conan offline?);" \
         "running clang-format only, skipping clang-tidy --fix." >&2
  fi
fi
tidy_ready=0
[ -f "${BUILD_DIR}/compile_commands.json" ] && tidy_ready=1

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

if [ "${tidy_ready}" -eq 1 ] && [ "${#tu_files[@]}" -gt 0 ]; then
  echo "fix.sh: clang-tidy --fix on ${#tu_files[@]} translation unit(s)…"
  # A non-zero exit is EXPECTED when unfixable findings remain (magic-numbers,
  # cognitive-complexity, …) — that's not a script failure. But don't swallow it
  # silently: a tool error (stale compile_commands, clang crash) looks the same,
  # so surface the exit code and let the dev judge from the output above.
  if ! clang-tidy -p "${BUILD_DIR}" --fix --fix-errors "${TIDY_EXTRA_ARGS[@]}" "${tu_files[@]}"; then
    echo "fix.sh: clang-tidy exited non-zero — expected if unfixable findings" \
         "remain; if you see TOOL errors above, the fix pass may be incomplete." >&2
  fi
elif [ "${tidy_ready}" -eq 0 ]; then
  echo "fix.sh: skipping clang-tidy --fix (no compile_commands.json)." >&2
fi

echo "fix.sh: done. Unfixable findings (if any) remain reported — resolve them by hand."
