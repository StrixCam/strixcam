---
title: "perf: Shard the PR clang-tidy gate across a runner matrix"
type: perf
status: active
date: 2026-06-24
deepened: 2026-06-24
revised: 2026-06-25
origin: docs/brainstorms/2026-06-24-tidy-diff-scope-requirements.md
---

# perf: Shard the PR clang-tidy gate across a runner matrix

> **Provenance:** Reconstructed 2026-06-25 from session history (`16f32beb`); the
> original plan lived in a now-deleted worktree and was never committed.
>
> **Pivot (2026-06-25):** This plan originally proposed **diff-scoping** the PR
> tidy gate (lint only changed `.cpp`/`.cc`, escalate to full on header/config
> edits). Acceptance criterion #1 demanded OQ-G be measured first. It was:
> running the escape-hatch intersection over **every merged PR** (8 merges)
> showed **7 of 7 code-bearing PRs touch a header** and would escalate to a full
> scan — the diff-scope win evaporates on this hexagonal C++ tree (ports + the
> mandatory per-model `fmt::formatter` specializations live in headers). The
> data killed the premise, so the plan was **re-targeted to OQ-H's
> coverage-preserving lever: shard the existing full scan across a runner
> matrix.** Sharding keeps full-tree coverage on every PR (no coverage hole, no
> "PR-green/push-blocks" asymmetry, "green = clean tree" stays true) and cuts
> wall-time by ~N. The shared-script extraction and selection self-test survive
> the pivot unchanged in spirit.

## Summary

PR `tidy` lints all ~95 TUs every run (~21 min flat) because clang-tidy cost
scales with **TU count** — each TU re-parses the heavy GStreamer/OpenCV headers.
**Shard** that full scan: split the TU list across a `tidy-shard` runner matrix
(round-robin, N=4) so each shard builds once and lints its slice in parallel,
cutting lint wall-time ~Nx while **every TU is still linted on every PR**. A thin
aggregator job named `tidy` (the wired required check) gates on all shards. The
selection/sharding logic lives in a shared, locally testable
`scripts/ci/tidy-run.sh` (replacing the byte-identical inline block duplicated
across `release-alpha.yml` and `release-beta.yml`) with its own self-test.

---

## Problem Frame

clang-tidy cost is dominated by **TU count**: each of the ~95 TUs re-parses heavy
GStreamer/OpenCV headers, so the gate is ~21 min regardless of PR size. A PR
changes a handful of TUs but pays for all of them. The prebuilt-image work removed
the *image rebuild*; it did not touch the clang-tidy invocation, which is now the
PR long pole. (See origin: `docs/brainstorms/2026-06-24-tidy-diff-scope-requirements.md`.)

The discarded alternative (diff-scoping) tried to shrink the **input set**; it
foundered on `.clang-tidy`'s `HeaderFilterRegex: '(src|tests)/.*'` — header
violations surface only while linting a TU that includes them, so any header edit
had to escalate to a full scan, and measurement showed that's nearly every PR.
Sharding instead shrinks the **per-runner work** without shrinking the input:
the whole tree is still linted, just spread across runners.

---

## Requirements

- R1. A PR finishes the `tidy` gate materially faster than today's ~21 min flat —
  **target: lint wall-time ≈ full-scan-time / N** for N shards (the residual
  per-shard overhead is the unconditional `cmake --preset test` + `sst_proto`
  build + image pull, replicated per shard). With N=4 and ~95 TUs the lint step
  drops from ~21 min to ~5–6 min per shard, all running in parallel.
- R2. **Full-tree coverage is preserved on every PR.** The union of the shards is
  exactly today's TU list (`find src tests \( -name '*.cpp' -o -name '*.cc' \)
  -not -path '*/_old/*'`); no TU is dropped and none is linted twice. There is
  **no coverage hole** and **no header-filter escape hatch** — every header is
  still exercised by whichever shard owns a TU that includes it.
- R3. The tidy hard gate is preserved unchanged in semantics:
  `WarningsAsErrors` still fails the run; a new violation in **any** TU still
  blocks the PR. **The "green PR tidy = whole-tree-clean" contract is
  unchanged** — sharding is an execution detail, not a coverage narrowing.
- R4. The wired required status check is still named **`tidy`**, byte-identical.
  Because a matrix job reports as `tidy-shard (0)`, `tidy-shard (1)`, … (which
  would not satisfy branch protection), the matrix runs under a **separate**
  `tidy-shard` job and a thin **aggregator** job named `tidy` `needs:` it and
  fails iff any shard failed.
- R5. The aggregator must **not** be defeated by `needs:` skip-propagation: a
  failed shard must fail `tidy`, not skip it into a green "skipped" (skipped =
  success for required checks). The aggregator uses `if: always()` (plus the
  event/`code` guard) and explicitly asserts the shard aggregate result.
- R6. The floor-NOLINT guard is unchanged in behavior (diff-scoped, PR-only) and
  its self-test still passes. It runs **once** (it is host-side and need not run
  per shard) — it moves into the aggregator job.
- R7. The file-selection / sharding logic has its own **self-test** (R-SELECT):
  it asserts the shard split is complete (union == full), disjoint, and
  deterministic, and that an unwired TU fails rather than silently skips.
- R8. No byte drift between the alpha and beta tidy jobs (extract to one script;
  apply the identical workflow blocks to both files).
- R9. Any `.cpp`/`.cc` selected for a shard, present in the tree but **absent
  from `compile_commands.json`** (a new file not yet wired into a CMake target),
  is a **fail**, never a silent skip (R-NEWFILE). *(Note: under sharding the
  guard covers every selected TU, not just diff-changed files — it is strictly
  stricter than the abandoned diff-scope design's "changed file" check.)*

**Origin trace:** R1/R8 derive from the requirements doc's speed + R-DRIFT goals;
R2/R3 replace the diff-scope coverage-narrowing (finding I) with coverage
preservation (the sharding pivot's whole point); R4/R5 are the matrix-specific
required-check + skip-propagation hazards surfaced during the pivot; R6/R7/R9
carry over the floor-guard, R-SELECT, and R-NEWFILE concerns from the original.

---

## Scope Boundaries

- No change to `.clang-tidy`'s check set or `scripts/tidy-args.sh` flags.
- No change to `format` / `test` jobs. (`ci-scripts` gains the tidy-run self-test
  step + shellcheck of the new scripts — U3.)
- No `ccache` / PCH compile caching **in this plan**. Sharding and a compile
  cache are *composable* (a cache would further cut each shard's build), but the
  cache is deferred (see Deferred to Follow-Up).
- Shard count is fixed at **N=4** in this plan (the matrix `shard:` list; the
  total is derived from it via `${{ strategy.job-total }}`, so there is no second
  value to keep in sync). Tuning N is a one-line follow-up, not a redesign.
- `release.yml` (`main`) stays promote-only; no tidy is added there. Unlike the
  diff-scope design, this is **not** a coverage gap: the PR `tidy` gate now lints
  the whole tree and blocks the merge, so a blocking full-tree lint **does** run
  before `main` (OQ-F resolved positively — see Open Questions).

### Deferred to Follow-Up Work

- `ccache`/PCH for the `cmake --preset test` build that feeds
  `compile_commands.json` — compounds with sharding (cuts the replicated
  per-shard build cost).
- Tuning the shard count N against observed wall-time / runner-minute cost.
- `LABELS hardware` retag of hardware-bound tests (touches `test`, not `tidy`).

---

## Context & Research

### Relevant Code and Patterns

- `.github/workflows/release-alpha.yml` — `tidy` job (PR-only:
  `if: github.event_name == 'pull_request' && needs.changes.outputs.code == 'true'`,
  `needs: [changes, image]`). Runs `cmake --preset test` +
  `cmake --build --preset test --target sst_proto`, then
  `find src tests \( -name '*.cpp' -o -name '*.cc' \) -not -path '*/_old/*' | xargs clang-tidy -p build/test "${TIDY_EXTRA_ARGS[@]}"`.
- `.github/workflows/release-beta.yml` — the `tidy` block is **byte-identical**
  (verified by diff). Both must change together → shared script (R8).
- `changes` job (both files) — `dorny/paths-filter@v3`, PR-only, output `code`
  already includes `scripts/**`, so edits to `tidy-run.sh` trigger the gate. No
  change needed here (sharding adds no push-time job).
- `scripts/check-floor-nolints.sh` + `scripts/test-check-floor-nolints.sh` — the
  floor-NOLINT guard + its self-test; the tidy job already calls both, and they
  move verbatim into the aggregator. The synthetic-repo self-test is the pattern
  the sharding self-test mirrors.
- `scripts/ci/resolve-version.sh` + `scripts/ci/resolve-version-test.sh` — the
  positional-arg interface and the `ci-scripts` host-side self-test wiring
  (incl. PATH-stub of `git`) that `tidy-run.sh` / `test-tidy-run.sh` copy.
- `scripts/tidy-args.sh` — sourced for `TIDY_EXTRA_ARGS` (shared with
  `scripts/fix.sh`); `tidy-run.sh` sources it the same way (via its own
  `SCRIPT_DIR` so it resolves regardless of cwd).

### Measurement that drove the pivot (OQ-G)

Escape-hatch intersection over every merged PR (8 merges) — **7/7 code PRs
escalate to full** under the diff-scope design; the only non-escalating PR was
docs-only (which already skips `tidy` via the `code` filter):

| PR | files | cpp | escape-hits | diff-scope outcome |
|----|-------|-----|-------------|--------------------|
| #7 docs | 3 | 0 | 0 | (docs-only — tidy skipped) |
| #6 cpp | 413 | 82 | 164 | escalate → full |
| #5 logic-align | 30 | 18 | 9 | escalate → full |
| #4 wifi-bt | 237 | 74 | 135 | escalate → full |
| #3 buffer | 41 | 10 | 29 | escalate → full |
| #2 gstreamer | 15 | 5 | 8 | escalate → full |
| merge cpp | 12 | 2 | 4 | escalate → full |
| merge cpp | 6 | 1 | 5 | escalate → full |

Sample caveat: 8 merges, early-stage, large module drops. But the structural
cause (ports + per-model `fmt::formatter` specializations in headers) makes any
feature PR header-touching regardless of size — so diff-scope helps ~0% of real
PRs, while sharding helps **every** PR.

### Institutional Learnings

- The job `name:` values (`ci-scripts`/`format`/`tidy`/`test`) are the wired
  required status checks — keep them **byte-identical** (see `docs/ci/rulesets.md`).
  A matrix job's per-leg name (`tidy-shard (0)`) is **not** the bare `tidy`, so
  the matrix must hide behind an aggregator that keeps the name (R4).
- A job skipped by `if:` (or by unmet `needs:`) counts as **success** for
  required checks — which is why the aggregator must `if: always()` and assert
  the shard result rather than relying on `needs:` to fail it (R5).

---

## Key Technical Decisions

1. **Shard the full scan; do not shrink the input.** Keep today's exact TU list
   and split it across N runners. Coverage is identical to a single full scan;
   only wall-time changes. This is OQ-H's lever, chosen after OQ-G killed
   diff-scope.
2. **Round-robin split (`NR mod N`) over a `LC_ALL=C sort`-ed TU list.** The
   split assigns each TU to a shard by its **index** in the sorted list, so every
   shard runner must compute a byte-identical order — `LC_ALL=C` pins
   byte-collation so the order is locale-independent (a UTF-8/C collation skew on
   one leg would silently rebin a TU). The split is then deterministic and even
   by construction (95 TUs → 24/24/24/23); the union of all N shards is provably
   the full list and the shards are pairwise disjoint — all asserted by the
   self-test, including locale-stability of the order (R7). **The shard `total`
   is derived from the matrix via `${{ strategy.job-total }}`, never hardcoded**,
   so the index and the total cannot drift into a silent-coverage-hole — any gap
   or out-of-range index fails *closed* (`tidy-run.sh` rejects `index >= total`).
3. **Separate matrix job + aggregator.** `tidy-shard` is a `strategy.matrix`
   job (`fail-fast: false`, `shard: [0,1,2,3]`); the wired required check is a
   thin host-side job named **`tidy`** that `needs: [changes, tidy-shard]`. A
   matrix cannot itself carry the bare `tidy` name (R4).
4. **Aggregator is `if: always()` + explicit assert** (R5). On a code PR it runs
   regardless of shard outcome and fails unless `needs.tidy-shard.result ==
   'success'` (the matrix aggregate — success only when all legs pass). On a
   docs-only PR (`code != true`) the `if` is false → skipped → green, matching
   today. It also hosts the floor-NOLINT guard (host-side, run once).
5. **Extract `scripts/ci/tidy-run.sh`** taking explicit positional args
   (`tidy-run.sh full` / `tidy-run.sh shard <index> <n>`), matching
   `scripts/ci/resolve-version.sh`. The selection logic is a sourced function
   `select_tidy_files` so the self-test can call it directly without invoking
   cmake/clang-tidy (no production-only "dry-run" branch).
6. **The build is unconditional and per-shard.** Each shard runs `cmake --preset
   test` + `--target sst_proto` (so `compile_commands.json` + protobuf headers
   exist) before linting its slice. Two distinct guards at two moments:
   **argument validation** (`select_tidy_files`) runs *before* the build so a
   malformed shard invocation fails fast without paying for cmake; the
   **DB-membership guard** (R9) runs *after* the build, once
   `compile_commands.json` exists — a selected TU present in the tree but absent
   from it is a **fail**, never a silent skip (finding D). A TU gone from the
   tree (deleted/renamed) is simply dropped.

---

## Open Questions

### Resolved

- **OQ-G — Is the speed win real for THIS codebase?** *Measured: no, not for
  diff-scope (7/7 code PRs escalate). Resolved by abandoning diff-scope for
  sharding, which is PR-shape-independent.*
- **OQ-H — Diff-scope vs a coverage-preserving fix?** *Resolved: chose the
  coverage-preserving fix (matrix sharding). ccache/PCH is deferred but
  composable, not a competitor.*
- **OQ-F — Does any blocking job lint the WHOLE tree?** *Resolved positively:
  the PR `tidy` gate now lints the whole tree (sharded) and blocks the merge, so
  a blocking full-tree lint runs on every `development` / `release/**` PR and
  thus before `main`. No push-time safety net is needed.*
- Edit two inline blocks vs extract → **extract one shared script** (R-DRIFT).

### Pre-existing repo inconsistency to reconcile (not introduced here)

- **clang-tidy v14 vs v18.** `.clang-tidy`'s header says "VERSION-LOCKED to
  clang-tidy-14 (jammy)", while CLAUDE.md says "noble v18". This plan proposes
  **no** version change and does not depend on the answer. Reconcile
  `.clang-tidy`'s header with the actual devcontainer toolchain as a separate
  cleanup.

---

## High-Level Technical Design

> *Directional guidance for review, not implementation specification.*

```mermaid
flowchart TD
    PR[pull_request + code changed] --> SHARDS{tidy-shard matrix\n shard 0..3, fail-fast:false}
    SHARDS --> S0[tidy-run.sh shard 0 4\n build + lint slice 0]
    SHARDS --> S1[tidy-run.sh shard 1 4]
    SHARDS --> S2[tidy-run.sh shard 2 4]
    SHARDS --> S3[tidy-run.sh shard 3 4]
    S0 --> AGG[tidy aggregator\n if: always()]
    S1 --> AGG
    S2 --> AGG
    S3 --> AGG
    AGG --> FLOOR[floor-NOLINT guard + self-test]
    AGG --> ASSERT{needs.tidy-shard.result == success?}
    ASSERT -- no --> FAIL[fail the required check]
    ASSERT -- yes --> PASS[green: whole tree clean]
```

`tidy-run.sh` selects the file list per `mode`+`shard`, runs the build, guards
DB-membership, then `xargs clang-tidy -p build/test "${TIDY_EXTRA_ARGS[@]}"`. The
selection function is what the self-test exercises.

---

## Implementation Units

- U1. **`scripts/ci/tidy-run.sh` — shared, mode-driven tidy runner**

**Goal:** One script encapsulating build + file-selection + clang-tidy, replacing
the inline block in both workflows, with a sharding mode.

**Requirements:** R1, R2, R8, R9

**Dependencies:** None

**Files:**
- Create: `scripts/ci/tidy-run.sh`

**Approach:**
- **Positional args:** `tidy-run.sh full` / `tidy-run.sh shard <index> <n>`
  (index in `[0, n)`, `n ≥ 1`); matches `resolve-version.sh`.
- **Sourced selection function** `select_tidy_files <mode> [index] [n]` emits the
  TU list on stdout and touches neither cmake nor clang-tidy. `full` →
  `find src tests \( -name '*.cpp' -o -name '*.cc' \) -not -path '*/_old/*' | sort`
  (today's exact list, sorted for determinism). `shard` → that list filtered by
  `NF { if ((n++ % tot) == idx) print }`. Invalid args → non-zero exit (loud).
- **Main body:** validate + **select first** (cheap, fails fast on bad args);
  empty selection → `full` exits 1 (degenerate tree), `shard` exits 0 (another
  shard owns it). Then `cmake --preset test` + `--build --target sst_proto`.
  Then the **DB-membership guard** (R9): a selected TU present in the tree but
  absent from `build/test/compile_commands.json` → fail; a TU gone from the tree
  is dropped. Then `source` `tidy-args.sh` (via `SCRIPT_DIR`) and
  `xargs clang-tidy -p build/test "${TIDY_EXTRA_ARGS[@]}"`.
- Guard the main body with `BASH_SOURCE==$0` so the self-test can source the
  file without running the build.

**Patterns to follow:** `scripts/check-floor-nolints.sh` (`set -euo pipefail`,
extension globs); the existing inline tidy step for the build + `tidy-args`
sourcing; `scripts/ci/resolve-version.sh` for the positional-arg interface.

**Test scenarios:** see U3 (self-test).

**Verification:** `scripts/ci/tidy-run.sh full` in the devcontainer reproduces
today's tidy result on a clean tree (identical `find` + build + `tidy-args`).

---

- U2. **Wire both workflows: `tidy-shard` matrix + `tidy` aggregator**

**Goal:** Replace the single `tidy` job with a sharded matrix plus a thin
aggregator that keeps the byte-identical required-check name and gates on the
shards.

**Requirements:** R3, R4, R5, R6, R8

**Dependencies:** U1

**Files:**
- Modify: `.github/workflows/release-alpha.yml`, `.github/workflows/release-beta.yml`

**Approach (apply identically to both files — R8):**
- **`tidy-shard`** (new): `needs: [changes, image]`, same PR + `code` `if:` as
  today, `permissions: {contents: read, packages: read}`,
  `strategy: { fail-fast: false, matrix: { shard: [0, 1, 2, 3] } }`. Steps:
  free-disk, checkout (`submodules: recursive`, **no** `fetch-depth: 0` — no diff
  needed), conan cache, QEMU, GHCR login, then
  `docker run … -e SHARD=${{ matrix.shard }} -e TOTAL=${{ strategy.job-total }} … bash -lc 'scripts/ci/tidy-run.sh shard "${SHARD}" "${TOTAL}"'`.
  The total is derived from the matrix via `strategy.job-total`, so it can never
  drift from the `shard:` list length.
- **`tidy`** (aggregator, the wired required check): `name: tidy`,
  `needs: [changes, tidy-shard]`,
  `if: ${{ always() && github.event_name == 'pull_request' && needs.changes.outputs.code == 'true' }}`,
  host-side (no devcontainer). Steps: checkout (`fetch-depth: 0` for the floor
  guard), floor-NOLINT self-test, floor-NOLINT guard (diff-scoped, PR base SHA),
  then **assert** `needs.tidy-shard.result == 'success'` (fail otherwise).
- Keep job **names** byte-identical for the required checks; `tidy-shard` is a
  new, non-required job. Keep the `image` pull, conan cache, QEMU setup on the
  shard job.

**Patterns to follow:** the existing `docker run --rm --user root … bash -lc`
wrapper; the floor-guard's `base.sha` usage; standard matrix + aggregator idiom.

**Test scenarios:**
- PR with code → 4 `tidy-shard` legs run, union covers all TUs; `tidy` aggregator
  green only if all pass.
- A violation in any shard → that shard fails → aggregator asserts failure → PR
  blocked.
- Docs-only PR (`code != true`) → `tidy-shard` + `tidy` skipped → required check
  green (unchanged from today).

**Verification:** `tidy` job name unchanged (required check intact); a forced
shard failure red-bars the PR via the aggregator (not a silent green-skip).

---

- U3. **`scripts/ci/test-tidy-run.sh` — selection / sharding self-test**

**Goal:** Assert the shard split so a regression can't silently drop a TU from
the merge-blocking lint.

**Requirements:** R7

**Dependencies:** U1, U2 (shares the workflow files — `ci-scripts` wiring lands
alongside U2's `tidy-shard`/`tidy` edits, so apply them in one pass per file)

**Files:**
- Create: `scripts/ci/test-tidy-run.sh`
- Modify: `.github/workflows/release-alpha.yml` / `release-beta.yml` to run it +
  shellcheck the new scripts in `ci-scripts` (host-side; mirror
  `resolve-version-test.sh`).

**Approach:**
- `source` `tidy-run.sh` and call `select_tidy_files` over synthetic temp trees
  (no cmake/clang-tidy). For the DB-membership case, run the real main body with
  `cmake`/`clang-tidy` **stubbed as no-ops on PATH** (the technique
  `resolve-version-test.sh` uses to stub `git`) over a hand-written
  `compile_commands.json`.
- Cases asserted:
  - `full` → the 4 real TUs, sorted, headers + `_old/` excluded.
  - shard **union == full** for several N (1, 2, 3, 4, 8) — completeness.
  - shards **pairwise disjoint** — no double-lint.
  - shard split **deterministic** — same slice twice.
  - `shard 0 1` collapses to full.
  - bad args (index == total, non-integer index, unknown mode) → non-zero exit.
  - unwired TU absent from `compile_commands.json` → **FAIL**; all TUs present →
    pass (R9).
- Add `shellcheck scripts/ci/tidy-run.sh scripts/ci/test-tidy-run.sh` to the
  `ci-scripts` shellcheck step.

**Patterns to follow:** `scripts/test-check-floor-nolints.sh` (synthetic-repo
fixture); `resolve-version-test.sh` (pass/fail counters, PATH-stub of a tool,
`ci-scripts` wiring).

**Coverage caveat:** `ci-scripts` is PR-only, so the self-test runs on PRs (the
only event that runs `tidy`). No push-time tidy exists in this design, so there
is no uncovered push path to worry about.

**Verification:** `./scripts/ci/test-tidy-run.sh` passes locally and in
`ci-scripts`; deliberately breaking the split (or the DB guard) makes it fail.

---

## Risks & Mitigations

- **R-REQUIRED-CHECK (P0):** a matrix `tidy` job reports as `tidy (0)`/`tidy (1)`
  and the bare `tidy` required check never reports → PR unmergeable (pending
  check). → decision 3: the matrix is `tidy-shard`; a thin aggregator keeps
  `name: tidy`.
- **R-SKIP-PROP (P0):** a failed shard could skip-propagate through `needs:` and
  turn the aggregator into a green "skipped" (skipped == success). → decision 4:
  aggregator is `if: always()` and explicitly asserts
  `needs.tidy-shard.result == 'success'`.
- **R-SELECT (closed):** a wrong split silently drops a TU (fail-open). → three
  layers: (a) round-robin over a `LC_ALL=C`-pinned sorted list so the order is
  locale-stable across runners; (b) `total` derived from
  `${{ strategy.job-total }}` so the index and total cannot drift (any gap /
  out-of-range fails *closed*); (c) U3 self-test asserts union == full, disjoint,
  deterministic, **and locale-stable**.
- **R-DESYNC (closed):** the matrix length and the shard `total` could drift if
  the total were hardcoded — `total > matrix length` would leave round-robin bins
  uncovered (silent ~coverage hole, gate green). → decision 2: derive `total`
  from `${{ strategy.job-total }}`; the two are then one source.
- **R-NEWFILE:** a selected TU absent from `compile_commands.json` (new, unwired)
  is silently unlinted. → decision 6 / U1: missing-from-DB ⇒ fail.
- **R-DRIFT:** alpha/beta tidy blocks diverge. → U1 single shared script + the
  identical workflow blocks (asserted byte-identical by diff).
- **R-SHARD-COST:** per-shard build + image pull is replicated N times (N× the
  runner-minutes for setup). Accepted: setup is parallel wall-time, and the lint
  long pole is what shrinks. Tuning N and a compile cache are deferred follow-ups.
  **Strategic caveat (doc-review, product-lens):** this is a money-for-latency
  trade on a low-cadence repo (~8 merges to date), and the deferred ccache/PCH is
  the *compounding* lever (it cuts total compute and makes each shard cheaper),
  whereas sharding multiplies compute for parallel wall-time. The minimal
  standalone-value slice is the R8 shared-script extraction (kills the alpha/beta
  drift regardless of the speed bet). If runner-minute cost matters more than gate
  latency at current cadence, an alternative is to ship U1 (the shared script)
  now and gate the matrix wiring (U2) behind a ccache spike + an absolute
  wall-time target. This plan proceeds with sharding by explicit decision; the
  caveat is recorded so the trade is owned, not assumed.

---

## Acceptance Criteria

- [ ] **OQ-G/OQ-H/OQ-F resolved:** measured (7/7 escalate), sharding chosen,
      whole-tree blocking lint confirmed on every PR.
- [ ] `scripts/ci/tidy-run.sh` exists; `tidy-run.sh full` reproduces today's
      result on a clean tree.
- [ ] PR `tidy` lints the **whole tree, sharded** — the union of `tidy-shard`
      legs equals today's TU list; no TU dropped or double-linted.
- [ ] Any selected `.cpp` present in the tree but absent from
      `compile_commands.json` **fails** the job (does not silently skip).
- [ ] The shard `total` is derived from `${{ strategy.job-total }}` (no hardcoded
      drift) and the TU list is `LC_ALL=C`-sorted (locale-stable across runners);
      the self-test asserts union completeness + locale-stability.
- [ ] The required check is still named **`tidy`** (byte-identical); the matrix
      runs as `tidy-shard`; the aggregator gates on all shards.
- [ ] A failed shard **fails** the `tidy` aggregator (not a green skip) — verified
      by `if: always()` + the explicit result assertion.
- [ ] `scripts/ci/test-tidy-run.sh` passes in `ci-scripts`; breaking the split or
      the DB-membership guard fails it.
- [ ] Floor-NOLINT guard unchanged (PR-only) and its self-test passes (now in the
      aggregator).
- [ ] alpha and beta tidy jobs call the same script and use identical workflow
      blocks (no byte drift).
- [ ] Job names `ci-scripts`/`format`/`tidy`/`test` unchanged (required checks).
- [ ] CLAUDE.md linting section documents the sharded tidy + the preserved
      "green = clean tree" contract.
