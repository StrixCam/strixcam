# clang-tidy hard gate — requirements

**Date:** 2026-06-15
**Repo:** sst-cam-firmware
**Status:** ready for planning
**Related:** `docs/brainstorms/ci-cd-release-pipeline-requirements.md`, `.github/workflows/ci.yml`, `.clang-tidy`

## Problem

The `tidy` job in `.github/workflows/ci.yml` is **advisory** (`continue-on-error: true`) and currently fails. It is the one remaining gap in an otherwise-green CI/CD pipeline. We want clang-tidy to be a real required status check on `main`.

### Root cause (verified from CI run 27565494770)

The job fails with **exit code 123** (xargs propagating a non-zero clang-tidy invocation). The trigger is **45 hard errors**, all of the form:

```text
src/.../gstreamer.hpp:3:10: error: 'cstdint' file not found [clang-diagnostic-error]
src/.../proc-system-stats.hpp:3:10: error: 'filesystem' file not found
... also: 'string' 'mutex' 'optional' 'vector' 'memory' 'atomic' 'functional'
```

clang-tidy (host x86 clang, targeting aarch64) **cannot find the C++ standard library headers**. The `--target` / `--sysroot` / `--gcc-toolchain` flags added in commit `f99f94a` did **not** actually fix libstdc++ discovery — the in-code CI comment claiming "header resolution fixed" is wrong.

**Likely concrete cause:** the CI step's fallback toolchain path is `/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin`, but the real container env is `BOOTLIN_TOOLCHAIN_BIN=/l4t/aarch64--glibc--stable-2022.08-1/bin` — **no `/toolchain/` segment**. A wrong `--gcc-toolchain` base means clang never locates the Bootlin libstdc++ include tree. To confirm, introspect the running container for the actual `include/c++/<ver>` and `<triple>/include/c++/<ver>` paths.

### Two distinct problems — do not conflate

1. **Hard fail (the 45 errors):** toolchain header-discovery bug. This is what turns the gate red.
2. **644 warnings** (190 in `src/`, 454 in `tests/`): readability/bugprone nits. `.clang-tidy` sets `WarningsAsErrors: ''`, so **warnings alone never fail the build.** Fixing problem 1 makes the job green even with all 644 warnings still present.

Top check counts: `readability-identifier-length` (290), `readability-magic-numbers` (189), `bugprone-easily-swappable-parameters` (58), `modernize-use-nodiscard` (23), `readability-convert-member-functions-to-static` (19), `readability-named-parameter` (16), `readability-function-cognitive-complexity` (13).

## Goal

Make `tidy` a **hard, meaningful** required status check on `main`: real lint errors and lint debt both fail the build, with zero existing debt at the moment it goes hard.

## Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | **Fix via the cross-toolchain path**, not a native host preset. Pass explicit `-isystem` args for the Bootlin aarch64 libstdc++ include dirs inside the existing devcontainer tidy job. | One environment shared with the cross test build; parses the true target ABI; ready in-place for CUDA/TensorRT when the tracking module lands (headers present in the sysroot, no separate host install). |
| 2 | **Strict everywhere.** Clean all 644 warnings across `src/` and `tests/`, promote the full check set to `WarningsAsErrors`, and drop `continue-on-error`. | Zero lint debt; the gate has teeth from day one. |
| 3 | **All clang-tidy verification runs inside the devcontainer, never host clang.** | Matches `CLAUDE.md` ("only supported build method is cross-compilation from the Dev Container"). The firmware devcontainer (`sst-cam-firmware_devcontainer-app-1`) is already running — use `docker exec`, no rebuild. |
| 4 | Scope = **sst-cam-firmware only.** | `tidy` = clang-tidy = firmware. The other three repos' CI is already green; this is the last advisory-red gate. |

## Success criteria

- [ ] `tidy` job exits 0 on a clean tree, with **0** `clang-diagnostic-error` (no `file not found`).
- [ ] `tidy` runs **without** `continue-on-error` and is a required status check on `main`.
- [ ] All 644 current warnings resolved (or explicitly, deliberately suppressed — see open items).
- [ ] A PR that introduces a new lint violation **fails** `tidy`.
- [ ] Verified by running clang-tidy **inside the devcontainer**, reproducing the green result locally before relying on CI.
- [ ] `format` and `test` gates unaffected.

## Out of scope

- Native host tidy preset (rejected in favour of cross-toolchain).
- Self-hosted runner (public repos — arbitrary fork code on home hardware; security no-go).
- GHCR devcontainer image caching (separate perf optimization; revisit only if 13-min tidy runs become painful).
- CI/CD changes in sst-cam-app, sst-cam-proto, sst-cam-emulator.

## Open items / refinements (resolve in planning)

1. **`WarningsAsErrors` curation vs blanket.** "Full check set as errors" bakes in high-friction *stylistic* checks — `modernize-use-trailing-return-type` (mandates trailing-return syntax everywhere) and `bugprone-easily-swappable-parameters` (58 hits, often only resolvable by wrapping params in structs). Cleaning them once is cheap; enforcing them as **permanent hard errors** means every future PR fights them. Decide: blanket `WarningsAsErrors: '*'`, or clean-all-644-now + a curated error subset (e.g. exclude trailing-return-type and easily-swappable-parameters, keep them as visible warnings).
2. **Test-code checks.** ~454 of the 644 warnings are in `tests/`, dominated by magic-numbers (1920/1080/30fps/ports) and short identifiers — idiomatic in tests. Under "strict everywhere" these get cleaned too. If that churn proves low-value, a `tests/.clang-tidy` override disabling those specific checks for test code is the fallback (would move some items from "fixed" to "deliberately suppressed").
3. **Exact `-isystem` paths.** Derive from the running container's actual Bootlin layout (`include/c++/<ver>`, `<triple>/include/c++/<ver>`, plus the C runtime include dir). Fix the wrong `/l4t/toolchain/...` fallback path in the CI step at the same time.
4. **Branch protection.** After the job is green and hard, add `tidy` to the required status checks on `main` (currently only `format` + `test`).

## Verification approach

Reproduce locally **in the devcontainer** before trusting CI:

```bash
docker exec sst-cam-firmware_devcontainer-app-1 bash -lc '
  cmake --preset test &&
  clang-tidy -p build/test <one failing TU> \
    --extra-arg=--target=aarch64-linux-gnu \
    --extra-arg=--sysroot=$CROSS_SYSROOT \
    --extra-arg=-isystem<derived bootlin c++ include path> ...'
```

Iterate the `-isystem` set on a single previously-erroring TU (e.g. `src/adapters/capture/frame/gstreamer/gstreamer.cpp`) until `'cstdint' file not found` disappears, then run the full file list.
