# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

SST Cam firmware — C++20 embedded runtime for NVIDIA Jetson Orin Nano (JetPack 7.2 / L4T r39.2 / Ubuntu 24.04). Dual IMX477 cameras, GStreamer pipeline, AI sports tracking. `_old/` contains deprecated Python prototypes; ignore it.

## Build

The **only supported build method is cross-compilation from the Dev Container.** Native on-device builds are not supported.

1. Install the **Dev Containers** extension (`ms-vscode-remote.remote-containers`).
2. Open this repo → **"Reopen in Container"** (or `Dev Containers: Reopen in Container`).
   VSCode builds the image automatically (~3 GB L4T r39.2 BSP fetch + sysroot assembly, ~10-15 min first time).
   The image build assembles the sysroot via `apply_binaries`, which chroots into the
   arm64 rootfs — it needs **qemu-aarch64 binfmt** on the host. Docker Desktop registers
   this automatically; on a plain Linux Docker host, register it once with
   `docker run --privileged --rm tonistiigi/binfmt --install arm64` (CI does this via
   `docker/setup-qemu-action`). Without it the build fails with `Exec format error`.
3. VSCode opens inside the container — cmake, clangd, and CMake Tools all work.
4. Configure + build:

```bash
cmake --preset debug
cmake --build --preset debug

# Release
cmake --preset release
cmake --build --preset release
```

Binary lands in `build/<preset>/bin/sst_cam_firmware`.

## Tests

Tests are off by default. Use the `test` preset (sets `SST_ENABLE_TESTS=ON` and pulls GTest via Conan):

```bash
cmake --preset test
cmake --build --preset test
ctest --preset test

# Run single test binary directly for faster iteration
./build/test/bin/sst_cam_firmware_tests --gtest_filter="ConfigLoaderTest.*"
```

**Compilation is the first test.** Before declaring any change done, run `cmake --build --preset debug` (or `--preset test`) and ensure it builds clean. If you cannot make the binary build, you have not finished. Do not hand work back as done while the tree is broken.

**Tests are isolated.** A test for module X must spawn its own dependencies (config, GStreamer pipeline, temp storage dir, …), not reach into the app's wiring or borrow state from another test. A recording-service test writes to its own per-test temp directory and enumerates that, never a shared one. No fixtures bleeding through, no "the previous test left this file in place." Each test must be runnable in any order, alone, repeatedly.

Tests must verify behaviour end-to-end at the **module** boundary: feed real inputs through the public port and assert the real outputs (a file on disk, a row in DB, a frame on a loopback socket). Mocks are for things outside the module (network endpoints, hardware that isn't on the test machine), not for the module's own collaborators.

**Hardware-bound tests are still written and committed**, even though they can't pass in the dev container. Tests that need the IMX477 sensors, the Jetson NVENC, BlueZ on a real D-Bus, wpa_supplicant against real radio, etc., go in the same `tests/` tree alongside everything else and are expected to fail when run in the cross-compile container. They pass on the device. Don't gate them with `#ifdef` or skip them — write them for real, run them on-device when you have hardware. The container suite shows them as "Failed" and that's expected.

## Linting & formatting

```bash
# Format (Google style, defined in .clang-format)
clang-format -i src/**/*.cpp src/**/*.hpp

# Tidy (checks defined in .clang-tidy — bugprone, performance, modernize, readability, google-*)
# clang-tidy is noble's v18 (JetPack 7.2 / Ubuntu 24.04). The enforced check set
# + the warning baseline are version-locked; treat a version bump as a deliberate,
# re-triaged change. Cross-toolchain flags live in scripts/tidy-args.sh.
clang-tidy -p build/test src/path/to/file.cpp
```

`compile_commands.json` is exported automatically to `build/<preset>/` and symlinked by clangd.

### Auto-fix before you push

`tidy` is a **hard CI gate** (clang-tidy with `WarningsAsErrors`), and CI is
verify-only — it never writes fixes back to your branch. Correct fixable
findings **dev-side**, inside the devcontainer, before pushing:

```bash
scripts/fix.sh          # clang-format + clang-tidy --fix on STAGED C/C++ only
scripts/fix.sh --all    # the whole src/ + tests/ tree (bulk cleanup)
```

`scripts/fix.sh` and the CI tidy runner (`scripts/ci/tidy-run.sh`, shared
byte-for-byte by both workflows) both source `scripts/tidy-args.sh`, so their
cross flags can never drift. `--fix` corrects only checks that ship
fixits; the noisy blockers (`magic-numbers`, `easily-swappable-parameters`,
`cognitive-complexity`, `branch-clone`) have no fixit and stay reported for you
to resolve by hand.

Enable the pre-commit hook once per clone (run inside the devcontainer):

```bash
git config core.hooksPath .githooks
```

It runs `scripts/fix.sh` on staged C/C++ files and re-stages what it rewrote.

## CI/CD & releasing

PR-gated, Conventional-Commit driven. The cross-build runs **inside the repo's devcontainer image**, but that image is built **once per `.devcontainer/**` content and reused** — see "Image reuse" below — not rebuilt per job. The pipeline follows the SST branch model `feat/* → development → release/X.Y.Z → main` with a three-rung maturity ladder:

- **alpha** — the devcontainer cross-build + container `ctest` in isolation (no hardware).
- **beta** — the aarch64 binary flashed to a **real Jetson** and tested **with the app**.
- **stable** — shipped: the verified beta binary promoted unchanged.

Tag scheme: `vX.Y.Z[-alpha.N|-beta.N]`. Three **branch-scoped** product workflows. Each owns one branch tier and folds the PR gate checks inside itself (`pull_request`-gated) — there is **no standalone `ci.yml`**:

- `.github/workflows/release-alpha.yml` (name `release-alpha`) — **owns `development`**. On **`pull_request` into `development`** it runs the gate checks `ci-scripts` (shellcheck + resolve-version tests), `format` (clang-format `--dry-run --Werror`), `tidy` (clang-tidy), `test` (cross-build the `test` preset + `ctest` under qemu; hardware-bound tests excluded by name — they only pass on-device). On **push to `development`** (a merge) + dispatch it runs the alpha **release** job: `scripts/ci/resolve-version.sh alpha` picks the next `vX.Y.Z-alpha.N` from a Conventional-Commit base bump (`feat:` → minor, `fix:`/`perf:` → patch, `BREAKING`/`type!:` → major, docs/chore-only → **skip**), cross-builds the binary, and publishes a **prerelease** with `sst_cam_firmware-<tag>-aarch64`. The check jobs are `if: github.event_name == 'pull_request'`; the release jobs are `if: github.event_name != 'pull_request'`.
- `.github/workflows/release-beta.yml` (name `release-beta`) — **owns `release/**`**. Same gate checks (`ci-scripts`/`format`/`tidy`/`test`) on **`pull_request` into `release/**`**. On **push to `release/**`** + dispatch it runs the beta **release** job: base `X.Y.Z` comes from the branch name; `resolve-version.sh beta X.Y.Z` → `vX.Y.Z-beta.N`. Cross-builds, publishes a prerelease binary, and **records the binary's SHA-256 in the Release notes** (the promote step verifies against it).
- `.github/workflows/release.yml` (name `release`) — **owns `main`**. On **push to `main`** (a `release/X.Y.Z → main` merge) + dispatch it **promotes**: derives `X.Y.Z` from the merged branch, selects the highest `vX.Y.Z-beta.N` tag, tags the stable `vX.Y.Z`, **downloads the beta binary, verifies its SHA-256** against the recorded digest, renames it to `sst_cam_firmware-vX.Y.Z-aarch64` (bytes preserved), and uploads it to the stable Release. **No cross-build runs on `main`** — `main` only promotes.
**Image reuse (content-hash build-or-pull).** Both `release-alpha.yml` and `release-beta.yml` start with an `image` job that tags the devcontainer by `hashFiles('.devcontainer/**')` (the full image recipe) and pushes it to `ghcr.io/scoutsporttechnology/sst-cam-firmware-devcontainer:<hash>`. If that tag already exists in GHCR it is a **pull** (~8 s manifest-inspect, no build); otherwise it builds + pushes that hash **once**. The sysroot-dependent jobs (`tidy-shard` — the sharded clang-tidy matrix — `test`, and the release cross-build) `needs: image` and `docker pull` + `docker run` it (~2-3 min pull, no per-run rebuild). `format` + `ci-scripts` are **host-only** (apt `clang-format-18` / shellcheck — no image); the `tidy` aggregator is host-only too (it just gates on the shard results + runs the floor guard). A `.devcontainer/**` change is the only thing that triggers a rebuild; every other PR reuses the pinned image. A `changes` job (dorny/paths-filter) gates `tidy-shard`/`test` so a **docs/workflow-only PR skips them** — a job skipped by `if:` counts as success for the required checks, so the wired names stay green without running. SECURITY: consumers pull by **content-hash tag, never `:latest`**; the push needs `packages: write`, so fork PRs (read-only token) skip it and build-only — they can neither poison nor benefit (no `pull_request` publish to a privileged context).

The `tidy` hard gate is intact inside `release-alpha.yml` / `release-beta.yml`: cross-toolchain header flags live in `scripts/tidy-args.sh`, the tree is clean under the full check set, and `.clang-tidy` promotes every diagnostic to an error (`WarningsAsErrors: '*'`); clang-tidy is noble's v18 (treat a version bump as a deliberate, re-triaged change). The floor-NOLINT guard ships alongside it. Auto-fix fixable findings dev-side with `scripts/fix.sh` before pushing (CI is verify-only). **tidy is SHARDED for speed:** clang-tidy cost scales with TU count (~95 TUs, each re-parsing heavy GStreamer/OpenCV headers ≈ ~21 min single-job), so `scripts/ci/tidy-run.sh shard <i> <n>` round-robin-splits the TU list across a `tidy-shard` runner matrix (4 legs) — **full-tree coverage is preserved every PR** (no diff-scoping, so "green = clean tree" still holds), only the wall-time shrinks ~Nx. A thin aggregator job named `tidy` (the byte-identical wired required check) `needs:` the shards and fails iff any shard failed (`if: always()` so a failed shard can't skip-propagate to a green "skipped"); it also hosts the floor-NOLINT guard. The selection/sharding logic has its own host-side self-test (`scripts/ci/test-tidy-run.sh`) run in `ci-scripts`. Bump the shard count by editing both the matrix `shard:` list and the `n` passed to `tidy-run.sh` in lockstep.

Default `GITHUB_TOKEN` only — no PAT/App. The "Release Tags" ruleset permits creating compliant semver tags. Ruleset + version-reset runbooks live in [docs/ci/](docs/ci/).

**Two non-negotiables:** (1) `main` runs **no failable build/publish job** — it only promotes the already-built, digest-verified beta binary. (2) No CI job pushes a commit back to `main`.

### Branch + commit + tag rules
- Flow: `feat/* → development → release/X.Y.Z → main`. `development` is the default integration branch; `main` holds only released, promoted code.
- `development` / `release/**`: PR + green `ci-scripts`/`format`/`tidy`/`test` to merge. `main`: PR + 1 approval + green checks + **no direct push** (admin/hotfix bypass).
- Tags `v*` are immutable semver (no delete/move/force-push), including `-alpha.N` / `-beta.N` prereleases.
- Use Conventional Commits. The **squash-merge subject** is what `resolve-version.sh` reads on `development` to choose the alpha base bump — a docs/chore-only subject mints no alpha.
- **Never push releasable commits directly to `development` or `release/**` — always land them via a PR merged on github.com.** A release tag is a lightweight ref, so its **"Verified" badge mirrors the target commit's signature**: github.com web-flow-signs PR squash/merge commits automatically, but a locally-authored direct push is `unsigned` → the `-alpha.N`/`-beta.N` cut from it has **no Verified badge** (this is exactly how `beta.8` lost it). If a local hotfix is genuinely unavoidable, sign it (`git commit -S`, SSH/GPG key registered on GitHub) so it still verifies. The badge is commit provenance, not binary integrity (that's the `sha256:` hand-off) — but a missing badge means a release commit bypassed PR review. See [docs/ci/release-commit-signing.md](docs/ci/release-commit-signing.md).

### Releasing
- Alpha: merge a `feat:`/`fix:`/… PR into `development` → `release-alpha.yml`'s push run cuts `vX.Y.Z-alpha.N` with the aarch64 binary. Seed/override: `gh workflow run release-alpha.yml -f version=v0.1.0` (or `-f bump=minor`).
- Beta: cut `release/X.Y.Z` from `development` → `release-beta.yml`'s push run cuts `vX.Y.Z-beta.N`; iterate fixes on the branch for `-beta.2`, `-beta.3`. Flash a real Jetson + test with the app to sign off.
- Stable: open the `release/X.Y.Z → main` PR; on merge, `release.yml` tags `vX.Y.Z` and copies the verified beta binary — no rebuild. Afterward delete the release branch and merge `main` back into `development`.
- The devcontainer build is heavy and can flake on hosted runners — re-run with `gh run rerun <id> --failed` (a self-hosted runner is the durable fix).
- Install/update on a Jetson: `deploy/install.sh` installs a **released** (stable or beta) binary (see `deploy/README.md`).

## Release lifecycle

The version ladder is driven by **which branch you push to**, not by counters. Tags climb `vX.Y.Z-alpha.N` (development) → `vX.Y.Z-beta.N` (release/*) → `vX.Y.Z` (main); the math lives in `scripts/ci/resolve-version.sh`.

**Alpha — automatic, every `development` merge.** `release-alpha.yml` runs `resolve-version.sh alpha`: the base is a Conventional-Commit bump from the latest *stable* tag (or from `v0.0.0` when none exists), and `-alpha.N` increments per merge. The aarch64 binary is cross-built and attached to the prerelease.

```
feat A → development   →  v0.1.0-alpha.1
feat B → development   →  v0.1.0-alpha.2
feat C → development   →  v0.1.0-alpha.3
```

With no stable tag yet, a `feat:` yields base `0.1.0` (a `feat!:`/`BREAKING CHANGE` → `1.0.0`; a `fix:`-only → `0.0.1`); docs/chore-only mints nothing.

**Beta — when you cut the release branch.** Manually branch `release/X.Y.Z` off `development` and push it; `release-beta.yml` runs `resolve-version.sh beta X.Y.Z` (base = the branch name), cross-builds the binary, and records its SHA-256 in the Release notes:

```
git switch -c release/0.1.0 development && git push   →  v0.1.0-beta.1
```

Each subsequent push to that branch bumps the beta counter — `-beta.2`, `-beta.3`, … This is the rung you **validate by flashing the binary to a real Jetson and testing with the app**. Alpha and beta are independent counters.

**Stable — when you merge `release/X.Y.Z → main`.** Pushing the branch *creates* the betas; **merging it to `main` promotes the latest beta to stable.** `release.yml` auto-selects the highest `vX.Y.Z-beta.N`, tags `vX.Y.Z`, then **downloads the beta binary, verifies its SHA-256, and re-uploads the same bytes** to the stable Release — no cross-build on `main`.

```
development:        alpha.1   alpha.2   alpha.3
                                       │ cut release/0.1.0
release/0.1.0:                         └─► beta.1 → beta.2 → beta.3
                                                              │ merge → main
main:                                                         └─► v0.1.0  (stable)
```

After `v0.1.0` stable exists, the next `feat:` on `development` bumps from the latest stable → `v0.2.0-alpha.1` (a `fix:` → `v0.1.1-alpha.1`). The alpha base climbs only once a stable is cut.

## Dependencies

**Conan** (portable, via `conanfile.py`): `nlohmann_json`, `spdlog`, `fmt`, `gtest` (test-only).

**System** (JetPack apt / pkg-config): GStreamer 1.0 (`gstreamer-1.0`, `-sdp`, `-app`, `-video`), OpenCV 4.

Conan packages are resolved automatically on `cmake --preset ...` via `cmake/conan_provider.cmake`. Never manually install Conan packages.

## Architecture

Hexagonal (ports & adapters), per-module. Each module lives under both `src/` subtrees:

```
src/
├── domain/<module>/     # Pure C++ — entities, value objects, invariants. No I/O, no heavy libs.
├── app/<module>/        # Use-case services, orchestration. Depends on domain + ports only.
├── adapters/<module>/   # GStreamer, filesystem, OpenCV. Implements ports.
└── <module>/ports/      # Abstract interfaces (pure virtual). Stable contracts between layers.
```

Dependency direction: `adapters → ports/app/domain`, `app → ports/domain`, `domain → nothing external`.

**Forbidden**: `domain/` importing from `adapters/`; ports exposing GStreamer/OpenCV types; utils that import heavy libs and get used by domain.

## Pipeline

The end-to-end intended flow:

```
       per-camera (×2)                                       shared            chosen frame only        parallel        outputs
─────────────────────────────────────────────────────  ┌───────────────┐   ┌──────────────────┐   ┌─────────────┐   ┌──────────┐
                                                       │               │   │                  │   │             │   │ storage  │
cam 0 ─► capture ─► preprocess ─► materialize ─► buf ─►│  AI/tracking  │──►│                  │   │             │ ┌►│          │
                                                       │   (per cam)   │   │                  │   │             │ │ └──────────┘
                                                       │       │       │   │  postprocess     │   │             │ │
                                                       │       ▼       │   │  (crop + zoom    │   │  overlay    │ │
                                                       │   physics ────┼──►│   + resize)      │──►│  (banner /  │─┤
                                                       │   (shared)    │   │                  │   │  scoreboard)│ │
                                                       │       │       │   │                  │   │             │ │ ┌──────────┐
                                                       │       ▼       │   │                  │   │             │ └►│          │
cam 1 ─► capture ─► preprocess ─► materialize ─► buf ─►│   decision    │──►│  ─► final buffer │   │             │   │ streaming│
                                                       │ (which cam +  │   │                  │   │             │   │          │
                                                       │  crop rect)   │   │                  │   │             │   └──────────┘
                                                       └───────────────┘   └──────────────────┘   └─────────────┘
```

Per-stage notes:

- **Capture (per camera)**: one GStreamer pipeline per IMX477. Pull-based `ICaptureFrame::Capture() → std::optional<Frame>`. The appsink is capped at 5 in-flight buffers — anything that pins a `GstBuffer` downstream eats into that cap.
- **Preprocess (per camera)**: raw NV12 `Frame` → `FrameBundle{ source_frame, ai_frame }`. Grayscale/Binary AI paths touch only the Y plane (no color conversion); RGB does NV12→RGB. `source_frame` is a value-copy of the raw — same `owner` shared_ptr, no pixel copy.
- **Materialize**: `sst::buffer::MaterializeFrame(bundle.source_frame)` deep-copies the source planes into an owned `vector<uint8_t>`. After this the GstBuffer is released. Called by the producer right before `buffer.Push`, so nothing past this point pins the GStreamer appsink.
- **Per-camera buffer**: `LatestOnlySlot<FrameBundle>` (drop older bundles on overwrite). Decouples capture cadence from AI cadence.
- **AI / tracking (per camera)**: consumes `ai_frame`; produces field / ball / (eventually) player detections. Unchosen bundles age out via buffer eviction — both frames and detections drop together.
- **Physics (shared)**: consumes detections from both cameras; estimates ball position / trajectory in world coordinates.
- **Decision (shared)**: takes physics output and picks **which camera** and **which `CropRect`** (zoom level + region in source-frame pixels) best frames the action. Hands the chosen `FrameBundle` + crop rect to postprocess.
- **Postprocess** (chosen frame only): NV12→BGR full-res → crop → resize to output resolution → format-convert. Expensive color conversion is paid here, not in preprocess. Pushes into the final buffer.
- **Final buffer**: small bounded queue between postprocess and the output stage.
- **Overlay (parallel)**: runs alongside the final buffer. Produces banner / scoreboard / event-info overlays from user/app state. Storage and streaming composite this on top of the final frame **if the user enabled it**.
- **Storage**: writes the (optionally overlaid) final frames to disk — local recording / archive.
- **Streaming**: sends the (optionally overlaid) final frames over the network — RTSP / HLS / etc.

Storage and streaming consume the same final buffer in parallel; either can be enabled or disabled independently. Capture threads do minimal work and never block on downstream. Buffers are bounded and drop old data. `PipelineOrchestrator` already runs the producer/consumer worker threads (single-camera, inline full-frame crop); the AI → physics → decision shared stages are not built yet — the hardware demo adds the `IDecision` seam and the second camera.

## Module status

Built and working:

- [x] **Config** — JSON-loaded device/calibration/storage configs (`src/{domain,app,adapters}/config/`).
- [x] **Capture** — GStreamer adapter for dual IMX477 (`src/adapters/capture/frame/gstreamer/`).
- [x] **Buffer** — `LatestOnlySlot<T>`, `DropOldestRing<T>`, plus `MaterializeFrame` for releasing GstBuffer-backed frames at the buffer boundary (`src/domain/buffer/`).
- [x] **Network / control** — WiFi and Bluetooth BLE control modules.
- [x] **Processing** — `IPreprocessor` / `IPostprocessor` ports + OpenCV adapter (Grayscale / Binary / RGB AI modes; crop + resize + format-convert post). `FrameBundle`, `CropRect`, `ColorMode` (`src/{domain,app,adapters}/processing/`).
- [x] **Pipeline orchestration** — `PipelineOrchestrator` (`src/app/pipeline/services/orchestrator/`) wires capture → preprocess → materialize → buffer → postprocess → fan-out via two worker threads (`ProducerLoop`/`ConsumerLoop`). Single-camera with an inline full-frame crop today; the `IDecision` seam and second camera are the hardware-demo work.
- [x] **Storage** — `RecordingService` (`src/app/storage/services/recording_service/`) + GStreamer continuous recorder write MP4s to disk; `DownloadServer` enumerates them and mints TTL download tokens.
- [x] **Streaming** — `StreamingService` (`src/app/streaming/services/streaming_service/`) fans out to an RTSP app-stream server + RTMP streamer (`src/adapters/streaming/`). RTSP `StartAppStream` has no production caller yet (hardware-demo work).
- [x] **Overlay** — `OverlayHandler`, cairo renderer, GStreamer compositor, proto→domain mapper (`src/{domain,app,adapters}/overlay/`). Built but **not yet wired into the final-frame production path**.

Not started:

- [ ] **Decision** — picks which camera's frame + crop / zoom rect; hands off to postprocess. (The hardware demo adds a static cam-0 `StaticDecision` behind the `IDecision` port — the intelligence seam.)
- [ ] **AI / tracking** — TensorRT model + adapter; field and ball first, players + jersey numbers later. One inference per camera.
- [ ] **Physics** — ball trajectory / world-coordinate projection from both cameras' detections.
- [ ] **Microphone** — MAX4466 dual-mic integration.

> **No database module.** There is no SQLite/DB module in `src/` and no `db/` directory. Recording metadata is enumerated directly from the filesystem (`DownloadServer` over the MP4 files in `cfg.storage.video`). Earlier docs claimed a SQLite + `DbSeeder` module — that was never built.

> **No hardware H.264 encoder on the Orin Nano.** The Jetson Orin Nano has no NVENC; `nvv4l2h264enc` does not resolve on this silicon. Recording, RTSP preview, and RTMP must use software encode (`x264enc speed-preset=ultrafast tune=zerolatency`, reading system memory — no `nvvidconv`/NVMM hop). Element resolution happens at `gst_parse_launch` runtime, so a missing `x264enc` keeps the container build green and fails only on-device — `x264enc` lives in `gstreamer1.0-plugins-ugly` (add to the sysroot, not the already-installed *bad*).

## Persistence

No database. Config is JSON-loaded (`src/{domain,app,adapters}/config/`); recordings are MP4 files on disk under `cfg.storage.video`, enumerated directly by `DownloadServer`. There is no SQLite, no `db/` directory, and no `DbSeeder` — do not add one for the hardware demo (filesystem enumeration is the contract).

## Models

Every domain model (struct/class in `src/domain/<module>/models/`) must ship a `fmt::formatter` specialization so it can be logged via `spdlog`/`fmt`. Place formatters in `src/domain/<module>/models/formatter/<model>-fmt.hpp` and re-export them from a sibling `_fmt.hpp` aggregator (see [src/domain/config/models/formatter/](src/domain/config/models/formatter/) and [src/domain/processing/models/formatter/](src/domain/processing/models/formatter/) for the pattern). When you add or rename a model, add/update its formatter in the same change.

## Documented solutions

`docs/solutions/` — documented solutions to past problems (bugs, best practices, workflow/tooling decisions), organized by category with YAML frontmatter (`module`, `tags`, `problem_type`). Relevant when implementing or debugging in documented areas (e.g. proto-contract logic alignment, the cpp→main consolidation).

## Style

Google C++ Style Guide. C++20, `CMAKE_CXX_EXTENSIONS ON`. Headers and sources co-located (no separate `include/`). `SST_REPO_ROOT_DIR` macro available at compile time for resolving config paths in tests.

## No skeletons, no TODOs, no stubs

Ship final, working code. **No skeletons.** A class whose method body is `log("not implemented"); return kUnimplemented;` is a skeleton — do not write it, and if you find one, finish it or delete it. A controller whose route handler is "provisional, until the .proto arrives" is a skeleton — wire the real verb today.

The same rule applies to: `// TODO`, `// FIXME`, commented-out CMake blocks, "uncomment when ready" markers, adapter classes whose only contents are `// TODO: bluez wiring`, and ports with no concrete implementation behind them. If a piece of work has to be deferred, do not leave a placeholder in the tree — track it outside the codebase.

If a new feature needs a new system dep, install it in the sysroot via `.devcontainer/sysroot/003_install_extra_pkgs.sh`, link it in `CMakeLists.txt` as `REQUIRED`, and use it. Do not add a header that includes the not-yet-installed library "to be filled in later."

## Adding system dependencies to the JetPack sysroot

The L4T r39.2 BSP sample rootfs does not include every `-dev` package — when a new system dep is needed (e.g. `sdbus-c++`, `gst-rtsp-server`), add the matching Ubuntu **noble** (24.04) `arm64` `.deb` URL to [.devcontainer/sysroot/noble-deb-urls.txt](.devcontainer/sysroot/noble-deb-urls.txt) (resolve the exact version + dep closure with `apt-get install --print-uris` against the assembled rootfs). `003_install_extra_pkgs.sh` extracts that list, and the Dockerfile runs it *before* `002_fix_sysroot.sh`, so newly extracted libraries are picked up by the symlink-fix and `.so` linker-stub passes automatically.
