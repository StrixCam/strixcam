---
title: "A clang-tidy hard gate can read 0-warnings-clean while silently under-enforcing"
date: 2026-06-16
category: tooling-decisions
module: ci-cd
problem_type: tooling_decision
component: tooling
severity: medium
applies_when:
  - "Promoting clang-tidy (or any linter) from advisory to a hard, merge-blocking gate"
  - "Editing .clang-tidy Checks, WarningsAsErrors, or HeaderFilterRegex"
  - "Cross-compiling where the build compiler (gcc) differs from the analyzer (clang-tidy)"
  - "Adopting a .clang-tidy copied from another repo, or adding a new top-level source dir"
tags:
  - clang-tidy
  - yaml-gotcha
  - ci-gate
  - cross-compile
  - header-filter
  - narrowing-error
---

# A clang-tidy hard gate can read 0-warnings-clean while silently under-enforcing

## Context

Making clang-tidy a hard CI gate in sst-cam-firmware (C++20, Jetson aarch64
cross-build): host x86 `clang-tidy-14` parses cross TUs via `scripts/tidy-args.sh`,
`.clang-tidy` sets `WarningsAsErrors: '*'`, and the `ci.yml` `tidy` job runs under
`set -euo pipefail` with no `continue-on-error`. The target is built with **gcc**;
clang-tidy is a separate **clang** front end used only for analysis.

The gate first read "clean" — exit 0, zero warnings. It was under-enforcing in three
distinct ways, each invisible to a casual 0-count check. Only flipping
`WarningsAsErrors: '*'` and verifying adversarially exposed them. See
[ci-cd-release-pipeline](./ci-cd-release-pipeline-2026-06-15.md) for the prerequisite
cross-sysroot setup that made the gate viable at all (moderate overlap — that doc is
"how the gate was made to run"; this one is "why a running gate can still be hollow").

## Guidance

### Trap 1 — a YAML block scalar swallows inline `#` comments

**Before** (`.clang-tidy`):

```yaml
Checks: >
  -*,
  bugprone-*,
  modernize-*,
  google-*      # Google style checks
```

A `>` (folded) or `|` (literal) block scalar treats **every** indented line as part of
the string value. The `#` is *not* a comment delimiter inside a block scalar, so the
effective `Checks` string becomes `…google-*      # Google style checks` — a malformed
trailing glob token. clang-tidy silently fails to enable `google-build-using-namespace`,
`google-runtime-int`, and `google-readability-casting`. The gate reported 0 findings for
checks it never ran. Removing the inline comment surfaced **38 real findings**.

**After** — keep comments out of the scalar, or use a plain string:

```yaml
# Google style checks (google-build-using-namespace, google-runtime-int, …)
Checks: >-
  -*,
  bugprone-*,
  modernize-*,
  google-*
```

Verify the *effective* enabled set rather than trusting the file:

```bash
clang-tidy-14 --list-checks | grep -E '^\s+google-'   # reads .clang-tidy from CWD
# empty output for a family you expect = the Checks glob is corrupted
```

### Trap 2 — narrowing is an `error:`, and gcc-clean ≠ clang-tidy-clean

A narrowing conversion in a braced-init list is C++11 ill-formed. clang-tidy emits it as:

```
error: non-constant-expression cannot be narrowed from type 'unsigned long' to
'std::uint32_t' ... [clang-diagnostic-c++11-narrowing]
```

Note `error:`, not `warning:`. The **gcc** cross-build only issued `-Wnarrowing` and
still produced a binary, so `cmake --build` + `ctest` were green. A verification that
counted only `grep -c "warning:"` (or only `grep -c "clang-diagnostic-error"`) missed
it — the line is an `error:` tagged `[clang-diagnostic-c++11-narrowing]`.

**Before:**

```cpp
constexpr std::size_t kBgrChannels = 3;
// dims.width is std::uint32_t → uint32 * size_t = size_t, narrowed to uint32 stride
frame.planes.push_back(FramePlane{ .stride = dims.width * kBgrChannels, ... });
```

**After** — make the conversion explicit, and fix verification to cover both classes:

```cpp
frame.planes.push_back(FramePlane{
    .stride = static_cast<std::uint32_t>(dims.width * kBgrChannels), ... });
```

```bash
# Wrong — misses clang-diagnostic errors:
grep -c "warning:" tidy.log
# Right — count both, but prefer the real exit code:
grep -cE ': (warning|error):' tidy.log
echo "$files" | xargs clang-tidy-14 -p build/test "${TIDY_EXTRA_ARGS[@]}"; echo "exit=$?"
```

### Trap 3 — `HeaderFilterRegex` silently exempts whole directories

```yaml
HeaderFilterRegex: 'src/.*'
```

clang-tidy drops any diagnostic whose source location is outside the regex **before**
`WarningsAsErrors` can promote it. A shared test header
(`tests/processing/synthetic_frames.hpp`) carried FLOOR `bugprone-easily-swappable-parameters`
and `bugprone-implicit-widening` violations that never reached the gate. The tree read
"0 under the enforced set," but an entire directory of headers was unchecked.

**After** — scope the regex to every gated directory, and document it as scope, not display:

```yaml
# Gate scope: diagnostics in headers OUTSIDE this regex are silently dropped.
# Expand when a new top-level dir with headers is added.
HeaderFilterRegex: '(src|tests)/.*'
```

## Why This Matters

A hard gate that reads 0 while under-enforcing is worse than no gate — it manufactures
false confidence and teams stop reading static-analysis output because "CI is green."
Here, 38 findings hid behind a corrupted check glob, a narrowing error was invisible to
the gcc build that produced the shipped binary, and a whole header directory of
parameter-swap bugs sat outside the enforced scope — all while every CI run reported
clean. None of these are clang-tidy bugs: block scalars are legal YAML, `clang-diagnostic-*`
is a distinct class from check warnings, and `HeaderFilterRegex` is documented as a
filter but behaves as the gate boundary. They are correct behaviors that are easy to
misread under time pressure.

## When to Apply

- Any time a linter goes from advisory to merge-blocking.
- After editing `.clang-tidy` `Checks`, `WarningsAsErrors`, or `HeaderFilterRegex`.
- After adding a new top-level source/header directory to a repo with a gated tidy job.
- Cross-compile projects where build (gcc) and analysis (clang-tidy) have independent
  diagnostic models.
- Onboarding a `.clang-tidy` copied from another project.

## Examples

**Adversarial verification protocol — run all four before trusting the gate:**

```bash
# 1. Effective check set (config strings corrupt silently)
clang-tidy-14 --list-checks | grep -E '^\s+(google|bugprone|performance)-' | head

# 2. Grep errors AND warnings; the exit code is authoritative
echo "$files" | xargs clang-tidy-14 -p build/test "${TIDY_EXTRA_ARGS[@]}" > tidy.log 2>&1
echo "exit=$?  err=$(grep -c 'error:' tidy.log)  warn=$(grep -c 'warning:' tidy.log)"

# 3. Audit HeaderFilterRegex against the real tree
find src tests \( -name '*.hpp' -o -name '*.h' \) | sed 's|/[^/]*$||' | sort -u

# 4. Seed a deliberate violation; confirm non-zero exit; then revert
printf '\nnamespace{int q=987654;int f(){return q;}}\n' >> some_tu.cpp
echo some_tu.cpp | xargs clang-tidy-14 -p build/test "${TIDY_EXTRA_ARGS[@]}"; echo "exit=$?"
git checkout -- some_tu.cpp
```

## Related

- [ci-cd-release-pipeline](./ci-cd-release-pipeline-2026-06-15.md) — prerequisite:
  cross-sysroot toolchain flags that made the gate runnable (its "tidy is advisory"
  note is now superseded; tidy is a hard gate).
- `docs/plans/2026-06-15-001-fix-clang-tidy-hard-gate-plan.md` — implementation
  (WarningsAsErrors mechanism, the `src/.*` → `(src|tests)/.*` expansion, the floor policy).
- `docs/bugs/2026-06-16-swappable-params-floor-nolint.md` — a downstream consequence:
  the FLOOR `bugprone-easily-swappable-parameters` check `// NOLINT`-suppressed on
  genuinely transposable production signatures.
