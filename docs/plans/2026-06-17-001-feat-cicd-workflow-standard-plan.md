---
title: "feat: Git + CI/CD workflow standard — sst-cam-firmware"
type: feat
status: active
date: 2026-06-17
origin: docs/brainstorms/2026-06-17-cicd-workflow-standard-requirements.md
---

# feat: Git + CI/CD workflow standard — sst-cam-firmware

## Summary

Refactor this repo's three-workflow pipeline into the org-wide SST branch model (`feat/* → develop → release/X.Y.Z → main`) with the test-fidelity maturity ladder (alpha = devcontainer cross-build + container `ctest` in isolation, beta = the aarch64 binary flashed to a real Jetson and tested with the app, stable = shipped). The two structural fixes: (1) move the failable aarch64 build off `main` so `main` only *promotes* the already-built beta binary, and (2) stop `devcontainer-image.yml` from committing the image digest back to `main`. Add SemVer prerelease tags, adopt the shared `resolve-version.sh`, and reset the bogus `v0.1.0`/`v0.1.1` tags to a clean `0.1.0-alpha` line toward `0.1.0-beta.1`. CI keeps running inside the devcontainer; this is a pipeline + governance refactor, no firmware C++ changes.

---

## Problem Frame

Two post-merge jobs run failable work *on* `main`: `release.yml` auto-cuts a release and cross-builds the aarch64 binary on every push to `main`, and `devcontainer-image.yml` builds the GHCR image and commits the digest back to `main`. A post-merge job that can fail means `main` can hold code whose release/publish broke — incompatible with "`main` = clean, final, released code." There is also no integration branch, no prerelease ladder, and the existing tags (`v0.1.0`, `v0.1.1`) were auto-cut by the old flow and correspond to no real, tested release. (see origin: docs/brainstorms/2026-06-17-cicd-workflow-standard-requirements.md)

---

## Requirements

- R1. Create a long-lived `develop` branch; make it the default integration target.
- R2. Rulesets: `develop` PR + green checks; `main` PR + green required checks + no direct push (admin/hotfix bypass); `release/*` requires the release checks. `main` requires `tidy`, `format`, `test` (and the build/artifact checks) green.
- R3. `main` runs **no failable build/publish job**.
- R4. Rework `ci.yml` to run format·tidy·test on PRs into `develop` (and `release/*`), not gated to `main` PRs only.
- R5. On merge to `develop`, auto-build + tag `vX.Y.Z-alpha.N` and publish the alpha binary.
- R6. On `release/X.Y.Z`, build the release binary + tag `vX.Y.Z-beta.N`; betas iterate.
- R7. Replace `release.yml`'s auto-cut-on-push-to-`main` with release-branch→main promotion: tag `vX.Y.Z` + publish the already-built beta binary, no rebuild on `main`.
- R8. Adopt Conventional Commits as the automated bump source.
- R9. `devcontainer-image.yml`: stop auto-committing the digest to `main`; GHCR image caching stays deferred/measurement-gated; no publish job may push to `main`.
- R10. Delete the bogus tags (`v0.1.0`, `v0.1.1`) + their releases; re-establish the clean scheme at `0.1.0-alpha`.
- R11. Consolidate work under `0.1.0-alpha.N`; immediate target `0.1.0-beta.1` (joint firmware+app hardware test). `1.0.0` = eventual first stable.
- R12. Update `CLAUDE.md`, `README`, and `deploy/README.md` to the new branch model, ladder, tag/version convention, and flow.

**Origin actors:** A1 Contributor, A2 Maintainer/admin, A3 CI, A4 sst-cam-app (beta BLE/WiFi counterpart).
**Origin flows:** F1 Feature→develop (alpha), F2 Cut release candidate (beta), F3 Promote to stable.
**Origin acceptance examples:** AE1 (R1,R4), AE2 (R5,R3), AE3 (R6), AE4 (R2,R3), AE5 (R7,R3).

---

## Scope Boundaries

- No external-tester cohorts; no nightly channel.
- No maintenance/backport branches (latest-only-supported).
- Not cutting `1.0.0`.
- No firmware C++ / build-system changes — pipeline, rulesets, scripts, docs only.
- GHCR image-cache wire-in to CI stays deferred (measurement-gated); keep the per-run devcontainer build.
- No change to proto consumption (git submodule).

### Deferred to Follow-Up Work

- GHCR image caching consumed by CI: stays measurement-gated (per-run build remains) — not this plan.
- Self-hosted runner for the ~37 GB JetPack build: operational, separate decision.
- Alpha build artifact-reuse optimization (promote the PR's already-built binary instead of rebuilding on develop merge): deferred; cross-workflow artifact passing is fiddly and the build is heavy — note, don't block.
- `0.1.0-beta.1` hardware sign-off itself: runs hand-in-hand with the app plan.

---

## Context & Research

### Relevant Code and Patterns

- `.github/workflows/ci.yml` — format·tidy·test, each inside `devcontainers/ci`, gated to `pull_request: [main]`. Job names `format`/`tidy`/`test` are the wired required checks — keep stable. Retarget the trigger to `develop`/`release/*`.
- `.github/workflows/release.yml` — `push: [main]` + dispatch; hand-rolled conventional-commit bump → tag → cross-build aarch64 → upload. This builds on `main` (R3 violation) — split into alpha/beta/promote.
- `.github/workflows/devcontainer-image.yml` — `push: [main]` touching `.devcontainer/**`; builds + pushes the GHCR image and **commits the digest back to `main`** (R9 violation). Drop the push-to-main commit; retarget off `main`.
- The version-resolution bash in `release.yml` (lines 44–88) is the pattern to extract into `scripts/ci/resolve-version.sh`.

### Institutional Learnings

- `tidy` is a **hard gate** now (cross-toolchain flags in `scripts/tidy-args.sh`, `.clang-tidy` `WarningsAsErrors: '*'`, version-locked clang-tidy-14). Preserve across the retarget. (CLAUDE.md; memory: clang-tidy-hard-gate-followups)
- Bump tool = hand-rolled bash (release-please removed org-wide). Default `GITHUB_TOKEN` only; "Release Tags" ruleset permits compliant tag creation. (memory: cicd-pipeline-plan)
- The JetPack devcontainer build is flaky on hosted runners (~37 GB disk; `jlumbroso/free-disk-space` reclaims ~30 GB); re-run `gh run rerun <id> --failed` on transient ENOSPC. Heavy build (~13–14 min). (memory: cicd-pipeline-plan; ci.yml header)
- Proto stays a submodule; CI uses `submodules: recursive`. (memory: cicd-pipeline-plan)

### External References

- SemVer 2.0 prerelease precedence; `git tag --sort=-v:refname` for numeric counter selection.

---

## Key Technical Decisions

- **Keep `ci.yml`'s three devcontainer jobs; only retarget the trigger.** The cross-build correctness lives in the devcontainer (Bootlin toolchain, L4T sysroot prep, host-protoc codegen, Conan cache) — do not re-derive it. Change `pull_request.branches` from `[main]` to `[develop, release/**]` and nothing else structural in `ci.yml`.
- **"main never builds" via beta-binary hand-off.** The aarch64 binary built on `release/*` is uploaded as the asset on the `vX.Y.Z-beta.N` prerelease Release. `promote.yml` on `main` **downloads and re-uploads that exact binary** to the `vX.Y.Z` stable Release — no cross-build runs on `main`. Same artifact-identity guarantee as the app plan.
- **`devcontainer-image.yml` stops *committing to* `main`, but keeps `push:[main]` as its trigger.** Drop only the "Record digest on main" commit/push step (+ its `contents: write`) — that is the entire R9 violation. **Do NOT retarget the publish trigger to `develop`**: `main` is the most-protected branch (no direct push, admin-only bypass), and publishing the GHCR image from a less-protected `develop` would widen the image-poisoning trust boundary — any actor able to push to `develop` could publish a poisoned `:latest` that every CI job then builds from. Trigger retarget is separable from R9 and unnecessary, so leave it on `main`. The digest is still surfaced in the run `Summary` for a manual bump. Image-cache consumption by CI stays deferred/measurement-gated. **When the cache wire-in is eventually un-deferred, it must re-establish digest pinning first** (pin by `@sha256:` digest, never the mutable `:latest`) — record this obligation so the dropped digest record doesn't reopen the mutable-tag poisoning hole. Satisfies R9 (no push to main) while preserving both the publish capability and the security model. (R3 scope: R3 governs the *product* build/publish — the release artifact cut as a post-merge step. The devcontainer image is CI infrastructure, fires only on `.devcontainer/**` changes, gates no merge, and is the pre-existing measurement-gated behavior R9 explicitly preserved — so keeping it on `main` push is consistent with R3's intent, not a violation.)
- **Extract `scripts/ci/resolve-version.sh`** (alpha/beta/stable) — same contract as the app/emulator plans; isolates and tests the prerelease counter math.
- **Four workflows total**: `ci.yml` (retargeted — format/tidy/test), `alpha.yml` (new — develop → **build+tag+publish** alpha binary), `release-beta.yml` (new — release/* → build+tag+publish beta binary), `promote.yml` (new — main → retag+copy). `release.yml` deleted. (`devcontainer-image.yml` is fixed in-place by U6, separate from these four.)
- **`main`'s required check is the release branch's check reused on the shared head SHA.** A `release/X.Y.Z → main` PR's head SHA *is* the release-branch tip, which already carries green `format`/`tidy`/`test` from its `release/*` push. GitHub surfaces those runs on the PR, so the `main` ruleset requires them with **no devcontainer cross-build re-running on `main`** — the R3/AE5 mechanism. `ci.yml` is deliberately *not* on `main` PRs (the cross-build is heavy). Caveat to verify: confirm GitHub surfaces a push-event check run as a PR status on the same SHA; if not, add a lightweight `pull_request: [main]` gate that only asserts the `vX.Y.Z-beta.N` Release/asset exists (no cross-build). Resolve before wiring U7's `main` ruleset.
- **Version reset = one-time maintainer runbook** deleting `v0.1.0` + `v0.1.1` and seeding `0.1.0-alpha`.

---

## Open Questions

### Resolved During Planning

- Bump/changelog automation tool? → Hand-rolled bash extended (release-please stays removed).
- How does `main` get the binary without building? → Copies the `vX.Y.Z-beta.N` Release asset.
- How to satisfy R9 without losing image publishing? → Drop **only** the main-commit step; **keep** the publish trigger on `push:[main]` (moving it to less-protected `develop` would widen the image-poisoning surface); keep caching deferred. R9 ("no publish job may push to main") forbids the *git push* to main, not the image build triggered by a main push.

### Deferred to Implementation

- Whether `promote.yml` automates "delete release branch + merge back to develop" or documents it as a maintainer action.
- Ruleset bypass model for the no-op/hotfix pushes (admin bypass assumed).
- Alpha build artifact-reuse vs rebuild on develop merge (rebuild for now; optimize later).
- Test harness wiring for `resolve-version.sh`.

---

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

```
feat/* ──PR──► ci.yml [format · tidy · test] (devcontainer cross-build) ──green+review──► develop
develop ──push──► alpha.yml ──► resolve-version.sh alpha ──► cross-build aarch64 ──► tag vX.Y.Z-alpha.N ──► publish alpha binary (prerelease)
develop ──cut──► release/X.Y.Z ──push──► release-beta.yml ──► cross-build aarch64 ──► tag vX.Y.Z-beta.N ──► publish beta binary (prerelease asset)
                                            │
                                  flash a real Jetson + test with the app (A4) ──► manual sign-off
                                            │
release/X.Y.Z ──PR (beta checks green)──► main ──push──► promote.yml ──► tag vX.Y.Z ──► copy beta binary to stable Release  (NO cross-build on main)

devcontainer-image.yml: push:develop touching .devcontainer/** → build+push GHCR image (NO commit to main); caching consumption deferred
```

---

## Implementation Units

- U0. **Bootstrap the branch model (prerequisite — do before all other units)**

**Goal:** Create `develop` and make it the default branch so the retargeted workflows and rulesets have a branch to key off.

**Requirements:** R1

**Dependencies:** None — first step; U2, U3, U4, U6, U7 depend on it.

**Files:**
- None (one-time `git` + `gh` operations; documented in `docs/ci/rulesets.md`)

**Approach:**
- Cut `develop` from `main` and push: `git switch -c develop main && git push -u origin develop`.
- Land the new/retargeted workflow files on `develop` first.
- Open one throwaway PR into `develop` so `ci.yml` emits its `format`/`tidy`/`test` check runs **once** — capture the exact names for U7's `required_status_checks` wiring (the JetPack build is heavy/flaky, so re-run with `gh run rerun --failed` if needed to get a clean first run).
- Flip the GitHub default branch: `gh api repos/:owner/:repo -X PATCH -f default_branch=develop`.
- Strict ordering: bootstrap → first CI run (capture names) → apply rulesets (U7 last).

**Test scenarios:**
- Test expectation: none (one-time git/gh setup) — verification below.

**Verification:** `develop` exists, is the repo default, and a PR into it triggers `format`/`tidy`/`test`; rulesets applied only after the first run captured check names.

---

- U1. **Add `scripts/ci/resolve-version.sh` + tests (version math)**

**Goal:** One tested script for next alpha/beta/stable tag.

**Requirements:** R5, R6, R7, R8

**Dependencies:** None

**Files:**
- Create: `scripts/ci/resolve-version.sh`
- Create: `scripts/ci/resolve-version-test.sh` (or `.bats`)

**Approach:** Same contract as the app/emulator plans — modes `alpha|beta|stable`, conventional-commit base bump from the latest stable tag, numeric prerelease counter. Reuse the existing `release.yml` bump bash. Carry forward `release.yml`'s `IN_VERSION`/`IN_BUMP` override inputs so the first post-reset alpha can be seeded deterministically (sidesteps the "No releasable since v0.0.0" full-history-scan anomaly); with no tags + no override, `alpha` bumps from implicit `v0.0.0`.

**Execution note:** Test-first — deterministic given a tag list.

**Patterns to follow:** `release.yml` lines 44–88; `sst-cam-app` U1.

**Test scenarios:**
- Happy path: no stable tags (post-reset) + `feat:`, mode `alpha` → `v0.1.0-alpha.1`. Covers AE2.
- Happy path: `[v0.1.0-alpha.1]` + `feat:` merge → `v0.1.0-alpha.2`.
- Happy path: mode `beta v0.1.0` → `v0.1.0-beta.1`; with `[v0.1.0-beta.1]` → `v0.1.0-beta.2`. Covers AE3.
- Happy path: mode `stable v0.1.0` → `v0.1.0`.
- Edge case: numeric precedence `-alpha.10` > `-alpha.9` → `.11`.
- Edge case: docs/chore-only → `released=false` (skip).
- Edge case: `BREAKING`/`type!:` → major bump base.
- Error path: invalid mode/base → non-zero exit.

**Verification:** Test suite green; correct tag per fixture.

---

- U2. **Retarget `ci.yml` to `develop`/`release/*`**

**Goal:** format·tidy·test gate PRs into the new branches.

**Requirements:** R3, R4

**Dependencies:** U0

**Files:**
- Modify: `.github/workflows/ci.yml`

**Approach:** Change `on.pull_request.branches` from `[main]` to `[develop, release/**]`. Keep the three devcontainer jobs, the floor-NOLINT guard, Conan cache, free-disk step, and the `format`/`tidy`/`test` job names unchanged (they remain the required checks). No other structural change.

**Patterns to follow:** existing `ci.yml`.

**Test scenarios:**
- Happy path: PR `feat/x → develop` → format·tidy·test (incl. cross-build) run and gate merge. Covers AE1.
- Happy path: PR into `release/0.1.0` → same checks.
- Edge case: PR into `main` no longer triggers `ci.yml`.
- Integration: a red tidy/test blocks merge into `develop`.

**Verification:** `develop`/`release/*` PRs run the three checks; `main` PRs don't.

---

- U3. **Add `alpha.yml` — cross-build + tag + publish alpha binary on push to `develop`**

**Goal:** Merge to `develop` auto-tags `vX.Y.Z-alpha.N` + publishes the aarch64 alpha binary.

**Requirements:** R5, R3, R8

**Dependencies:** U0, U1

**Files:**
- Create: `.github/workflows/alpha.yml`

**Approach:** `on.push.branches: [develop]` + `workflow_dispatch`. `resolve-version.sh alpha`; if `released=true`, cross-build via `devcontainers/ci` (free-disk + Conan cache + `submodules: recursive`, as `release.yml`'s build job), `gh release create vX.Y.Z-alpha.N --prerelease`, upload `sst_cam_firmware-<tag>-aarch64`. `permissions: contents: write`.

**Patterns to follow:** `release.yml` `build-and-upload` job (free-disk, Conan cache, `devcontainers/ci` runCmd, `softprops/action-gh-release`).

**Test scenarios:**
- Happy path: `feat:` merge to `develop` → `v0.1.0-alpha.1` prerelease with the aarch64 binary. Covers AE2.
- Edge case: docs/chore-only merge → skip, green.
- Edge case: second `feat:` merge → `v0.1.0-alpha.2`.
- Integration: cross-build runs here, never on main (R3).
- Edge case: transient ENOSPC/flake on the JetPack build → `gh run rerun --failed` recovers (documented, not a code fix).

**Verification:** A `feat:` merge yields a `-alpha.N` prerelease with the binary; non-releasable merges produce none.

---

- U4. **Add `release-beta.yml` — build + tag + publish beta binary on `release/*`**

**Goal:** Pushes to `release/X.Y.Z` cross-build the binary, tag `vX.Y.Z-beta.N`, publish it as the prerelease asset `main` later promotes.

**Requirements:** R6, R3

**Dependencies:** U0, U1

**Files:**
- Create: `.github/workflows/release-beta.yml`

**Approach:** `on.push.branches: [release/**]` + `workflow_dispatch`. Base `X.Y.Z` from branch name; `resolve-version.sh beta X.Y.Z`; cross-build; `gh release create vX.Y.Z-beta.N --prerelease`; upload the aarch64 binary asset.

**Patterns to follow:** `release.yml` `build-and-upload` job.

**Test scenarios:**
- Happy path: push to `release/0.1.0` → `v0.1.0-beta.1` with the aarch64 binary. Covers AE3, F2.
- Happy path: a fix pushed to the branch → `v0.1.0-beta.2`.
- Edge case: branch name not matching `release/X.Y.Z` → fail fast.
- Integration: the beta binary asset is retrievable by tag (promote depends on it).

**Verification:** A `release/0.1.0` push produces a `v0.1.0-beta.N` prerelease carrying the binary.

---

- U5. **Add `promote.yml` — tag stable + copy beta binary on push to `main` (no build); delete `release.yml`**

**Goal:** `release/X.Y.Z → main` tags `vX.Y.Z` and publishes the already-built beta binary by copying — zero cross-build on `main`.

**Requirements:** R7, R3

**Dependencies:** U4

**Files:**
- Create: `.github/workflows/promote.yml`
- Delete: `.github/workflows/release.yml`

**Approach:**
- `on.push.branches: [main]` + `workflow_dispatch`. Derive `X.Y.Z` from the merged `release/X.Y.Z` branch name, then select the source beta tag explicitly: `git tag -l "vX.Y.Z-beta.*" --sort=-v:refname | head -1` (highest `-beta.N`); fail fast if none matches. `resolve-version.sh stable X.Y.Z` for the stable tag.
- `gh release create vX.Y.Z --generate-notes`; `gh release download <beta-tag> --dir dist`; **rename** `sst_cam_firmware-<beta-tag>-aarch64` → `sst_cam_firmware-vX.Y.Z-aarch64` (bytes preserved); re-upload to the stable Release. **No `devcontainers/ci` / cross-build step exists in this workflow** — the structural R3/AE5 guarantee.
- **Integrity check before re-upload:** compare the downloaded binary's SHA-256 against a digest `release-beta.yml` records (e.g. in the beta Release notes). Tag immutability protects the *tag*, not the Release *asset* — a `--clobber` upload could swap the asset; the digest compare makes promotion a verified hand-off, not trust-on-download.
- Document deleting the release branch + merge-back to develop; if not automated, emit a run-summary reminder so `develop` never silently diverges below `main`.

**Patterns to follow:** `gh release create` + asset download/upload.

**Test scenarios:**
- Happy path: merge `release/0.1.0 → main` → `v0.1.0` tagged, beta binary on the stable Release. Covers AE5.
- Edge case: no matching beta Release → fail fast (never silently rebuild).
- Edge case: workflow contains no `devcontainers/ci`/build step — assert by inspection. Covers R3.
- Integration: promoted binary bytes equal the beta binary.

**Verification:** `main` has zero failable build jobs; the stable Release reuses the beta binary.

---

- U6. **Fix `devcontainer-image.yml` — stop committing the digest to `main`**

**Goal:** No publish job pushes to `main`; image caching stays deferred.

**Requirements:** R9, R3

**Dependencies:** None

**Files:**
- Modify: `.github/workflows/devcontainer-image.yml`

**Approach:** Remove **only** the "Record digest on main" commit/push step + its `contents: write` (the `Summary` step already surfaces the digest for a manual bump). **Keep `on.push.branches: [main]`** (+ `workflow_dispatch`, still pathed to `.devcontainer/**`) — do NOT retarget to `develop`. Rationale (security): `main` is the most-protected branch; publishing the GHCR `:latest` image from a less-protected `develop` would let any actor with `develop` write access push a poisoned image that all CI then builds from. The trigger move is separable from R9 and unnecessary, so it stays on `main`. After dropping the commit step only `packages: write` remains (job-level, unchanged). CI image-cache consumption stays deferred (no `imageName`/`cacheFrom`); record that the eventual wire-in must re-establish digest pinning (`@sha256:`, never `:latest`) before consuming.

**Patterns to follow:** existing `devcontainer-image.yml` security model (SHA-pinned actions, job-level perms, concurrency group, no `pull_request` trigger).

**Test scenarios:**
- Happy path: a `.devcontainer/**` change on `main` publishes the GHCR image; the digest appears in the run summary; no commit is pushed to any branch.
- Edge case: workflow makes no commit/push to any branch and holds no `contents: write` — assert by inspection. Covers R9/R3.
- Edge case: the workflow has no `pull_request` trigger (fork PRs can't obtain `packages: write`) — preserved from the original security model.

**Verification:** `main` receives no bot commits from CI; the image still publishes (from `main`); the job no longer requests `contents: write`.

---

- U7. **Branch + tag rulesets for `develop`, `main`, `release/*`**

**Goal:** Enforce the branch model and required checks.

**Requirements:** R1, R2, R3

**Dependencies:** U0, U2, U3, U4, U5

**Files:**
- Modify/Create: `docs/ci/rulesets.md` (intent + `gh api` commands/JSON)

**Approach:** `develop`: PR + green `format`/`tidy`/`test` (default-branch flip is U0). `main`: PR + green required checks + block direct push/force/delete, admin/hotfix bypass. The `main` PR's required check is the release-head SHA's already-green `format`/`tidy`/`test` reused (see the Key Technical Decision on `main`'s required check) — **not** a re-run of the heavy cross-build; verify GitHub surfaces it, else add the lightweight no-build assertion gate. `release/*`: require the beta checks (same `format`/`tidy`/`test` suite, reported on `release/*` pushes). Keep the existing immutable "Release Tags" ruleset; confirm it permits `-alpha.N`/`-beta.N`/stable names. Wire `required_status_checks` only after U0/U2–U5 run once.

**Execution note:** Capture exact required-check names from a real run before wiring.

**Test scenarios:**
- Test expectation: none (GitHub config) — verification operational below.

**Verification:** Direct push to `main` rejected; a `release/* → main` PR with red checks blocked (AE4); `develop` is the default branch.

---

- U8. **Version reset to the `0.1.0-alpha` line (runbook)**

**Goal:** Delete `v0.1.0` + `v0.1.1` + their releases; seed the clean `0.1.0-alpha` line.

**Requirements:** R10, R11

**Dependencies:** U0, U3

**Files:**
- Create: `docs/ci/version-reset-runbook.md`

**Approach:** One-time maintainer steps (admin only). The "Release Tags" ruleset blocks tag deletion, so the bypass is **mandatory, not "if needed"**: (1) admin temporarily disables the "Release Tags" ruleset (or adds self to its bypass list) via the GitHub UI; (2) delete both bogus Releases + tags — `gh release delete v0.1.0 --yes --cleanup-tag` and `gh release delete v0.1.1 --yes --cleanup-tag`; (3) verify `git tag -l 'v*'` shows neither `v0.1.0` nor `v0.1.1` and no other bogus tags; (4) **re-enable the ruleset immediately**. Then let the first `feat:` develop merge mint `v0.1.0-alpha.1` (or seed via `alpha.yml` dispatch using the `IN_VERSION`/`IN_BUMP` override — see U1). Precondition: confirm no consumer superproject currently pins `v0.1.0`/`v0.1.1`. Document the `0.1.0-beta.1` target (joint firmware+app hardware test) and `1.0.0` as eventual first stable.

**Test scenarios:**
- Test expectation: none (operational runbook) — verification below.

**Verification:** `git tag -l` shows no `v0.1.0`/`v0.1.1`; first develop alpha is `v0.1.0-alpha.1`.

---

- U9. **Docs: rewrite CI/CD sections in `CLAUDE.md`, `README`, `deploy/README.md`**

**Goal:** Docs match the new branch model, ladder, tag scheme, and flow.

**Requirements:** R12

**Dependencies:** U2, U3, U4, U5, U6, U7

**Files:**
- Modify: `CLAUDE.md` ("CI/CD & releasing", "Branch + commit + tag rules", "Releasing")
- Modify: `README.md`
- Modify: `deploy/README.md`

**Approach:** Replace the two-workflow description with the four-workflow + devcontainer-image model; document the ladder (alpha = container cross-build+ctest; beta = real Jetson + app; stable shipped), the `vX.Y.Z[-alpha.N|-beta.N]` scheme, the `feat/* → develop → release/X.Y.Z → main` flow, and the two non-negotiables. Note that `install.sh` installs a released (stable or beta) binary.

**Test scenarios:**
- Test expectation: none (documentation) — verification below.

**Verification:** Docs describe four workflows + the ladder + tag scheme with no remaining "push to main auto-cuts a release" or "commits the digest to main" claims.

---

## System-Wide Impact

- **Interaction graph:** `ci.yml` retarget changes which PRs gate; `release.yml` deletion + `devcontainer-image.yml` fix remove both main-side failable jobs. New workflows key off `develop`/`release/**`/`main`. The app (A4) is the beta hardware counterpart; proto is the wire contract (a proto major forces a major here).
- **Error propagation:** Build/tidy/test failures land before merge (PRs) or on `develop`/`release/*`; promotion fails loudly if the beta binary is missing.
- **State lifecycle risks:** Asset hand-off must promote the exact beta binary; tag immutability prevents re-tagging. The dropped digest-commit removes the only bot-write to `main`.
- **API surface parity:** Same branch/ladder/tag model as app, proto, emulator (artifact + alpha/beta definitions differ).
- **Unchanged invariants:** Devcontainer cross-build correctness, `tidy` hard gate + floor-NOLINT guard, proto submodule consumption, `deploy/install.sh` install path, and all firmware C++ are unchanged.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Retarget breaks the wired required-check names | Job names `format`/`tidy`/`test` kept byte-identical; re-confirm ruleset after first develop run. |
| `develop` not created → `develop`/`release/*` triggers reference a missing branch | U0 bootstraps `develop` + default flip before any retarget. |
| `main`'s required check unrealizable — `ci.yml` off `main` PRs so a `release/*→main` PR has no checks (AE4/AE5) | Require the release-head SHA's already-green check on the PR (Key Decision); verify GitHub surfaces it, else add a no-build assertion gate. |
| Publishing GHCR image from less-protected `develop` → poisoned `:latest` to all CI | U6 keeps `devcontainer-image.yml` on `push:[main]`; drops only the digest-commit step. |
| JetPack build flake on alpha/beta (hosted runner ENOSPC) | `jlumbroso/free-disk-space` retained; `gh run rerun --failed`; self-hosted runner is the durable fix (deferred). |
| `promote.yml` accidentally cross-builds on main | Structural: no `devcontainers/ci` step; asserted by inspection (U5). |
| `devcontainer-image.yml` still writes to main | U6 removes the commit step + drops `contents: write`; asserted by inspection. |
| Prerelease counter math wrong | U1 numeric-precedence tests. |
| Version reset misses a bogus tag | U8 deletes both `v0.1.0` and `v0.1.1`; verify `git tag -l` after. |
| Cross-repo drift | Plans authored together; same version contract documented in each. |

---

## Documentation / Operational Notes

- Two one-time operational runbooks: version reset (U8) and ruleset application (U7) via `gh`.
- `0.1.0-beta.1` is a joint firmware+app hardware test; coordinate cut timing with the app plan.
- JetPack build remains heavy/flaky on hosted runners; re-run guidance stays in docs.

---

## Sources & References

- **Origin document:** [docs/brainstorms/2026-06-17-cicd-workflow-standard-requirements.md](docs/brainstorms/2026-06-17-cicd-workflow-standard-requirements.md)
- Related code: `.github/workflows/ci.yml`, `.github/workflows/release.yml`, `.github/workflows/devcontainer-image.yml`
- Prior CI/CD work: memory `cicd-pipeline-plan`, `clang-tidy-hard-gate-followups`; `docs/plans/2026-06-10-001-feat-ci-cd-release-pipeline-plan.md`
