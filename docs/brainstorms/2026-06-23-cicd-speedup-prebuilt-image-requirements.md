# CI/CD Speedup — Reuse the Prebuilt Cross-Compile Image — Requirements

**Date:** 2026-06-23
**Repo:** sst-cam-firmware
**Status:** Ready for planning
**Scope:** Standard (CI pipeline change)

## Problem

PR CI runs take 15–20 min. The cost is not the cross-compile — it is that **every job rebuilds the devcontainer image from scratch**: `format`, `tidy`, and `test` each independently build `ubuntu:24.04` + fetch the ~3 GB L4T r39.2 BSP + `apply_binaries` + 69 noble debs + cross-build (3× the same ~12-min build per run, in parallel). The beta/alpha release jobs rebuild it again.

## Key insight (the reframe)

The cross-compile image is a **stable, build-once artifact**. Its expensive layers change almost never:

- base `ubuntu:24.04` apt → ~never
- `001_fetch_bsp_rootfs.sh` (BSP fetch + apply_binaries, ~3 GB) → only on a new L4T version
- `003_install_extra_pkgs.sh` + `noble-deb-urls.txt` (69 debs) → only when a system dep is added
- `002_fix_sysroot.sh`, build-tools apt, node, conan, npm → rare

**A PR that touches only `src/**` changes ZERO image layers** — the image is rebuilt byte-identical every run for nothing. So the image should be built **once** (when `.devcontainer/**` changes) and **reused unchanged** everywhere else.

## Decision

- **Anchor: reuse a pinned prebuilt image from GHCR.** `.github/workflows/devcontainer-image.yml` already builds + pushes `ghcr.io/scoutsporttechnology/sst-cam-firmware-devcontainer:latest` on `.devcontainer/**` pushes to `main`, with a security model + digest surfaced in the run Summary. This work **un-defers consumption**: the CI jobs pull the pinned image instead of rebuilding.
- **Measure first.** The only real unknown is whether pulling the ~10–15 GB image fits and beats a rebuild on a hosted runner (with `jlumbroso/free-disk-space`). The existing workflow header documents this exact measurement gate. Measurement is the first deliverable; wire-in proceeds only if the pull clearly wins.
- **Fallback: BuildKit GHA layer cache (`type=gha`).** If the GHCR pull ENOSPCs / doesn't beat rebuild, fall back to `cacheFrom/cacheTo=type=gha`. On `src`-only PRs this is a near-100% cache hit (build = cache import + final compile). Accept its non-determinism (10 GB/repo cache, LRU + 7-day eviction → cold cache = full rebuild).
- **Keep the checks parallel.** `format`/`tidy`/`test` stay independent parallel jobs, each pulling/reusing the image. Do NOT collapse them into one sequential job.
- **Path-filter docs/workflow-only PRs.** Skip the heavy jobs entirely when a PR changes no `src/**` / `.devcontainer/**` / build inputs. Branch protection requires these as status checks, so skipped paths must still report green (path-aware required checks or green-stub jobs) — not left "pending".
- **Keep the conan cache** (`~/.conan2` keyed on `conanfile.py`) on all build jobs.

## Goal / value

PR CI for a `src`-only change drops from ~15–20 min (full image rebuild ×3) to roughly the pull + cross-build time; docs/workflow-only PRs finish in well under a minute. The cross-compile image becomes an explicit, pinned, versioned artifact rather than a per-run rebuild.

## Success criteria

- A `src`-only PR runs `format`/`tidy`/`test` without rebuilding the devcontainer image (measured wall-time materially lower than today's ~15–20 min).
- A docs/workflow-only PR skips the heavy jobs and still satisfies branch protection (all required checks green).
- The matrix checks remain parallel.
- The measurement (pull wall-time + peak disk on a hosted runner, post `free-disk-space`) is captured and recorded; the chosen mechanism is justified by it.
- Image consumption is **pinned by `@sha256:` digest**, never `:latest` (honors the `devcontainer-image.yml` security model — a mutable tag reopens the image-poisoning hole).

## Scope Boundaries

- Reuse the existing `devcontainer-image.yml` publisher as-is; do not redesign the publish/security model.
- No self-hosted runner in this work (revisit if hosted pull loses on disk — it would make caching moot).
- No BSP slimming (dropping kernel/DTB from `apply_binaries`) and no `ccache` — incremental, separate.
- Stable promotion (`release.yml`) stays no-build; not in scope beyond confirming it's unaffected.

### Deferred to Follow-Up Work

- Self-hosted runner as the durable answer if the GHCR pull doesn't fit hosted-runner disk.
- `ccache` for cross-compile objects; BSP image slimming.

## Risks / unknowns (for planning to resolve)

- **Disk ENOSPC on pull** — the ~10–15 GB image pull may exhaust a hosted runner even after `free-disk-space`; this is the gating measurement. Mitigations: `docker-images:false` in free-disk, prune before pull. If it fails → gha-cache fallback.
- **Branch protection vs path-filter** — a skipped required check reads as "pending" and blocks merge. Needs path-aware required-check config or always-green stub jobs for the skipped paths.
- **Digest pinning** — consuming `:latest` is forbidden by the publisher's security model; the wire-in must read/pin the published `@sha256:` digest (the auto-commit of the digest was removed, so the pin source must be re-established).
- **Image freshness** — when `.devcontainer/**` changes, the publish (push-to-main) must land before consumers pull, or a `.devcontainer/**` PR's own CI has no up-to-date image to pull (it may need to build locally on that PR). Planning must define the freshness/fallback path.
- **Stale comments** — several workflow comments still say "~37 GB L4T sysroot"; the BSP image is smaller. Update during the change.

## Dependencies / assumptions

- `devcontainer-image.yml` continues to publish on `.devcontainer/**` → `main`; GHCR package is readable by CI (same org, `GITHUB_TOKEN`).
- The image rarely changes (the core premise) — verify by confirming no normal PR touches image-layer inputs.
- Conan cache and `free-disk-space` remain.

## Out of scope

- Self-hosted runners (this iteration).
- Changing the alpha/beta/stable ladder, required-check job names, or the no-build stable promotion.
- ccache / BSP slimming.
