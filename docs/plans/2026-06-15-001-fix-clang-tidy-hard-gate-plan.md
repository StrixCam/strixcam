---
title: "fix: Make clang-tidy a hard required gate"
type: fix
status: active
date: 2026-06-15
origin: docs/brainstorms/2026-06-15-clang-tidy-hard-gate-requirements.md
---

# fix: Make clang-tidy a hard required gate

## Summary

Fix clang-tidy's C++ standard-library header discovery in the devcontainer `tidy` job (currently 45 `clang-diagnostic-error: file not found`, exit 123), then drive the resulting 644 warnings to zero, promote the check set to `WarningsAsErrors`, drop `continue-on-error`, and add `tidy` to the protected-branch required checks — turning an advisory-red job into a hard gate with zero lint debt. Add a dev-side auto-fix path (`scripts/fix.sh` + pre-commit) so fixable findings are corrected before push and CI stays verify-only, and cache the devcontainer image in GHCR to cut the ~13-min rebuild every CI run pays today. All work stays on the cross-compilation toolchain — the repo has no native app preset and none is introduced.

---

## Problem Frame

The `tidy` job is the last advisory-red gap in an otherwise-green CI/CD pipeline (see origin: `docs/brainstorms/2026-06-15-clang-tidy-hard-gate-requirements.md`). It fails because host clang-tidy, parsing aarch64 cross translation units, cannot locate libstdc++ headers — not because of any real code defect.

**Root cause:** the 45 `clang-diagnostic-error` + exit 123 are confirmed from **CI run 27565494770**; the toolchain *layout* below was confirmed by running the cross **g++** inside the live devcontainer. (Note: the running `sst-cam-firmware_devcontainer-app-1` container was built 2026-06-04, before the Dockerfile's clang-tidy/clang-format install was added 2026-06-11 — so `clang-tidy` is **absent** there today and the container must be rebuilt before it can verify anything. See U1's pre-step.)

- The Bootlin toolchain triple is **`aarch64-buildroot-linux-gnu`**, but the CI step passes `--target=aarch64-linux-gnu`. clang's `--gcc-toolchain` auto-detection looks for a gcc install under the *target triple* — the mismatch means it never registers the buildroot gcc, so libstdc++ is invisible.
- libstdc++ headers live in the **toolchain tree**, not in `--sysroot=/l4t/targetfs` (which only carries system libs like GStreamer/OpenCV). The current flags point only at the targetfs sysroot, so `<cstdint>`, `<string>`, `<vector>`, etc. can never resolve.
- The CI step's fallback toolchain path `/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin` is also wrong (real path has no `/toolchain/` segment) — latent bug if the env var ever stops being set.

Cross g++ reports its true include search list (version 11.3.0):

    /l4t/aarch64--glibc--stable-2022.08-1/aarch64-buildroot-linux-gnu/include/c++/11.3.0
    /l4t/aarch64--glibc--stable-2022.08-1/aarch64-buildroot-linux-gnu/include/c++/11.3.0/aarch64-buildroot-linux-gnu
    /l4t/aarch64--glibc--stable-2022.08-1/aarch64-buildroot-linux-gnu/include/c++/11.3.0/backward
    /l4t/aarch64--glibc--stable-2022.08-1/lib/gcc/aarch64-buildroot-linux-gnu/11.3.0/include
    /l4t/aarch64--glibc--stable-2022.08-1/lib/gcc/aarch64-buildroot-linux-gnu/11.3.0/include-fixed
    /l4t/aarch64--glibc--stable-2022.08-1/aarch64-buildroot-linux-gnu/include
    /l4t/aarch64--glibc--stable-2022.08-1/aarch64-buildroot-linux-gnu/sysroot/usr/include

---

## Requirements

- R1. `tidy` job emits **0** `clang-diagnostic-error` (no `file not found`) on a clean tree.
- R2. All clang-tidy warnings resolved or deliberately, recorded-suppressed across `src/` + `tests/`. **The 644 (190 src / 454 tests) is a provisional floor measured over the broken parse** — once U1 lets every TU parse fully the count will move (likely up, as downstream code + headers become analyzable). Re-baseline after U1; the target is "zero under the enforced set," not "fix exactly 644."
- R3. `tidy` runs **without** `continue-on-error` and enforces the check set via `WarningsAsErrors`.
- R4. A PR introducing a new lint violation **fails** `tidy`.
- R5. `tidy` is a required status check on `main` (alongside `format`, `test`).
- R6. `format` and `test` gates remain unaffected.
- R7. The fix lives in the existing devcontainer (cross) job — no native host preset, no self-hosted runner. All checks remain cross-only (verified: no native preset exists).
- R8. Auto-fixable findings are corrected **dev-side** before push (`scripts/fix.sh` + pre-commit); CI is verify-only and never writes back to the branch (fork-PR safe).
- R9. CI no longer rebuilds the devcontainer from scratch every run — the image is cached in GHCR and reused by `format`/`tidy`/`test` (~13 min → ~5 min target).

**Origin acceptance criteria:** R1–R6 comprehensively carry the origin success-criteria checklist (not a strict 1:1 — origin's "no continue-on-error + required check" criterion splits into R3 and R5). R7–R9 added during planning from user direction.

---

## Scope Boundaries

- No native host tidy preset (rejected in brainstorm in favour of cross-toolchain). No native app preset introduced — the repo is cross-only and stays that way.
- No self-hosted runner (public repos — fork code on home hardware is a security no-go). This is also why GHCR caching (U7), not a warm runner, is the run-time fix.
- No CI auto-commit / write-back of fixes to PR branches (breaks fork PRs; auto-fix is dev-side instead — U6).
- No CI/CD changes to sst-cam-app, sst-cam-proto, or sst-cam-emulator.
- No change to the `.clang-tidy` *check selection* beyond what R2/R3 triage requires (we tune, not redesign).

---

## Context & Research

### Relevant Code and Patterns

- `.github/workflows/ci.yml` — the `tidy` job (lines ~84-151): `continue-on-error: true`, `devcontainers/ci@v0.3`, `cmake --preset test` → `compile_commands.json`, `xargs clang-tidy -p build/test` with `--target` / `--sysroot` / `--gcc-toolchain` extra-args. The sibling `format` and `test` jobs show the same devcontainer-run pattern to mirror.
- `.clang-tidy` — `Checks: -*,bugprone-*,performance-*,modernize-*,readability-*,google-*`; `WarningsAsErrors: ''`; `HeaderFilterRegex: 'src/.*'`; `FormatStyle: google`.
- `.devcontainer/Dockerfile` — sets `BOOTLIN_TOOLCHAIN_BIN=/l4t/aarch64--glibc--stable-2022.08-1/bin`, `CROSS_SYSROOT=/l4t/targetfs`.
- CMake presets: only the cross `test` preset exists (generates `compile_commands.json` under `build/test/`).

### Institutional Learnings

- `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` — the CI/CD release-pipeline decisions; this plan completes the one deferred gate from it.

### External References

- None needed — the fix is fully determined by the local toolchain layout (introspected above).

---

## Key Technical Decisions

- **Try matched-triple + `--gcc-toolchain` FIRST; g++-derived `-isystem` is the fallback.** (Reversed from an earlier draft on review.) Pass `--target=aarch64-buildroot-linux-gnu --gcc-toolchain=/l4t/aarch64--glibc--stable-2022.08-1` — clang is *designed* to auto-detect exactly this buildroot layout (`bin/../lib/gcc/<triple>/<ver>` + `<triple>/include/c++/<ver>`, both confirmed present), and it keeps **clang's own builtin/resource dir** in the search path. Only if that fails, fall back to injecting g++-derived dirs via `--extra-arg-before=-isystem<dir>` — and when injecting, **exclude gcc's `include` and `include-fixed` builtin dirs** (they carry gcc intrinsics like `arm_neon.h` + a conflicting `<stdint.h>` that collide with clang-14's builtins → `__builtin`/redefinition errors). Inject only the libstdc++ C++ dirs + target libc dirs, preserving g++'s emission order.
- **Keep `--sysroot=$CROSS_SYSROOT` (targetfs).** Targetfs supplies GStreamer/OpenCV/system headers; the toolchain tree supplies libc/libstdc++.
- **Falsification test, not just "no file-not-found":** a TU including `<cstdint>` and one including `<arm_neon.h>` must parse with **zero** redefinition / `__builtin` / intrinsic-mismatch warnings — proving clang's and gcc's headers aren't fighting, not merely that headers were found.
- **Pin `clang-tidy-14`.** The devcontainer's apt `clang-tidy` resolves to 14 (jammy). The 644 baseline *and* the enforced check set are version-locked — a silent bump to 15+ adds checks and breaks the gate. Pin `clang-tidy-14` in the Dockerfile and call `clang-tidy-14` explicitly in CI + `scripts/fix.sh`; treat a version bump as a deliberate, re-triaged change.
- **Strict-everywhere with a non-negotiable floor (R2/R3).** Enforce via `WarningsAsErrors`, but "tune out" has a hard floor so the gate can't be silently hollowed: **`bugprone-*` and `performance-*` may NOT be removed from the enforced set** (they catch real defect classes). For those, fix the code or use a justified per-site `// NOLINT` — never a blanket removal. Only *stylistic/readability* checks that fire on idiomatic code (`readability-magic-numbers`, `readability-identifier-length`, `readability-function-cognitive-complexity`) may be relaxed, and for test code that relaxation lives in a `tests/.clang-tidy`, not a global drop. Mechanism matters: a check is "tuned out" by **removing it from `Checks`** (so it neither warns nor errors) — `WarningsAsErrors` only promotes already-emitted diagnostics, so leaving a check in `Checks` while dropping it from `WarningsAsErrors` still emits a warning. Record the final `Checks` + `WarningsAsErrors` strings verbatim in `.clang-tidy` with rationale comments.
- **Auto-fix is dev-side, CI is verify-only (R8).** clang-tidy `--fix` only auto-corrects checks that ship fixits (`modernize-use-nodiscard`, trailing-return, many `readability-*`); the noisy blockers (`magic-numbers`, `easily-swappable-parameters`, `cognitive-complexity`, `branch-clone`) have **no** fixit and must error. So "auto-correct what's possible, block the rest" is clang-tidy's native model. Run the fixable half via `scripts/fix.sh` (`clang-format -i` + `clang-tidy --fix`) + a pre-commit hook, both **inside the devcontainer**. CI never auto-commits — write-back breaks fork PRs (public repo; `GITHUB_TOKEN` can't push to forks) and fights author history. *Alternative considered:* CI bot commits fixes back — rejected for the fork-PR + history reasons.
- **GHCR image cache, not a warm runner (R9) — measured before committed.** Publish the devcontainer image to `ghcr.io` on `.devcontainer/**` change; `format`/`tidy`/`test` consume it via `devcontainers/ci` `imageName` + `cacheFrom`. The "~5 min" figure is an **unvalidated target**, not a ceiling: a ~37 GB image *pull* on an ephemeral runner that already needs `jlumbroso/free-disk-space` to fit the *build* may ENOSPC on the *pull* instead, or not beat the current rebuild. So U7's **first acceptance step is a measurement** (real pull wall-time + peak disk on a hosted runner with free-disk applied) with an explicit rollback to per-run build if it regresses. Security hardening is mandatory, not optional — see U7 (digest-pin base + cache image, publish trigger `push: branches:[main]` only, per-job `packages: read`). Sub-minute would need a warm self-hosted runner, which is out (security).
- **Cross-only, confirmed (R7).** `CMakePresets.json` exposes only `base`-inheriting cross presets; there is no native app build. `CMakeLists.txt:131` (protoc-on-host) and `cmake/toolchains/jetson-r36.5.cmake:52` (qemu-user to run aarch64 tests on x86) are cross-compilation mechanics, not a native build. tidy binds to the cross `test` preset's `compile_commands.json`; nothing native to exclude.

---

## Open Questions

### Resolved During Planning

- *How to expose libstdc++ to clang-tidy?* → Inject g++-derived `-isystem` dirs + correct the target triple. (Key Technical Decisions.)
- *Native vs cross?* → Cross (origin decision).

### Resolved (post-review)

- *Can any check be tuned out to hit "zero"?* → **Only stylistic/readability checks, never `bugprone-*`/`performance-*`** (the floor). See Key Technical Decisions. `bugprone-easily-swappable-parameters` (58) is therefore fixed-or-`// NOLINT`-per-site, NOT removed from enforcement.
- *Is 644 the cleanup target?* → No. It's a provisional floor over the broken parse; re-baseline after U1 (R2).
- *Full test cleanup vs `tests/.clang-tidy` relax?* → **Full strict cleanup of `tests/`** (user decision), with `ctest` regression-guarding after each cluster. A `tests/.clang-tidy` relax of the idiomatic readability checks remains the documented fallback only if churn proves clearly low-value.

### Deferred to Implementation

- **Post-U1 warning re-baseline + the exact stylistic-check split.** After U1 lands, re-count warnings and record which *stylistic* checks (if any) are relaxed (and where: global vs `tests/.clang-tidy`). The `bugprone-*`/`performance-*` floor is fixed. Record the final `Checks`/`WarningsAsErrors` strings in `.clang-tidy`.
- **Whether the matched-triple `--gcc-toolchain` path works alone** (preferred) or the g++-derived `-isystem` fallback is needed — determined empirically in U1 against the falsification test.

---

## Implementation Units

### U1. Fix std-header discovery in the `tidy` job

**Goal:** clang-tidy parses all TUs with 0 `clang-diagnostic-error`; the post-fix warning count is re-baselined; the job is provably one config-line away from green (still under `continue-on-error` until U4).

**Requirements:** R1, R7

**Dependencies:** None

**Files:**
- Create: `scripts/tidy-args.sh` (shared arg-builder — emits the triple/`--gcc-toolchain`/`-isystem`/`--sysroot` flag set; single source of truth reused by CI and `scripts/fix.sh` in U6)
- Modify: `.github/workflows/ci.yml` (the `tidy` job's `clang-tidy` runCmd — source `scripts/tidy-args.sh` rather than inlining flags)
- Modify (pre-step): `.devcontainer/Dockerfile` if needed to pin `clang-tidy-14`

**Pre-step (blocking):** Rebuild the devcontainer image so `clang-tidy-14` is present (the live `app-1` predates the 2026-06-11 Dockerfile install) and confirm `clang-tidy-14 --version` resolves on PATH. Nothing below can be verified until this is done.

**Approach:**
- **Primary:** set `--target=aarch64-buildroot-linux-gnu --gcc-toolchain=/l4t/aarch64--glibc--stable-2022.08-1` and let clang auto-detect the buildroot layout (keeps clang's own builtin/resource dir). Keep `--sysroot=$CROSS_SYSROOT`, `-Qunused-arguments`.
- **Fallback (only if primary still errors):** inject g++-derived `-isystem` dirs via `--extra-arg-before=`, **excluding** gcc's `include`/`include-fixed` builtin dirs (clang-14 vs gcc-11.3 builtin conflict). See Key Technical Decisions.
- Put the whole flag set in `scripts/tidy-args.sh` so CI and the U6 fix script can't drift.
- Fix the wrong fallback path `/l4t/toolchain/...` → `/l4t/aarch64--glibc--stable-2022.08-1/bin` **in its own isolated commit** so a revert of the header-flag bet doesn't also revert this trivial defensive fix.
- After errors clear, **re-count warnings** (`src/` + `tests/`) and record the new baseline — it supersedes 644 for U2/U3 scoping.

**Execution note:** Verify inside the **rebuilt** devcontainer before pushing — `docker exec … clang-tidy-14 …`, never host clang. Iterate on one previously-erroring TU (`src/adapters/capture/frame/gstreamer/gstreamer.cpp`) until errors clear, then the full list.

**Patterns to follow:** existing `tidy` runCmd structure; sibling `format`/`test` jobs for devcontainer-run conventions.

**Test scenarios:**
- Happy path: clang-tidy on the full `src/`+`tests/` list in-container → 0 `clang-diagnostic-error`; only warnings remain.
- Falsification (not just "found"): a TU including `<cstdint>` and one including `<arm_neon.h>` parse with **zero** redefinition / `__builtin` / intrinsic-mismatch diagnostics — proves clang and gcc headers aren't colliding.
- Edge case: a header-only consumer (e.g. `src/adapters/control/ble/bluez/chunk-assembler.hpp`) resolves `<cstddef>`.
- Negative control: revert the triple to `aarch64-linux-gnu` → `file not found` returns (proves the triple is the lever).

**Verification:** In-container `clang-tidy-14` over all TUs exits 0 for diagnostic-errors; falsification TUs are clean; the new warning baseline is recorded; CI `tidy` no longer shows exit 123.

---

### U2. Drive `src/` warnings to zero (post-U1 baseline)

**Goal:** No clang-tidy warnings remain in `src/` under the enforced check set. Scope from the **post-U1 re-baselined** count, not the provisional 190.

**Requirements:** R2

**Dependencies:** U1, U6 (run the auto-fixable subset via `scripts/fix.sh` first to clear fixit-bearing checks mechanically, then hand-fix the rest)

**Files:**
- Modify: `src/**` (warning sites — concentrated in `src/adapters/control/ble/bluez/`, `src/adapters/control/wifi/`, `src/adapters/capture/`, `src/adapters/processing/`, `src/adapters/overlay/`, `src/app/control/services/handlers/`)
- Modify (possibly): `.clang-tidy` (record any *stylistic*-check relaxations with rationale comments)

**Approach:**
- Run `scripts/fix.sh` first (auto-applies fixits: `[[nodiscard]]`, trailing-return, many readability), then hand-fix the remainder.
- **Floor applies:** `bugprone-*`/`performance-*` are fixed or `// NOLINT`-per-site-with-justification, **never removed from enforcement** (`bugprone-easily-swappable-parameters`'s 58 hits included). Only stylistic/readability checks may be relaxed, recorded in `.clang-tidy`.
- Re-run in-container after each cluster.

**Patterns to follow:** existing constants/naming conventions in the touched modules; Google style.

**Test scenarios:**
- Regression: `test` preset still builds and `ctest` passes after each cluster (renames/constants must not change behavior). Covers R6.
- Happy path: clang-tidy over `src/` → 0 warnings under the enforced set.

**Verification:** In-container clang-tidy on `src/` emits 0 warnings; `cmake --build --preset test` green.

---

### U3. Drive `tests/` warnings to zero (full cleanup; post-U1 baseline)

**Goal:** No clang-tidy warnings remain in `tests/` under the enforced check set. **Full strict cleanup** (user decision), scoped from the post-U1 re-baselined count.

**Requirements:** R2

**Dependencies:** U1, U6

**Files:**
- Modify: `tests/**` (heaviest in `tests/storage/`, `tests/streaming/`, `tests/control/`)
- Create (fallback only): `tests/.clang-tidy` (documented fallback if a *stylistic* check's churn proves clearly low-value — never for `bugprone-*`/`performance-*`)

**Approach:**
- `scripts/fix.sh` first, then hand-fix: named constants for resolutions/ports/fps, longer identifiers, name unused params, decompose high-cognitive-complexity `TestBody`s where reasonable.
- **Floor applies in tests too:** `bugprone-*`/`performance-*` stay enforced. Only the idiomatic readability checks (`magic-numbers`, `identifier-length`, `cognitive-complexity`) are eligible for the `tests/.clang-tidy` fallback if cleanup proves disproportionate — record the decision.
- **Regression discipline (CLAUDE.md test-isolation):** renames/constants are behavior-preserving; run `ctest --preset test` after every cluster — a mechanical rename that touches per-test temp dirs or fixtures must not bleed state across tests.

**Patterns to follow:** existing test fixtures' constant/naming style; CLAUDE.md test-isolation rules.

**Test scenarios:**
- Regression: full `ctest --preset test` passes unchanged after each cluster. Covers R6.
- Happy path: `clang-tidy-14` over `tests/` → 0 warnings under the effective config.

**Verification:** In-container `clang-tidy-14` on `tests/` emits 0 warnings; `ctest --preset test` green.

---

### U4. Enforce the gate: `WarningsAsErrors` + drop `continue-on-error`

**Goal:** Warnings now fail the build; the `tidy` job is a hard gate with zero debt.

**Requirements:** R3, R4

**Dependencies:** U1, U2, U3

**Files:**
- Modify: `.clang-tidy` (`WarningsAsErrors` → the enforced set, per the U2/U3 triage outcome)
- Modify: `.github/workflows/ci.yml` (remove `continue-on-error: true` and the stale advisory comment from the `tidy` job)

**Approach:**
- Tuned-out *stylistic* checks (if any) are **removed from `Checks`** — not just dropped from `WarningsAsErrors` (a check left in `Checks` still emits a warning). `bugprone-*`/`performance-*` stay in `Checks`.
- Set `WarningsAsErrors: '*'` to promote everything still emitted. Record the final `Checks` + `WarningsAsErrors` strings verbatim in `.clang-tidy` with rationale comments.
- Remove `continue-on-error`; the job now hard-fails on any diagnostic.

**Test scenarios:**
- Happy path: clean tree → `tidy` exits 0 and is green (not yellow).
- Error path: introduce a deliberate violation (e.g. a magic number in a `src/` file on a scratch branch) → `tidy` exits non-zero and fails the PR. Covers R4.
- Edge case: a `// NOLINT`-suppressed line (if any genuine false positive exists) does not fail.

**Verification:** In-container full-tree clang-tidy exits 0; a seeded violation makes it exit non-zero.

---

### U5. Make `tidy` a required status check + refresh docs

**Goal:** `main` cannot merge a PR with a red `tidy`; documentation reflects the now-hard gate.

**Requirements:** R5

**Dependencies:** U4

**Files:**
- Modify: branch protection / ruleset on `main` (add `tidy` to required status checks — via `gh api` or repo settings; the ruleset referenced as "U5" in `ci.yml`'s header comment)
- Modify: `CLAUDE.md` (the "CI/CD & releasing" section currently says tidy is advisory `continue-on-error` — update to "hard gate")
- Modify: `.github/workflows/ci.yml` (header comment already lists `tidy` as a required check — confirm consistent)

**Approach:**
- **Stability bar before the flip (this is the only irreversible step):** require **≥3 consecutive green `tidy` runs on `main`** after U4, with the enforced `Checks`/`WarningsAsErrors` set frozen and recorded, before adding `tidy` to required checks. If the check set is still churning, do not flip.
- Add `tidy` alongside `format` and `test` in the protected-branch required checks.
- Update prose in `CLAUDE.md` and any stale "advisory" comments.

**Test scenarios:**
- Test expectation: none — branch-protection + docs change. Verified by inspecting the ruleset (`gh api repos/:owner/:repo/rulesets` or branch protection) shows `tidy` required, and a PR with failing tidy is blocked from merge.

**Verification:** `gh` shows `tidy` in required checks; `CLAUDE.md` no longer calls tidy advisory.

---

### U6. Dev-side auto-fix tooling (fix script + pre-commit)

**Goal:** Developers auto-correct fixable findings before push; CI stays verify-only. Clears the *fixit-bearing subset* of the cleanup mechanically (a minority — the noisy blockers have no fixit), so U2/U3 start from a smaller manual set.

**Requirements:** R8

**Dependencies:** U1 (consumes the `scripts/tidy-args.sh` arg-builder authored in U1)

**Files:**
- Create: `scripts/fix.sh` (sources `scripts/tidy-args.sh`; runs `clang-format -i` + `clang-tidy-14 --fix`/`--fix-errors`)
- Create: `.pre-commit-config.yaml` (or a `.githooks/` hook documented in CLAUDE.md) wiring `scripts/fix.sh` on staged C/C++ files
- Modify: `CLAUDE.md` (document `scripts/fix.sh` + the hook under "Linting & formatting")

**Approach:**
- `scripts/fix.sh` and the CI `tidy` job **both source `scripts/tidy-args.sh`** (authored in U1) — one arg set, no drift.
- **Scope to staged files in the hook**, not the whole tree: `git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(cpp|hpp|cc|h)$'`. A full-tree `--fix` would silently mutate unstaged dirty files. (A `--all` mode for bulk cleanup is fine for explicit invocation; the *hook* path is staged-only.)
- Runs inside the devcontainer (cross toolchain env; not a host-clang path).
- `--fix` applies fixits where they exist; unfixable checks (`magic-numbers`, `easily-swappable-parameters`, `cognitive-complexity`, `branch-clone`) remain as errors for the dev to resolve.

**Execution note:** Land right after U1 (reuses its arg-builder); run before U2/U3 to clear the fixit-bearing subset first. Do **not** treat it as de-risking the U2/U3 manual scope — it doesn't touch the named blockers.

**Patterns to follow:** existing `.clang-format`/`.clang-tidy` usage in CLAUDE.md "Linting & formatting".

**Test scenarios:**
- Happy path: dirty a fixable item (e.g. a member that should be `[[nodiscard]]`), run `scripts/fix.sh` in-container → file is corrected, re-running clang-tidy shows that finding gone.
- Edge case: an unfixable finding (a magic number) is left untouched by `--fix` and still reported — proving auto-fix doesn't mask blockers.
- Integration: staging a file with a formatting violation and committing triggers the hook, which formats it before the commit lands.

**Verification:** `scripts/fix.sh` corrects the fixable subset and leaves unfixable findings as errors; the hook fires on commit; CLAUDE.md documents both.

---

### U7. Cache the devcontainer image in GHCR

**Goal:** CI jobs reuse a prebuilt, digest-pinned devcontainer image instead of rebuilding the ~37 GB sysroot every run. Run-time improvement is a **target to be measured**, not an assumed ~5 min.

**Requirements:** R9

**Dependencies:** U1 (soft order): the image bakes nothing about the tidy flags — those live in `ci.yml`/`scripts/tidy-args.sh` — but land U7 **after** U1 so the first post-cache CI run already uses the corrected invocation rather than caching a run that still header-errors.

**Files:**
- Create: `.github/workflows/devcontainer-image.yml` (build + push to `ghcr.io/<owner>/sst-cam-firmware-devcontainer`; trigger **`on: push: branches:[main]` paths `.devcontainer/**`** + `workflow_dispatch` — **no `pull_request`**, so a fork can't trigger a `packages: write` publish)
- Modify: `.github/workflows/ci.yml` (all three jobs: **per-job** `permissions: packages: read` — not workflow-level — a ghcr login step, and `imageName`/`cacheFrom` pinned by **digest**)
- Modify: `.devcontainer/Dockerfile` (pin base `FROM nvcr.io/...:6.1@sha256:<digest>`)
- Create: `.devcontainer/image-digest` (tracked digest of the last published cache image, written by the publish workflow, consumed by CI to pin `imageName`)

**Approach (measurement-gated):**
1. **First, measure** — publish once, then on a real hosted runner with `jlumbroso/free-disk-space` applied, record pull wall-time + peak disk. If the pulled image doesn't beat the rebuild, or ENOSPCs, **stop and keep per-run build** (documented rollback). Only proceed to rewire all three jobs if the measurement wins.
2. Publish workflow: `permissions: packages: write` + `docker/login-action` to ghcr with `GITHUB_TOKEN` (fixes the prior `docker tag … ghcr.io` failure), runs only from `main`.
3. **Supply-chain pinning:** digest-pin the NGC base in the Dockerfile and reference the GHCR cache image by digest (via `.devcontainer/image-digest`), so a mutable-tag overwrite can't silently feed a poisoned image (e.g. a clang-tidy that always exits 0) to every CI job.
4. CI jobs consume read-only (`packages: read`, per-job); fork PRs pull the public image, never push.

**Patterns to follow:** `release.yml`'s `push: branches:[main]` + `permissions` model; existing `devcontainers/ci@v0.3` usage.

**Test scenarios:**
- Happy path: after publish, a code-only PR's three jobs pull the digest-pinned image and skip the full unpack (record wall-time + disk delta).
- Edge case: a PR touching `.devcontainer/**` triggers rebuild+republish; CI updates `image-digest` and dependent jobs use the new digest.
- Security: a `pull_request` from a fork does **not** trigger `devcontainer-image.yml` and cannot obtain `packages: write`.
- Error path: a pull/auth failure fails the job **loudly** (no silent fallback to a stale or slow path).

**Verification:** CI logs show a digest-pinned image pull (not a 13-min unpack) on code-only PRs; measured pull time + peak disk recorded and beating the rebuild; publish runs only from `main`; base image digest-pinned.

---

## System-Wide Impact

- **Interaction graph:** the PR CI `tidy` job + `.clang-tidy`; `format`/`test` jobs are touched by U7 (GHCR `imageName`/`cacheFrom`) but their check semantics are unchanged; new `scripts/fix.sh` + pre-commit hook are dev-side. `release.yml` untouched.
- **API surface parity:** none — no runtime code contract changes (U2/U3 are behavior-preserving cleanups).
- **State lifecycle risks:** none.
- **Integration coverage:** `ctest --preset test` after U2/U3 guards against accidental behavior change from renames/constants.
- **Unchanged invariants:** the cross-build toolchain, devcontainer image, `format`/`test` gates, and all production runtime behavior are explicitly unchanged.

---

## Risks & Dependencies

| Risk | Mitigation |
| --- | --- |
| **Verification harness broken** — live `app-1` lacks clang-tidy | U1 pre-step rebuilds the devcontainer + confirms `clang-tidy-14 --version` before any verification. |
| **clang-14 vs gcc-11.3 builtin-header conflict** if g++ builtin dirs are injected | Primary path is matched-triple `--gcc-toolchain` (keeps clang's resource dir); fallback injection excludes gcc `include`/`include-fixed`; falsification test asserts zero redefinition diagnostics. |
| **"644" balloons after U1** (broken-parse artifact) | U1 re-baselines the count; U2/U3 scope from the post-U1 number, not 644. |
| **Gate silently hollowed via tune-out** | Hard floor: `bugprone-*`/`performance-*` may not be removed from `Checks`; only stylistic checks relaxable, recorded in `.clang-tidy`. |
| Cleaning warnings introduces behavior regressions | `ctest --preset test` green after each cluster; cleanups behavior-preserving; CLAUDE.md test-isolation respected. |
| Adding `tidy` to required checks blocks merges if it flakes | U5 stability bar: ≥3 consecutive green `tidy` on `main` with the check set frozen before the flip. |
| CI invocation and `scripts/fix.sh` drift | Both source `scripts/tidy-args.sh` (authored in U1) — single arg set. |
| **GHCR pull may regress (ENOSPC / slower than rebuild)** | U7 is measurement-gated: measure pull time + peak disk first; documented rollback to per-run build if it doesn't win. |
| **GHCR mutable-tag image poisoning** (a bad image hits all CI) | Digest-pin the NGC base + the GHCR cache image; CI references by digest via `.devcontainer/image-digest`. |
| GHCR publish perms abused from a fork | `devcontainer-image.yml` triggers only `push: branches:[main]` (no `pull_request`); CI jobs get per-job `packages: read` only. |
| Pre-commit `--fix` mutates unstaged files | Hook scopes to staged C/C++ files only (`git diff --cached`). |
| clang-tidy version bump silently changes the gate | `clang-tidy-14` pinned in Dockerfile + called explicitly; a bump is a deliberate re-triage. |

---

## Sources & References

- **Origin document:** `docs/brainstorms/2026-06-15-clang-tidy-hard-gate-requirements.md`
- Related config: `.github/workflows/ci.yml`, `.clang-tidy`, `.devcontainer/Dockerfile`
- Related learning: `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md`
- Evidence: CI run 27565494770 (the 45 `file not found` errors + 644 warnings)
