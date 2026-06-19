---
date: 2026-06-17
topic: cicd-workflow-standard
---

# Git + CI/CD Workflow Standard — sst-cam-firmware

## Summary

Refactor this repo's branching, CI/CD, versioning, and docs to the org-wide SST workflow standard: `feat/* → develop → release/X.Y.Z → main`, a test-fidelity maturity ladder (alpha = isolated/automated, beta = integrated on real hardware, stable = shipped), SemVer tags built before merge so `main` never runs a failable job, and a clean version reset to the `0.1.0-alpha` line on the way to a first `0.1.0-beta.1` (firmware+app integration) and an eventual `1.0.0`.

---

## Problem Frame

CI/CD on `main` runs **after** code is already merged: `release.yml` auto-cuts a release on every push to `main`, and `devcontainer-image.yml` builds/publishes and tries to commit a digest back to `main`. A post-merge job that can fail means `main` can hold code whose release/publish broke — incompatible with "`main` = clean, final, released code." There is also no integration branch, no prerelease ladder, and the existing tags (`v0.1.0`, `v0.1.1`) were auto-cut by the old flow and do not correspond to any real, tested release. The repo has many features done but no functional release and no release candidate.

---

## Actors

- A1. **Contributor** — works on `feat/*`/`fix/*` branches, opens PRs into `develop`.
- A2. **Maintainer/admin** (you) — cuts release branches, performs manual beta hardware sign-off, merges the release gate, manages rulesets and tags.
- A3. **CI** — runs checks and builds on PRs and produces tagged artifacts.
- A4. **The mobile app (sst-cam-app)** — the integration counterpart during beta hardware testing (BLE/WiFi).

---

## The Workflow Standard (shared across all four SST repos)

**Branches**
- `feat/*`, `fix/*` — branched off `develop`. Free: no CI/CD while working.
- `develop` — integration trunk, always-green (enforced by the PR gate). New work lands here for the next release.
- `release/X.Y.Z` — short-lived, cut from `develop` when stabilizing a release; deleted after merge to `main`. Lets `develop` keep flowing while a release stabilizes.
- `main` — final released code only. Nothing builds here; it promotes the already-built, signed-off artifact.
- `hotfix/*` — short-lived, off the `main` tag, for an urgent fix when `develop` holds unreleased work.

**Maturity ladder (by test fidelity)**
- **alpha** — validated in *isolation, automatically*. No hardware.
- **beta** — validated in *integration, by hand* (maintainer is the tester). Real artifact, real conditions.
- **stable** — beta signed off and shipped.

**Versions & tags (SemVer 2.0)**
- The **semantic version** is `X.Y.Z[-alpha.N|-beta.N]` — no `v`. The **git tag** is the version with a `v` prefix (tag-name convention only; the `v` is not part of the version).
- `vX.Y.Z-alpha.N` on `develop` (auto) → `vX.Y.Z-beta.N` on `release/X.Y.Z` (gated/manual) → `vX.Y.Z` on `main` (stable). Order: `-alpha.N` < `-beta.N` < stable.
- Pre-1.0 (`0.MINOR.PATCH`): minor = feature, patch = fix, no stability guarantee. `1.0.0` = first real stable release. Post-1.0: **major = breaks the app/wire (proto) contract or config schema**, minor = backward-compatible capability, patch = backward-compatible fix.

**The two non-negotiable rules**
- **Build-in-PR / tag-on-merge.** The failable build runs *before* a merge (it is part of the PR checks). A merge only tags/promotes already-validated code. `main` never builds.
- **`main`'s checks are gates, not re-runs.** Promotion to `main` requires the release branch's checks to be green; those jobs ran upstream, not on `main`.

**Flow**
```
feat/* ─PR: format·tidy·test (incl. compile)─► develop ─auto─► tag vX.Y.Z-alpha.N
develop ─cut─► release/X.Y.Z ─build real artifact + tag vX.Y.Z-beta.N─► manual hardware sign-off
release/X.Y.Z ─PR (beta checks green)─► main ─► tag vX.Y.Z + publish beta artifact ; delete release branch ; merge back to develop
hotfix: off main tag → fix → vX.Y.(Z+1) → main → back to develop
```

**Release trigger** — automated bump from Conventional Commits + one human gate (maintainer merges the release). Beta→stable sign-off is manual for now (hardware test); automation of the bump/changelog is adopted, the ship decision stays human.

---

## This repo's specifics

- **Artifact:** cross-compiled aarch64 binary (`sst_cam_firmware-<version>-aarch64`), installed on a Jetson Orin Nano via `deploy/install.sh`. Built only in the devcontainer (cross-build).
- **alpha** = cross-build in the devcontainer + container `ctest` (hardware-bound tests are excluded/expected-fail per CLAUDE.md). Isolated, automated.
- **beta** = the binary flashed to a real Jetson Orin Nano and tested **with the app** over BLE/WiFi (the firmware↔app contract working end-to-end). Manual maintainer sign-off.

---

## Key Flows

- F1. **Feature → develop (alpha).** Trigger: contributor opens PR `feat/x → develop`. Actors: A1, A3. Steps: CI runs format·tidy·test (cross-build is part of test); green + review → merge → CI tags `vX.Y.Z-alpha.N` and publishes the alpha binary. Outcome: develop stays green; an installable alpha exists. Covered by: R1, R2, R5, R8.
- F2. **Cut release candidate (beta).** Trigger: maintainer decides to stabilize `X.Y.Z`. Actors: A2, A3. Steps: cut `release/X.Y.Z` from develop; CI builds the release binary + tags `vX.Y.Z-beta.N`; maintainer flashes a Jetson and tests with the app; fixes land on the release branch → `-beta.2`… until sign-off. Outcome: a hardware-validated candidate. Covered by: R3, R4, R6, R9.
- F3. **Promote to stable.** Trigger: beta signed off. Actors: A2, A3. Steps: PR `release/X.Y.Z → main` (requires beta checks green); merge → tag `vX.Y.Z` + publish the already-built binary; delete release branch; merge back to develop. Outcome: `main` == `vX.Y.Z`, no build ran on main. Covered by: R3, R7, R10.

---

## Requirements

**Branch model & protection**
- R1. Create a long-lived `develop` branch; make it the default integration target for `feat/*`/`fix/*`.
- R2. Rulesets: `develop` requires PR + green checks; `main` requires PR + green required-status-checks + no direct push (admin/hotfix bypass only); `release/*` requires the release checks. `main` requires `tidy`, `format`, `test` (and the build/artifact checks) to be green before merge.
- R3. `main` runs **no failable build/publish job**. Anything that builds or publishes runs on PRs / `develop` / `release/*`, never as a post-merge job on `main`.

**CI/CD pipelines**
- R4. Rework `ci.yml` to run the checks on PRs into `develop` (and on `release/*`), not gated to `main` PRs only.
- R5. On merge to `develop`, auto-build + tag `vX.Y.Z-alpha.N` and publish the alpha binary (build-in-PR; the merge tags/publishes already-validated code).
- R6. On `release/X.Y.Z`, build the release binary and tag `vX.Y.Z-beta.N`; betas iterate on the branch.
- R7. Replace `release.yml`'s "auto-cut stable on every push to `main`" with the release-branch→main promotion: tagging `vX.Y.Z` + publishing the already-built beta artifact, no rebuild on `main`.
- R8. Adopt Conventional Commits as the automated version-bump source across the repo.
- R9. `devcontainer-image.yml`: stop auto-committing the digest to `main`; keep GHCR image caching **deferred / measurement-gated** (CI keeps the per-run build until a measured pull beats it). No publish job may push to `main`.

**Versioning reset**
- R10. Delete the existing bogus tags (`v0.1.0`, `v0.1.1`) and their GitHub releases; re-establish the clean scheme starting at the `0.1.0-alpha` line.
- R11. Consolidate the work done so far under `0.1.0-alpha.N`; the immediate target is `0.1.0-beta.1` (first firmware+app hardware integration test). `1.0.0` is the eventual first stable release (not yet, no candidate).

**Documentation**
- R12. Update `CLAUDE.md` ("CI/CD & releasing", "Branch + commit + tag rules"), `README`, and `deploy/README.md` to describe the new branch model, ladder, tag/version convention, and release flow.

---

## Acceptance Examples

- AE1. *When a PR is opened into `develop`*, format·tidy·test (incl. cross-build) run and must be green before merge. Covers: R1, R4.
- AE2. *When a commit merges to `develop`*, CI tags `vX.Y.Z-alpha.N` and publishes the alpha binary — without re-running the build that already passed in the PR where feasible. Covers: R5, R3.
- AE3. *When `release/X.Y.Z` receives a commit*, CI builds the release binary and tags `vX.Y.Z-beta.N`. Covers: R6.
- AE4. *When a `release/X.Y.Z → main` PR is opened with red beta checks*, the merge is blocked. Covers: R2, R3.
- AE5. *When the release PR merges to `main`*, `vX.Y.Z` is tagged and the already-built beta artifact is published — no build job runs on `main`. Covers: R7, R3.

---

## Success Criteria

- `main` has zero failable build/publish jobs; every red CI happens before a merge.
- A contributor can go feat → develop(alpha) → release(beta) → main(stable) following only the documented flow.
- Tags/releases are clean and SemVer-correct; `0.1.0-beta.1` can be cut and hardware-tested with the app.
- The same branch/ladder/tag model is in force here as in app, proto, emulator (only the artifact and alpha/beta definitions differ).

---

## Scope Boundaries

- No alpha/beta external-tester cohorts; no nightly channel (dropped).
- No long-lived maintenance/release branches; no backporting to old majors (only the latest release is supported).
- Not cutting `1.0.0` (future; no candidate yet).
- GHCR image-cache wire-in to CI stays deferred (measurement-gated); keep the per-run devcontainer build.
- Implementation specifics — exact workflow YAML, ruleset JSON, release-please/semantic-release config — belong in the per-repo plan, not here.

---

## Key Decisions

- Build-in-PR / tag-on-merge; `main` never builds (the core fix).
- Short-lived `release/X.Y.Z` branch (not a tag) so `develop` keeps flowing during stabilization.
- alpha/beta defined by test fidelity (isolated/automated vs integrated/on-hardware), not by audience.
- SemVer version `X.Y.Z`; git tag `vX.Y.Z` (the `v` is a tag prefix only).
- Reset all existing tags/releases; start at `0.1.0-alpha`.

---

## Dependencies / Cross-repo Coordination

- **proto** is the wire contract; a proto major bump forces a major here. firmware consumes proto as a submodule.
- **app** is the beta integration counterpart — `0.1.0-beta.1` is a joint firmware+app hardware test, so the firmware and app plans likely run **hand-in-hand / simultaneously**.
- Each repo gets its own plan; coordinate the firmware+app beta milestone.

---

## Outstanding Questions

- Automating the beta→stable bump/changelog (release-please vs semantic-release) — chosen at plan time; manual ship decision stays.
- Exact ruleset bypass model for the digest/no-op jobs and hotfix pushes.
