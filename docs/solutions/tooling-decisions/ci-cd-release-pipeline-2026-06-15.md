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
---

# CI/CD release pipeline: devcontainer cross-build CI + binary release (firmware)

## Context

CI/CD for the C++ firmware, set up alongside proto + app. Cross-cutting design forced by an org constraint: **"Allow GitHub Actions to create and approve PRs" is disabled org-wide** → **release-please unusable** → default `GITHUB_TOKEN` + conventional-commit tag-on-merge (a GitHub App was attempted and abandoned). The firmware-specific challenge: CI must run the **Jetson aarch64 cross-build inside the repo's devcontainer** (custom Bootlin toolchain + ~37GB L4T sysroot), which is heavy and surfaced several runner-environment gotchas.

## Guidance

**Two workflows, default `GITHUB_TOKEN` only:**

- `.github/workflows/ci.yml` — **`pull_request` only**, run inside the devcontainer via `devcontainers/ci`. Required checks: `format` (clang-format) + `test` (cross-build `test` preset + `ctest` under qemu). `tidy` (clang-tidy) runs but is **advisory** (`continue-on-error`).
- `.github/workflows/release.yml` — **`push: main`** + `workflow_dispatch`. Conventional-commit bump → tag + Release, then a gated job cross-builds the production binary and uploads `sst_cam_firmware-<tag>-aarch64`. Also ships `deploy/install.sh` + systemd unit for on-Jetson install.

**Four runner gotchas, all real, all cost cycles:**

1. **ENOSPC on the JetPack image.** A stock hosted runner (~14GB free) can't pull/unpack the ~37GB L4T sysroot → `no space left on device`. Fix: run `jlumbroso/free-disk-space` first to reclaim ~30GB. It then fits (~13min build). If it still ENOSPCs, a **self-hosted runner** is the durable answer.
2. **`devcontainers/ci` ghcr cache tag failure.** Setting `imageName`/`cacheFrom` to `ghcr.io/...` makes the action try `docker tag ... ghcr.io/...` without registry login → fails *after* a successful build. Fix: drop `imageName`/`cacheFrom` (build locally each run; caching deferred).
3. **Hardware-bound tests fail in-container by design.** Tests needing real IMX477/BlueZ/radio/NVENC can't pass in the cross-compile container (per CLAUDE.md). Exclude them: `ctest --preset test -E '<names>'`. Durable fix: tag them with a CTest `LABELS hardware` and use `ctest -LE hardware`.
4. **clang-tidy can't resolve the cross sysroot.** Host clang-tidy parsing the aarch64 `compile_commands.json` errors on every TU ("Error while processing") — it can't find the Bootlin libstdc++/system headers. Fix: pass `--extra-arg=--target=aarch64-linux-gnu --extra-arg=--sysroot=$CROSS_SYSROOT --extra-arg=--gcc-toolchain=<bootlin-root>`. Also: the devcontainer shipped only `clangd`; install `clang-format` + `clang-tidy` explicitly. (tidy is advisory pending a fully-clean cross config.)

**Build flakiness:** the heavy devcontainer build is intermittently flaky on hosted runners — `gh run rerun <id> --failed` (observed `format` fail then pass on identical source). Self-hosted runner removes this.

## Why This Matters

- The devcontainer-in-CI gotchas are firmware-specific and **not discoverable without running the heavy build** — each costs a ~14min iteration. Capturing them turns a multi-hour debug into a lookup.
- `format`/`test` are required checks; `tidy` is advisory so the cross-sysroot config doesn't block merges. Re-tighten tidy (drop `continue-on-error`) once it's clean.
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

- `docs/solutions/tooling-decisions/cpp-rewrite-promotion-to-main-2026-06-09.md` — branch consolidation + proto-submodule pinning (same submodule model this pipeline gates).
- Sibling captures: `sst-cam-proto` and `sst-cam-app` have repo-specific versions under `docs/solutions/tooling-decisions/`. proto's covers the cross-cutting GITHUB_TOKEN/ruleset rationale.
- `CLAUDE.md` → "CI/CD & releasing" section.
