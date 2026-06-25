---
title: "install.sh must provision every host prerequisite the binary assumes"
date: 2026-06-24
category: conventions
module: deploy/install.sh
problem_type: convention
component: tooling
severity: high
applies_when:
  - "Writing or reviewing an install/deploy script for a cross-compiled binary"
  - "The binary dynamically links libraries present only in the build sysroot"
  - "A systemd unit runs the service as a non-root user that must read/write host dirs"
  - "Config or state dirs are compiled-in paths (e.g. /etc/...) the service self-populates on first boot"
  - "Adding a system dep to the sysroot, or a new compiled-in path the firmware touches"
symptoms:
  - "`error while loading shared libraries: libsdbus-c++.so.1` (exit 127), crash-loop"
  - "`fatal error: failed to load configuration files` (exit 1) after `Cannot open config file` x4"
  - "Rollback restores a backup carrying the same missing host prerequisite"
  - "CI cross-build and ctest pass while the binary fails on a clean JetPack 7.2 flash"
root_cause: incomplete_setup
resolution_type: tooling_addition
related_components:
  - development_workflow
  - documentation
tags:
  - install-script
  - cross-compilation
  - runtime-dependencies
  - shared-libraries
  - file-permissions
  - systemd
  - jetpack
  - preflight-check
---

# install.sh must provision every host prerequisite the binary assumes

## Context

This firmware is **cross-compiled** in a dev container whose sysroot is deliberately
fattened beyond a base JetPack flash: `.devcontainer/sysroot/noble-deb-urls.txt` +
`003_install_extra_pkgs.sh` add noble `.deb`s (`sdbus-c++`, `gst-rtsp-server`, …) so the
build links and the binary loads *inside the container*. The deploy target is a different
world:

1. A **base JetPack 7.2 flash does not have those packages.** Some runtime *is* in the L4T
   r39.2 sample rootfs (protobuf, opencv — the `003` header says so), so it is not "install
   everything we link"; it is "install exactly the delta the flash lacks."
2. The systemd unit runs the binary as a **non-root user** (`User=sst-cam`, embedded in
   `install.sh embedded_unit()`), so it cannot create or write anything under root-owned
   `/etc`.

Both gaps are invisible at build time and invisible in the container test suite. They
surface only as a crash-loop on a freshly flashed device — exactly when you have the least
visibility. This is the same shape the codebase already flags for GStreamer plugins
("`gst_parse_launch` resolves elements at runtime → container build stays green, fails only
on-device"); the convention below generalizes that insight from plugins to shared-lib
loading and directory provisioning.

## Guidance

**`install.sh` must provision every host prerequisite the binary assumes** — not just drop
the binary in place. Concretely:

- **(a) Runtime library packages absent from the base flash.** If the binary `DT_NEEDED`s a
  `.so` the flash doesn't ship, name its package in a single explicit list and install only
  what's missing.
- **(b) Directories the service user reads or writes** — especially compiled-in paths under
  root-owned `/etc`. Create them and `chown` them to the service user, because the non-root
  user cannot create them itself.

Two cross-cutting rules make the provisioning robust rather than cosmetic:

- **Fail loud and early, before you touch the running service.** A preflight
  `ldd … | grep "not found"` that `die`s *before* `systemctl stop`. The anti-pattern it
  kills: stop the service → swap in an unloadable binary (exit 127) → "roll back" to a
  backup carrying the *same* missing dep → service is down either way, now with a confusing
  rollback log. A pre-swap `die` protects the already-running service and hands the operator
  the exact missing libs plus the apt line to fix them.
- **Single source of truth + a drift test for each provisioned thing.** The runtime-dep list
  must track the sysroot manifest; the provisioned config dir must byte-match the
  compiled-in path. Encode both as static assertions in `deploy/install-test.sh`, which runs
  in CI's `ci-scripts` job — no device, no apt, no network.

## Why This Matters

It converts silent crash-loops (`status=127`, `fatal error: failed to load configuration
files`, exit 1, restart, repeat) into a single actionable **pre-swap** error message, and it
keeps a healthy running service running when a deploy can't succeed. It also pre-empts the
*next* instance of the same gap — the codebase already names candidates: `x264enc` lives in
`gstreamer1.0-plugins-ugly` (resolved at `gst_parse_launch` runtime, so it builds green and
fails on-device), and camera/GPIO/NVENC access needs extra group membership / udev rules for
`sst-cam` (the embedded unit comments call this out). Each is the same shape: a host
prerequisite the binary assumes that the flash doesn't provide.

## When to Apply

- Any new system dep added to the sysroot (`003_install_extra_pkgs.sh` /
  `noble-deb-urls.txt`) whose **runtime** isn't in the base flash → add its package to
  `RUNTIME_DEPS`.
- Any new compiled-in path the firmware **reads or writes** at runtime → provision + `chown`
  it in `ensure_setup`, and add a drift assertion tying it to the source constant.
- Any change to the **service user or unit** (a new `User=`, a new `WorkingDirectory`, a new
  hardware group) → re-check what that identity can and cannot touch.

## Examples

### Worked example 1 — missing runtime shared libraries (PR #22 / v0.1.0-beta.6)

The binary dynamically links `libsdbus-c++.so.1` (BLE control plane) and
`libgstrtspserver-1.0-0` (app RTSP), pulled in via `pkg_search_module(... sdbus-c++ ...)` /
gst-rtsp-server in `CMakeLists.txt`. Those come from the fattened sysroot, not the flash. On
a fresh device the service crash-looped with
`error while loading shared libraries: libsdbus-c++.so.1`, systemd `status=127`; install.sh
"rolled back" to a backup carrying the *same* missing dep.

Fix — a single explicit list, an idempotent installer, and a pre-stop gate:

```bash
# deploy/install.sh — source of truth = .devcontainer/sysroot/003_install_extra_pkgs.sh
RUNTIME_DEPS=(libsdbus-c++1 libgstrtspserver-1.0-0)

ensure_runtime_deps() {                     # idempotent: dpkg-query skips present pkgs
  local pkg missing=()
  for pkg in "${RUNTIME_DEPS[@]}"; do
    dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q "install ok installed" \
      || missing+=("$pkg")
  done
  [ "${#missing[@]}" -eq 0 ] && return 0
  apt-get install -y --no-install-recommends "${missing[@]}" || log "warning: ..."
}

check_runtime_libs() {                      # the HARD guarantee; runs BEFORE service stop
  local missing_libs
  missing_libs="$(ldd "$1" 2>/dev/null | awk '/not found/ {print $1}' | sort -u)"
  [ -n "$missing_libs" ] || return 0
  die "the new binary needs shared libraries that are missing on this device: ..."
}
```

Critically, in the main flow `check_runtime_libs "$new_bin"` is the line **immediately
before** `systemctl stop "$SERVICE"`. The gate dies while the old service is still up — the
swap never happens with an unloadable binary.

### Worked example 2 — config dir not writable by the non-root service user (PR #23 / v0.1.0-beta.7)

`src/main.cpp` compiles in `kConfigDir = "/etc/sst/cam/config"`. On first boot the firmware
self-writes default JSON there via `ConfigLoader::EnsureDefault`
(`src/app/config/services/config_loader/config-loader.cpp`):
`create_directories(path.parent_path())` then `std::ofstream out(path, …)`. But the unit
runs `User=sst-cam`, and install.sh originally provisioned only `/opt/sst-cam`, never
`/etc/sst`. The non-root write into root-owned `/etc` failed silently — `EnsureDefault` logs
and returns, then `get()` loads four missing files → `Config load failed: device` →
`throw std::runtime_error("failed to load configuration files")` → `main` exits 1 →
crash-loop.

Fix — provision the dir and hand it to the service user in `ensure_setup`:

```bash
# deploy/install.sh
CONFIG_DIR="/etc/sst/cam/config"   # MUST match src/main.cpp kConfigDir (drift-tested)

# ...in ensure_setup():
if [ ! -d "$CONFIG_DIR" ]; then
  mkdir -p "$CONFIG_DIR"
fi
chown -R "${SERVICE_USER}:${SERVICE_USER}" "/etc/sst"
```

The firmware keeps its self-provisioning behavior (good for an operator-editable file it must
never clobber); install.sh just makes the directory the non-root user can't create itself
exist and be owned correctly.

### The drift tests (`deploy/install-test.sh`, run in CI `ci-scripts`)

Static contract checks — no device, apt, or network. The two that enforce the
single-source-of-truth rule:

```bash
# RUNTIME_DEPS <-> sysroot manifest: every dep must be a package the build sysroot
# actually pulls (so the list can't name a phantom pkg, and a sysroot rename trips this).
# noble-deb-urls.txt URL-encodes '++' as '%2b%2b'; filename is "<pkg>_<version>_<arch>.deb".
for d in "${DEPS[@]}"; do
  enc="${d//+/%2b}"                         # libsdbus-c++1 -> libsdbus-c%2b%2b1
  grep -qiE "/${enc}_[0-9]" "$SYSROOT_URLS" || bad "RUNTIME_DEPS '$d' not in sysroot (drift?)"
done

# CONFIG_DIR <-> src/main.cpp kConfigDir: a mismatch silently reintroduces bug 2
# (firmware reads one path, install.sh provisions another).
main_dir="$(sed -nE 's/.*kConfigDir *= *"([^"]+)".*/\1/p' "$MAIN_CPP" | head -n1)"
[ "$main_dir" = "$config_dir" ] || bad "CONFIG_DIR ('$config_dir') != kConfigDir ('$main_dir')"
```

The suite also asserts `RUNTIME_DEPS` is non-empty (an empty list silently reintroduces
bug 1), that both known packages are present, that the preflight `check_runtime_libs` line
number is **less than** the `systemctl stop` line number (ordering is the whole point), and
that `ensure_setup` both `mkdir -p "$CONFIG_DIR"` and `chown -R … "/etc/sst"`. So the next
sysroot add or path change that forgets to provision its host prerequisite fails the build
instead of the Jetson.

## Related

- `docs/solutions/tooling-decisions/ci-cd-release-pipeline-2026-06-15.md` — owns
  `deploy/install.sh` + the systemd unit as the on-Jetson install mechanism; predates these
  host-provisioning fixes. Its one-line install.sh description is now incomplete — this doc
  is the host-prerequisite half.
- `docs/plans/2026-06-22-001-feat-jetpack-7.2-sbsa-retarget-plan.md` — defines the
  `EXPECTED_L4T_MAJOR` platform guard and names `003_install_extra_pkgs.sh`, the
  source-of-truth list `RUNTIME_DEPS` derives from.
- `docs/plans/2026-06-10-001-feat-hardware-demo-pipeline-firmware-plan.md` — same
  "container-green ≠ device-runnable" class, for GStreamer plugins resolved at runtime.
- PRs: **#22** `fix(deploy): install runtime shared-lib deps + preflight ldd gate` (beta.6),
  **#23** `fix(deploy): provision /etc/sst/cam/config for non-root service user` (beta.7).
