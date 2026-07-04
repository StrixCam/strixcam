# SST Cam firmware — task runner.
#
# Every recipe runs natively when you're INSIDE the dev container, and
# transparently delegates to the container via the `devcontainer` CLI when you
# run it from the HOST. You never have to think about which — `just build` and
# `just test` work the same in both places.
#
# The firmware cross-compiles to aarch64 (Jetson). Pure-logic unit tests run on
# the x86 host under qemu (QEMU_LD_PREFIX points at the sysroot loader);
# hardware-bound tests (real IMX477 / BlueZ / radio / live GStreamer) only pass
# on-device and are expected to fail in the container.
#
# Requirements:
#   - Host:         `just` + the `devcontainer` CLI on PATH (or at ~/.devcontainers/bin).
#   - In-container: `just` (installed via the devcontainer `just` feature).
# Start the container once from the host with `just up` before delegating.

set shell := ["bash", "-uc"]

# aarch64 sysroot loader prefix — lets qemu run the cross-compiled test binary.
qemu_prefix := "/l4t/targetfs"
test_bin := "./build/test/bin/sst_cam_firmware_tests"

# List recipes.
default:
    @just --list

# --- container lifecycle (host-side) --------------------------------------

# Start the dev container (no-op if already running).
up:
    @_DC="$(command -v devcontainer || echo "$HOME/.devcontainers/bin/devcontainer")"; "$_DC" up --workspace-folder "{{justfile_directory()}}"

# Open an interactive shell in the dev container.
shell:
    @_DC="$(command -v devcontainer || echo "$HOME/.devcontainers/bin/devcontainer")"; "$_DC" exec --workspace-folder "{{justfile_directory()}}" bash

# --- build (auto native-or-delegated) -------------------------------------

# Configure + build the debug binary (default dev-loop build).
build-debug-firmware:
    @just _run "cmake --preset debug && cmake --build --preset debug"

# Configure + build the release binary (the deployable artifact).
build-release-firmware:
    @just _run "cmake --preset release && cmake --build --preset release"

# Configure + build the test binary (GTest via Conan).
build-test:
    @just _run "cmake --preset test && cmake --build --preset test"

# --- test -----------------------------------------------------------------

# Build the test preset and run the whole suite under qemu. Extra gtest args
# pass through, e.g. `just test --gtest_filter=DeviceHandlerTest.*`.
test *args: build-test
    @just _run "QEMU_LD_PREFIX={{qemu_prefix}} {{test_bin}} {{args}}"

# Run the already-built suite without rebuilding (faster iteration).
test-only *args:
    @just _run "QEMU_LD_PREFIX={{qemu_prefix}} {{test_bin}} {{args}}"

# Run via ctest (shows the CTest test list / timings).
ctest:
    @just _run "QEMU_LD_PREFIX={{qemu_prefix}} ctest --preset test --output-on-failure"

# --- CI parity ------------------------------------------------------------

# The exact gate CI runs: format-check + tidy + build-test + ctest. Same
# entrypoint locally and in .github/workflows/ci.yml — keep the two in sync.
ci-check:
    @just _run "set -euo pipefail && \
        srcs=\$(find src tests \\( -name '*.cpp' -o -name '*.hpp' -o -name '*.cc' -o -name '*.h' \\) -not -path '*/_old/*') && \
        printf '%s\\n' \"\$srcs\" | xargs clang-format --dry-run --Werror && \
        cmake --preset test && \
        tus=\$(find src tests \\( -name '*.cpp' -o -name '*.cc' \\) -not -path '*/_old/*') && \
        printf '%s\\n' \"\$tus\" | xargs clang-tidy -p build/test --warnings-as-errors='*' && \
        cmake --build --preset test && \
        QEMU_LD_PREFIX={{qemu_prefix}} ctest --preset test --output-on-failure"

# --- local deploy loop ----------------------------------------------------

# Local validation loop — cross-build the release binary and install it straight
# to a Jetson over ssh, WITHOUT cutting a release or minting a tag. The build
# runs in the container; scp/ssh run on the HOST (which holds your ssh keys /
# ~/.ssh/config), so you need `ssh`/`scp` on the host PATH and key-based access
# to the device. Validate here; only push (and let CI mint a tag) once it works.
#
# Usage: just deploy-firmware user@jetson.local
#        just deploy-firmware jetson.local --no-camera   # extra install.sh flags pass through
deploy-firmware HOST *flags: build-release-firmware
    @BIN="build/release/bin/sst_cam_firmware"; \
     [ -f "$BIN" ] || { echo "binary not found: $BIN (build-release failed?)" >&2; exit 1; }; \
     command -v scp >/dev/null || { echo "scp not found on host PATH" >&2; exit 1; }; \
     echo "Copying ${BIN} + install.sh to {{HOST}} ..."; \
     scp "$BIN" deploy/install.sh "{{HOST}}:/tmp/" || exit 1; \
     echo "Running install.sh --binary on {{HOST}} (sudo) ..."; \
     ssh -t "{{HOST}}" "sudo bash /tmp/install.sh --binary /tmp/sst_cam_firmware {{flags}}"

# --- housekeeping ---------------------------------------------------------

# Remove build trees (config + objects; the Conan cache is untouched).
clean:
    @just _run "rm -rf build"

# --- internals ------------------------------------------------------------

# Run CMD natively inside the container, else delegate to it via the CLI.
[private]
_run +cmd:
    @if [ -f /.dockerenv ]; then \
        bash -lc "{{cmd}}"; \
    else \
        _DC="$(command -v devcontainer || echo "$HOME/.devcontainers/bin/devcontainer")"; \
        "$_DC" exec --workspace-folder "{{justfile_directory()}}" bash -lc "{{cmd}}"; \
    fi
