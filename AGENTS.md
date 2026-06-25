# AGENTS.md

Guidance for coding agents working in **sst-cam-firmware**. For full
architecture, build commands, and the hexagonal module layout, read
[`CLAUDE.md`](CLAUDE.md) — this file mirrors its CI/CD contract and adds the
rules an agent most often trips on.

## Build & test (devcontainer only)

The **only** supported build is cross-compilation from the repo Dev Container
(custom Bootlin toolchain + the order-sensitive L4T sysroot prep under
`.devcontainer/sysroot/`). Native on-device builds are unsupported. Reopen in
Container, then:

```bash
cmake --preset debug   && cmake --build --preset debug     # or release
cmake --preset test    && cmake --build --preset test && ctest --preset test
```

**Compilation is the first test** — never hand work back with the tree broken.
Hardware-bound tests (real IMX477 / NVENC / BlueZ / radio) are written and
committed but are expected to **fail** in the container; they pass on-device.
Do not `#ifdef`-skip them.

## Linting (hard gate)

`tidy` is a **hard CI gate**: `.clang-tidy` promotes every diagnostic to an
error (`WarningsAsErrors: '*'`) and clang-tidy is version-locked to
`clang-tidy-14`. CI is verify-only — fix fixable findings dev-side first:

```bash
scripts/fix.sh          # clang-format + clang-tidy-14 --fix on STAGED C/C++
scripts/fix.sh --all    # whole src/ + tests/ tree
```

`scripts/fix.sh` and CI both source `scripts/tidy-args.sh` (single source of
truth for cross-toolchain flags — they can't drift).

## CI/CD & releasing

PR-gated, Conventional-Commit driven, on the SST branch model
`feat/* → development → release/X.Y.Z → main` with a test-fidelity **maturity
ladder**:

- **alpha** (`vX.Y.Z-alpha.N`) — devcontainer cross-build + container `ctest` in isolation (no hardware); minted on every `development` merge.
- **beta** (`vX.Y.Z-beta.N`) — the aarch64 binary flashed to a **real Jetson** and tested **with the app**; built on `release/*`.
- **stable** (`vX.Y.Z`) — shipped; the verified beta binary promoted unchanged, cut on merge to `main`.

**Two non-negotiables — do not break these:**

1. **Build-in-PR / tag-on-merge** — the aarch64 cross-build (`ci-scripts`/`format`/`tidy`/`test`) is required on `development` / `release/*` PRs.
2. **`main` never builds** — `release.yml` only copies the already-built,
   **SHA-256-verified** beta binary. Never add a `cmake`/devcontainer build step
   to `release.yml`.

Three **branch-scoped** product workflows + the devcontainer-image publisher. Each
product workflow owns one branch tier; the PR gate checks are folded **inside** the
alpha/beta workflows (`pull_request`-gated), so there is **no standalone `ci.yml`**:

| Workflow (name) | Trigger | Does |
| --------------- | ------- | ---- |
| `release-alpha.yml` (`release-alpha`) — owns `development` | PR → `development` | `ci-scripts` (shellcheck + resolve-version tests) + `format` + `tidy` + `test` (the required checks) |
| `release-alpha.yml` (`release-alpha`) | push → `development` (+ dispatch) | `resolve-version.sh alpha` → cross-build → atomic `vX.Y.Z-alpha.N` `--prerelease` |
| `release-beta.yml` (`release-beta`) — owns `release/**` | PR → `release/**` | same `ci-scripts` + `format` + `tidy` + `test` checks |
| `release-beta.yml` (`release-beta`) | push → `release/**` (+ dispatch) | base = branch `X.Y.Z` → cross-build → atomic `-beta.N` `--prerelease`, records binary SHA-256 in notes |
| `release.yml` (`release`) — owns `main` | push → `main` (+ dispatch) | derive `X.Y.Z`, tag `vX.Y.Z`, download + **verify SHA-256** of beta binary, re-upload renamed (no build) |
| `devcontainer-image.yml` | push → `main` touching `.devcontainer/**` | publish GHCR devcontainer image; surfaces digest in Summary, commits nothing |

The check jobs are `if: github.event_name == 'pull_request'`; the release jobs are
`if: github.event_name != 'pull_request'`. Version math is
`scripts/ci/resolve-version.sh` (single source for all release workflows; tested by
`scripts/ci/resolve-version-test.sh` — run it after editing the script, and
`ci-scripts` gates it). Default `GITHUB_TOKEN` only — no PAT/App. The "Release
Tags" ruleset permits creating compliant semver tags.
Prerelease publish is **atomic** (one `softprops/action-gh-release@v2` step
creates the tag and attaches the binary) so a cancel can't leave an asset-less
immutable tag.

### Branch + commit + tag rules

- `development` is the default branch; target `feat/*` / `fix/*` PRs at it. Do not
  target `main`.
- `main` is promote-only: no direct push; PR from `release/*` + 1 approval + green checks.
- Tags `v*` are immutable semver (`-alpha.N` < `-beta.N` < stable; no delete/move/force-push).
- Use Conventional Commits; the squash-merge subjects since the last stable tag
  drive the alpha base bump (`feat:` → minor, `fix:`/`perf:` → patch,
  `BREAKING`/`type!:` → major, docs/chore-only → **skip**).

### Operational runbooks (maintainer/admin, not agent-run)

- `docs/ci/rulesets.md` — apply the branch/tag rulesets.
- `docs/ci/version-reset-runbook.md` — reset to the `0.1.0-alpha` line.

## Scope guardrails

- CI/CD + docs changes here do **not** touch C++ / CMake / build-system code.
- Proto stays a git submodule; CI must keep `submodules: recursive` and the
  `sst_proto` codegen before tidy/build.
- No database, no NVENC, no skeletons/TODOs/stubs — see CLAUDE.md before adding
  any of these.
