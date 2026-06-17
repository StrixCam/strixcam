---
title: "CI/CD release pipeline: devcontainer cross-build CI + binary release (firmware)"
date: 2026-06-15
category: tooling-decisions
module: ci-cd
problem_type: tooling_decision
component: tooling
severity: medium
applies_when:
  - "Setting up or changing GitHub Actions CI/CD for the firmware"
  - "Running the Jetson cross-build devcontainer on a hosted runner"
  - "Wiring clang-tidy or hardware-bound tests into CI"
tags: [ci-cd, github-actions, devcontainer, cross-compile, clang-tidy, jetson, conventional-commits]
related_components: [development_workflow, tooling]
last_updated: 2026-06-16
---

# CI/CD release pipeline: devcontainer cross-build CI + binary release (firmware)

## Context

CI/CD for the C++ firmware, set up alongside proto + app. Cross-cutting design forced by an org constraint: **"Allow GitHub Actions to create and approve PRs" is disabled org-wide** → **release-please unusable** → default `GITHUB_TOKEN` + conventional-commit tag-on-merge (a GitHub App was attempted and abandoned). The firmware-specific challenge: CI must run the **Jetson aarch64 cross-build inside the repo's devcontainer** (custom Bootlin toolchain + ~37GB L4T sysroot), which is heavy and surfaced several runner-environment gotchas.

## Guidance

**Two workflows, default `GITHUB_TOKEN` only:**

- `.github/workflows/ci.yml` — **`pull_request` only**, run inside the devcontainer via `devcontainers/ci`. Required checks: `format` (clang-format) + `test` (cross-build `test` preset + `ctest` under qemu) + `tidy` (clang-tidy). **Update 2026-06-16:** `tidy` is now a **hard gate** — the cross-sysroot header discovery was fixed (gotcha #4), the tree was driven to zero, and `.clang-tidy` sets `WarningsAsErrors: '*'` with no `continue-on-error`. (The branch-protection ruleset must list `tidy` as required alongside `format`/`test`.)
- `.github/workflows/release.yml` — **`push: main`** + `workflow_dispatch`. Conventional-commit bump → tag + Release, then a gated job cross-builds the production binary and uploads `sst_cam_firmware-<tag>-aarch64`. Also ships `deploy/install.sh` + systemd unit for on-Jetson install.

**Four runner gotchas, all real, all cost cycles:**

1. **ENOSPC on the JetPack image.** A stock hosted runner (~14GB free) can't pull/unpack the ~37GB L4T sysroot → `no space left on device`. Fix: run `jlumbroso/free-disk-space` first to reclaim ~30GB. It then fits (~13min build). If it still ENOSPCs, a **self-hosted runner** is the durable answer.
2. **`devcontainers/ci` ghcr cache tag failure.** Setting `imageName`/`cacheFrom` to `ghcr.io/...` makes the action try `docker tag ... ghcr.io/...` without registry login → fails *after* a successful build. Fix: drop `imageName`/`cacheFrom` (build locally each run). **Update 2026-06-16:** a digest-pinned GHCR publish workflow now exists — `.github/workflows/devcontainer-image.yml` (push to main on `.devcontainer/**`, no `pull_request`, job-level `packages: write`) WITH the missing `docker/login-action` step that was the root cause. CI consumers are **not yet wired** to pull it: the ~37GB pull on a hosted runner may ENOSPC or lose to the per-run rebuild, so rewiring `imageName`/`cacheFrom` is **measurement-gated** (per-run build stays as the rollback).
3. **Hardware-bound tests fail in-container by design.** Tests needing real IMX477/BlueZ/radio/NVENC can't pass in the cross-compile container (per CLAUDE.md). Exclude them: `ctest --preset test -E '<names>'`. Durable fix: tag them with a CTest `LABELS hardware` and use `ctest -LE hardware`.
4. **clang-tidy can't resolve the cross sysroot.** Host clang-tidy parsing the aarch64 `compile_commands.json` errors on every TU ("Error while processing") — it can't find the Bootlin libstdc++/system headers. **Update 2026-06-16 (corrected fix):** the working fix is the **matched** triple `--target=aarch64-buildroot-linux-gnu` + `--gcc-toolchain=<bootlin-root>` (+ `--sysroot=$CROSS_SYSROOT`, `-Qunused-arguments`). The earlier `--target=aarch64-linux-gnu` was wrong — the mismatched triple defeats clang's gcc auto-detection so libstdc++ stays invisible. Flags are now centralized in `scripts/tidy-args.sh` (one source of truth for the CI `tidy` job + the dev-side `scripts/fix.sh`). Install `clang-format` + `clang-tidy-14` explicitly in the Dockerfile (the base ships only `clangd`); clang-tidy is **version-locked to clang-tidy-14**. See `docs/solutions/tooling-decisions/clang-tidy-silent-gate-gaps-2026-06-16.md` for the subtle ways this gate can still read clean while under-enforcing.

**Build flakiness:** the heavy devcontainer build is intermittently flaky on hosted runners — `gh run rerun <id> --failed` (observed `format` fail then pass on identical source). Self-hosted runner removes this.

## Why This Matters

- The devcontainer-in-CI gotchas are firmware-specific and **not discoverable without running the heavy build** — each costs a ~14min iteration. Capturing them turns a multi-hour debug into a lookup.
- `format`/`test`/`tidy` are all required checks now. (`tidy` was advisory until 2026-06-16 while the cross-sysroot config was being fixed; that re-tightening is done — see gotcha #4 and the Update on the `ci.yml` bullet above.)
- Squash-merge subject must be conventional or `release.yml` cuts no release.

## When to Apply

- Editing `ci.yml` / `release.yml`, the devcontainer, or test/lint gating.
- Diagnosing a red firmware CI run (check ENOSPC / ghcr-tag / hardware-test / clang-tidy-sysroot first — likely environment, not code).
- Bumping the proto contract: it's a git submodule — `cd proto && git checkout vX.Y.Z && cd .. && git add proto && commit`.

## Examples

**Release manually / seed first tag** (auto-scan skips on empty tag history; works once `v0.1.0` exists):

```bash
gh workflow run release.yml -R ScoutSportTechnology/sst-cam-firmware -f bump=minor
```

**On-Jetson install/update:** `deploy/install.sh` (fail-before-stop, idempotent) — see `deploy/README.md`.

## Related

- `docs/solutions/tooling-decisions/clang-tidy-silent-gate-gaps-2026-06-16.md` — follow-up: the three silent ways the `tidy` hard gate (gotcha #4) can still read 0-warnings-clean while under-enforcing, with an adversarial verification protocol.
- `docs/bugs/2026-06-16-swappable-params-floor-nolint.md` — open bug surfaced by the hard gate: the FLOOR `bugprone-easily-swappable-parameters` check `// NOLINT`-suppressed on genuinely transposable production signatures.
- `docs/plans/2026-06-15-001-fix-clang-tidy-hard-gate-plan.md` — the plan that turned `tidy` advisory→hard (U1–U7).
- `docs/solutions/tooling-decisions/cpp-rewrite-promotion-to-main-2026-06-09.md` — branch consolidation + proto-submodule pinning (same submodule model this pipeline gates).
- Sibling captures: `sst-cam-proto` and `sst-cam-app` have repo-specific versions under `docs/solutions/tooling-decisions/`. proto's covers the cross-cutting GITHUB_TOKEN/ruleset rationale.
- `CLAUDE.md` → "CI/CD & releasing" section.
