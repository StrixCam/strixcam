# Device-ID Provisioning at Install — Requirements

**Date:** 2026-06-26
**Status:** Ready for planning
**Repo:** sst-cam-firmware
**Scope:** Lightweight (single script + guard test; no firmware code change)

## Problem

Every freshly-installed camera advertises the identical BLE name **`sst-cam-0000`**
(the user saw "sst-camm-0000" — the double-m is a typo; firmware only ever emits
`sst-cam-NNNN`). The trailing id should be unique per unit, generated and persisted at
initial setup by `install.sh`. Today it is neither hardcoded nor random — it is the
deterministic render of a placeholder serial that `install.sh` never replaces.

## Root cause (confirmed from code)

- Advertised name is computed: `MakeAdvertisedName(DeriveUnitNumber(serial_number))`
  (`src/main.cpp:203-205`), format `fmt::format("{}{:04}", "sst-cam-", unit % 10000)`
  (`src/domain/control/utils/advertised-name.hpp:24-26`).
- `serial_number` is read from `/etc/sst/cam/config/device.json`. On first boot the
  firmware writes the built-in default `"serial_number": "00000000"`
  (`src/app/config/services/config_loader/config-defaults.hpp:17`) via
  `EnsureDefault` (never clobbers an existing file, `config-loader.cpp:50-64`).
- Empty/zero serial → `DeriveUnitNumber` → 0 → `sst-cam-0000`.
- `install.sh` creates + chowns `/etc/sst/cam/config` (`deploy/install.sh:47,234-239`)
  but writes **no config files** and contains **no** serial/id/uuid/random generation
  (grep confirms). The design comment defers first-boot JSON to the firmware. The repo
  has `MakeUuidV4` (`src/domain/common/utils/uuid.hpp`) but it's used only for
  match/recording/clip ids, never the device serial.

The gap: nothing upstream ever sets a unique serial, so every unit is `sst-cam-0000`.

## Decision

Provision the serial in `install.sh` (it already owns one-time per-device setup and the
config dir). No firmware code change — the read path already consumes `serial_number`.

## Approach
1. **Generate** a unique numeric id once in `ensure_setup()` (e.g. derive digits from
   `/etc/machine-id`, or `/dev/urandom`). Use low digits so `DeriveUnitNumber`'s
   `% 10000` gives a stable 4-digit suffix.
2. **Persist** by writing `/etc/sst/cam/config/device.json` (full schema from
   `config-defaults.hpp`, only `serial_number` substituted) with that id, **before**
   first boot — but only if the file does not already exist (mirror the firmware's
   never-clobber rule → idempotent re-runs never change an existing unit's identity).
   Chown to `sst-cam` (dir already chowned at `deploy/install.sh:239`).
3. **No firmware change.** Because install writes the file first, firmware `EnsureDefault`
   sees it exists and won't overwrite.

## Success criteria
- A fresh install yields a unit advertising `sst-cam-NNNN` with NNNN unique per device.
- Re-running `install.sh` does not change an already-provisioned unit's id.
- `deploy/install-test.sh` guards the new step (mirrors its existing chown guard at
  `deploy/install-test.sh:84`).

## Open questions / notes
- `DeriveUnitNumber` truncates to 4 digits (`% 10000`) — collisions possible across a
  >10k fleet. Acceptable for current demo scope; flag if true global uniqueness needed
  (the `sst-cam-NNNN` 4-digit format is fixed at `advertised-name.hpp:21-22`).
- Source of randomness: `/etc/machine-id` is stable per-install and avoids needing
  `/dev/urandom` entropy at first boot — preferred unless re-imaging must yield a new id.

## Dependencies / assumptions
- Lands on a `fix/*` branch in sst-cam-firmware off the current `release/0.1.0`.
- Independent of the app race-class work; can proceed in parallel.
