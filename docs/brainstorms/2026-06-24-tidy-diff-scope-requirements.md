# CI Tidy Speedup — Diff-Scope the PR clang-tidy Gate — Requirements

**Date:** 2026-06-24
**Repo:** sst-cam-firmware
**Status:** Ready for planning
**Scope:** Standard (CI pipeline change)

> **Provenance:** Reconstructed 2026-06-25 from session history (`16f32beb`). The
> original doc lived in a now-deleted worktree (`worktree-tidy-diff-scope-brainstorm`)
> and was never committed; this rebuilds the substance — decisions, escape hatches,
> and acceptance criteria — faithfully from the session synthesis, not byte-for-byte.

## Problem

The CI `tidy` job lints **all ~95 translation units** on every PR. Each TU
re-parses the heavy GStreamer / OpenCV headers, so clang-tidy costs ~21 min
**flat — independent of PR size**. A one-line `src/` change pays the same ~21 min
as a tree-wide refactor.

The prebuilt-image work (`2026-06-24-001-feat-cicd-prebuilt-image-plan.md`) removed
the per-job *image rebuild*; it did **not** touch the clang-tidy invocation itself,
which still runs `find src tests \( -name '*.cpp' -o -name '*.cc' \) | xargs clang-tidy`
over the whole tree. That invocation is now the long pole of PR CI.

## Key insight (the reframe)

clang-tidy cost scales with **number of TUs compiled**, and a PR only changes a
handful of `.cpp`/`.cc` files. Linting only the **changed TUs** turns a flat
~21 min into "seconds per changed file" for the common case, while the build that
produces `compile_commands.json` (`cmake --preset test` + `sst_proto`) still runs
every time so the AST is real.

The one thing that breaks naïve diff-scoping is **headers**. `.clang-tidy` uses
`HeaderFilterRegex: '(src|tests)/.*'`, so clang-tidy reports violations in project
headers *while processing a TU that includes them*. If a PR edits a header but you
only relint the changed `.cpp` files, a violation introduced in that header goes
uncaught on any PR that doesn't also touch an including TU. The header-filter hole
is the real tradeoff to close — not a theoretical one.

## Decisions

- **D1 — PR tidy is diff-scoped.** On `pull_request`, lint **only the changed
  `.cpp`/`.cc` translation units** (added/modified relative to the PR base), not
  the whole tree.
- **D1 escape hatch — fall back to a FULL scan when the diff touches anything
  that can invalidate header coverage or the lint config:** any header
  (`*.h` / `*.hpp`), `.clang-tidy`, or `scripts/tidy-args.sh`. A header edit
  re-lints the whole tree (closes the header-filter hole); a `.clang-tidy` /
  `tidy-args.sh` edit re-lints the whole tree (a check-set or flag change must be
  re-triaged against everything).
- **D2 — full all-files tidy runs on PUSH** to `development` / `release/**` as a
  safety net. Even if a diff-scoped PR slips a header violation through (e.g. a
  rare path the escape hatch didn't classify), the push-time full scan catches it
  before the branch is built/released. Alpha and the merge are both covered.
- **Diff base = the PR base SHA via a three-dot diff** (`base.sha...HEAD`),
  reusing the `fetch-depth: 0` checkout + `github.event.pull_request.base.sha`
  the floor-NOLINT guard already relies on. No new checkout cost.
- **The build still runs every time.** `cmake --preset test` +
  `cmake --build --preset test --target sst_proto` are unchanged — they generate
  `compile_commands.json` and the protobuf headers. The savings come **only** from
  how many TUs `clang-tidy` is invoked over, not from skipping the configure/build.

## Goal / value

PR `tidy` for a typical few-file `src/` change drops from a flat ~21 min to roughly
"build + lint N changed TUs" (minutes, not tens of minutes), with **no loss of the
hard-gate guarantee**: a lint violation that can reach `main` is still caught —
on the PR when it's in a changed TU or reachable header, and on push for everything
else.

## Success criteria

- A PR that changes only a few `.cpp`/`.cc` files runs `tidy` materially faster
  than today's ~21 min flat, and lints exactly those changed TUs.
- A PR that edits a header (`*.h`/`*.hpp`), `.clang-tidy`, or `scripts/tidy-args.sh`
  runs a **full** tidy (escape hatch fires).
- A **push** to `development` / `release/**` runs a full all-files tidy (D2 safety
  net) — and this is **verified to actually fire** (see risk R-PUSH below).
- The tidy hard gate is preserved: `WarningsAsErrors` still fails the PR; a new
  violation in a changed TU still blocks the merge.
- No regression to the floor-NOLINT guard (it stays diff-scoped, PR-only).
- The file-selection logic is **unit-tested** (a wrong selection silently skips a
  merge-blocking lint — see R-SELECT).

## Risks

- **R-PUSH (P0) — the push-time safety net must actually run.** The `changes`
  (paths-filter) job is currently `if: github.event_name == 'pull_request'`, and
  `tidy` gates on `needs.changes.outputs.code == 'true'`. If D2 is implemented by
  merely dropping tidy's `pull_request`-only `if:`, then on push `changes` is
  skipped, `needs.changes.outputs.code` is empty, the tidy gate evaluates false,
  and **the entire push-time full scan silently never runs** — and `main` runs no
  tidy at all. D2 requires making `changes` run on push too (or otherwise wiring
  the push gate so it can't no-op). This must be verified with a live push.
- **R-SELECT — a wrong selection branch silently skips a lint.** If the
  changed-file detection mis-classifies (misses a changed TU, or fails to trigger
  the escape hatch on a header edit), a violation merges green. The selection
  logic needs its own self-test, mirroring the floor-guard self-test
  (`scripts/test-check-floor-nolints.sh`).
- **R-DRIFT — two byte-identical tidy blocks.** The tidy job is duplicated
  byte-for-byte across `release-alpha.yml` and `release-beta.yml`. Editing both
  inline risks drift; extract the logic to a shared script.

## Scope Boundaries

- No change to the check set in `.clang-tidy` or to `scripts/tidy-args.sh` flags.
- No change to `format` / `test` / `ci-scripts` jobs.
- No `ccache` / compile caching — orthogonal, separate work.
- Stable promotion (`release.yml`) stays no-build; `main` only promotes (it does
  not run tidy, and this work does not add one there).

### Deferred to Follow-Up Work

- Compile caching (`ccache`) for the build that feeds `compile_commands.json`.
- Tagging hardware-bound tests with `LABELS hardware` (tracked separately; touches
  the `test` job, not `tidy`).
