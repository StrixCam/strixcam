# Deploying sst-cam-firmware to a Jetson

This directory ships the manual deployment path for the firmware:

- `install.sh` — fetches a GitHub Release asset and swaps the running binary, idempotently and safely.
- `sst-cam-firmware.service` — the systemd unit that supervises the binary.

The firmware is distributed as a **private** GitHub Release asset named
`sst_cam_firmware-<tag>-aarch64`. `install.sh` installs a **released** binary —
either a **stable** `vX.Y.Z` (promoted to `main` by
`.github/workflows/release.yml`) or a **beta** `vX.Y.Z-beta.N` (cut on a
`release/X.Y.Z` branch by `.github/workflows/release-beta.yml`) for on-device
hardware sign-off. The stable asset is the exact, SHA-256-verified beta binary
re-published unchanged — `main` never rebuilds. (See `CLAUDE.md` and `docs/ci/`
for the full branch model and maturity ladder.)

---

## One-time setup (on the Jetson)

Run these once per device.

### 1. Create the service user and install directory

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin sst-cam
sudo mkdir -p /opt/sst-cam/bin
sudo chown -R sst-cam:sst-cam /opt/sst-cam
```

> The service runs as the non-root `sst-cam` user. If the firmware needs
> hardware access (cameras, GPIO, NVENC, etc.), add `sst-cam` to the relevant
> groups (e.g. `video`) and/or install the needed udev rules — do **not** run
> the service as root.

### 2. Install the systemd unit

```bash
sudo cp deploy/sst-cam-firmware.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable sst-cam-firmware
```

The service won't start successfully until a binary is installed (next step).

### 3. Provide the GitHub auth token

The repo is private, so downloading a Release asset requires a token with read
access to the repo's releases (a fine-grained PAT scoped to the repo, a classic
PAT, or a deploy token — **operator/security decision, see the plan**).

Pass it inline to `install.sh` (it is read from the `GITHUB_TOKEN` environment
variable). Avoid persisting it in shell history; prefer a root-only file:

```bash
sudo install -m 600 /dev/stdin /etc/sst-cam/github_token <<< 'ghp_xxx'   # example
```

---

## Updating the firmware

Install the latest release:

```bash
sudo GITHUB_TOKEN=ghp_xxx deploy/install.sh
```

Install a specific version (a stable `vX.Y.Z` or a beta `vX.Y.Z-beta.N` for
hardware sign-off):

```bash
sudo GITHUB_TOKEN=ghp_xxx deploy/install.sh --version v1.2.3
sudo GITHUB_TOKEN=ghp_xxx deploy/install.sh --version v0.1.0-beta.1
```

If you stored the token in a file:

```bash
sudo GITHUB_TOKEN="$(cat /etc/sst-cam/github_token)" deploy/install.sh --version v1.2.3
```

### What `install.sh` does

1. **Fails early** (before stopping the service) if the token is missing, the
   download fails, or the asset is not a valid aarch64 ELF — no downtime on a
   bad fetch.
2. **No-op** if the installed binary already matches the release (sha256) — no
   needless restart.
3. Stops the service, **atomically** swaps in the new binary (keeping a
   `.bak` backup), and restarts the service.
4. If the new binary fails to become active, **restores the backup**, restarts,
   and exits non-zero with a clear message.
5. Prints the final `systemctl is-active` status.

### Requirements on the device

`install.sh` needs `curl`, `jq`, `file`, `sha256sum`, and `systemctl` on PATH.
Install `jq` if missing: `sudo apt-get install -y jq`.

---

## Confirm before first production use

- The service name (`sst-cam-firmware`) and install path
  (`/opt/sst-cam/bin/sst_cam_firmware`) match what is hard-coded in
  `install.sh` and the unit. Change both together if the device layout differs.
- The chosen token strategy grants only release read access and is stored
  root-only on the device.
