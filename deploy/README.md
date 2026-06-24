# Deploying sst-cam-firmware to a Jetson

The firmware ships as a **public** GitHub Release with two assets:

- `sst_cam_firmware-<tag>-aarch64` — the cross-built binary (JetPack 7.2 / L4T r39.2).
- `install.sh` — a **self-contained** installer, bundled on the release so the
  device never needs a repo clone.

A release is either a **beta** `vX.Y.Z-beta.N` (cut on `release/X.Y.Z` for
on-device sign-off) or a **stable** `vX.Y.Z` (the exact, SHA-256-verified beta
binary promoted to `main` unchanged). See `CLAUDE.md` / `docs/ci/` for the ladder.

---

## Install / update — one command

On the Jetson (must be on **JetPack 7.2 / L4T r39.2**), run the installer straight
off the release. The **first run sets everything up automatically** (creates the
`sst-cam` system user, `/opt/sst-cam`, and the systemd unit) — no manual steps.

```bash
# latest release
curl -fsSL https://github.com/ScoutSportTechnology/sst-cam-firmware/releases/latest/download/install.sh \
  | sudo bash

# a specific version (by tag)
curl -fsSL https://github.com/ScoutSportTechnology/sst-cam-firmware/releases/latest/download/install.sh \
  | sudo bash -s -- --version v0.1.0-beta.2

# a specific binary (by recorded sha256 — finds the release with that digest)
curl -fsSL https://github.com/ScoutSportTechnology/sst-cam-firmware/releases/latest/download/install.sh \
  | sudo bash -s -- --sha256 82ec55b03d45a9383d332d469666a59b4cb58074017cb8499cd0ca8b2be2dfff
```

> `releases/latest/download/install.sh` always serves the newest installer; it
> can still install any older `--version` / `--sha256` you ask for.

If you have the repo checked out on the device, `sudo deploy/install.sh [...]`
is equivalent.

Watch it run:

```bash
journalctl -u sst-cam-firmware -f
systemctl status sst-cam-firmware
```

---

## What `install.sh` does

0. **One-time setup (idempotent)** — creates the `sst-cam` user, `/opt/sst-cam/bin`,
   and installs + enables the systemd unit if any are missing. Re-running is safe.
   Skip with `--no-setup`.
1. **Resolves the release** by `--version <tag>` or `--sha256 <hex>` (default `latest`).
2. **Fails early** (before stopping the service) on a bad download, a non-aarch64
   ELF, a **platform mismatch** (a JetPack-7.2 binary on a non-r39 flash), or a
   **sha256** that disagrees with the digest recorded in the release notes.
3. **No-op** if the installed binary already matches (sha256).
4. Stops the service, **atomically** swaps in the new binary (keeps a `.bak`),
   restarts.
5. **Rolls back** to the backup if the new binary fails to become active.

### Options & environment

| Flag / env | Effect |
|---|---|
| `--version vX.Y.Z` | Install a specific release tag. |
| `--sha256 <hex>` | Install the release whose recorded binary digest matches (and verify the download against it). |
| `--no-setup` | Skip the one-time setup check. |
| `GITHUB_TOKEN` | Optional; lifts the 60 req/hr GitHub API rate limit only (sent on metadata requests, never on the asset download). |
| `SST_SKIP_PLATFORM_CHECK=1` | Bypass the JetPack-7.2 device guard. |

### Device requirements

`curl`, `jq`, `file`, `sha256sum`, `systemctl` on PATH (`sudo apt-get install -y jq`
if missing). Hardware access (cameras, GPIO, NVENC, BlueZ) may need extra group
membership / udev rules for the `sst-cam` user — `install.sh` adds it to `video`
when that group exists, but do **not** run the service as root.

The firmware reads its config from `/etc/sst/cam/config/*.json`; without config
and connected cameras the service will start then exit — that is expected until
the device is fully provisioned.
