# Deploying sst-cam-firmware to a Jetson

Each GitHub Release carries **one** asset — the cross-built binary
`sst_cam_firmware-<tag>-aarch64` (JetPack 7.2 / L4T r39.2). The installer
(`deploy/install.sh`) lives only here in the repo — the single source of truth —
and is run straight from the repo at the tag you're installing (the exact command
is printed in every release's notes). A release is either a **beta**
`vX.Y.Z-beta.N` (cut on `release/X.Y.Z` for on-device sign-off) or a **stable**
`vX.Y.Z` (the SHA-256-verified beta binary promoted to `main` unchanged). See
`CLAUDE.md` / `docs/ci/` for the ladder.

---

## Install / update — one command

On the Jetson (must be on **JetPack 7.2 / L4T r39.2**), pull the installer from the
repo at the tag and run it. The **first run sets everything up automatically**
(creates the `sst-cam` system user, `/opt/sst-cam`, and the systemd unit) — no
manual steps. (This exact line is also in each release's notes — copy it from there.)

```bash
# install a specific release (replace the tag)
curl -fsSL https://raw.githubusercontent.com/ScoutSportTechnology/sst-cam-firmware/v0.1.0-beta.3/deploy/install.sh \
  | sudo bash -s -- --version v0.1.0-beta.3
```

If the repo is checked out on the device, `sudo deploy/install.sh --version <tag>`
is equivalent. To pin by digest instead of tag, add `--sha256 <hex>` (it finds the
matching release and verifies the download); a repo clone is easiest for that.

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
