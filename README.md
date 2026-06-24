# SST Cam Firmware

**On-device firmware for the SST Cam — an open-source AI-assisted sports camera (work in progress).**

This repository contains the firmware and runtime stack that runs directly on the camera hardware (NVIDIA Jetson Orin Nano). It speaks the shared [`sst-cam-proto`](https://github.com/ScoutSportTechnology/sst-cam-proto) wire contract to the [mobile app](https://github.com/ScoutSportTechnology/sst-cam-app) over BLE (control) and WiFi Direct (preview + downloads).

---

## Roadmap

System-wide arc — this repo's slice marked per phase. Module-level detail in
[Module status](#module-status) below.

| Phase | Theme | Firmware status |
| ----- | ----- | --------------- |
| 1 | **Contract & scaffolding** — wire spec, dev container, hardware bring-up | ✅ done |
| 2 | **Connect & control** — BLE/WiFi modules, command handling, telemetry | ✅ done |
| 3 | **Capture & transfer** — dual-camera capture, buffers, recording, preview | 🚧 capture + buffer + processing + orchestration + storage + streaming + overlay built (single-cam, software-encode + dual-cam + decision seam + raw capture = hardware-demo work) |
| 4 | **Intelligence** — AI tracking, physics, camera/crop decision | ⬜ not started |
| 5 | **Broadcast** — overlays, streaming output | ⬜ not started |

---

## ⚠️ Development Requirements

This project currently targets only one platform:

- NVIDIA Jetson Orin Nano
- JetPack 7.2
- L4T r39.2 (Ubuntu 24.04)

Development is done exclusively via the **Dev Container** in this repo, which cross-compiles for aarch64 from an x86_64 host. Native on-device builds are not supported.

1. Install the **Dev Containers** VSCode extension (`ms-vscode-remote.remote-containers`).
2. Open the repo and run **Dev Containers: Reopen in Container**.
3. Once inside the container, configure and build with the `debug` preset (see CLAUDE.md for the full preset list).

Raspberry Pi 5 support has been dropped. All development efforts are focused entirely on Jetson.

---

## Hardware Platform

Current hardware configuration:

- NVIDIA Jetson Orin Nano
- Dual IMX477 HQ cameras
- Planned microphone integration with dual MAX4466 modules

The Raspberry Pi 5 + dual IMX708 prototype phase is complete and no longer supported.

---

## Language & Architecture

The project originally started in Python for rapid prototyping and hardware bring-up.

It has been fully rewritten in modern C++ for:

- Deterministic performance
- Lower latency
- Explicit memory control
- Long-term maintainability on embedded systems

The Python implementation is deprecated and no longer maintained.

---

## Pipeline

End-to-end intended flow:

```text
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

- **Capture** runs one GStreamer pipeline per IMX477. The appsink is capped at 5 in-flight buffers.
- **Preprocess** turns raw NV12 into a `FrameBundle{ source_frame, ai_frame }`. Grayscale / Binary AI modes touch only the Y plane — no color conversion on the hot path.
- **Materialize** deep-copies the source out of the GstBuffer right before the bundle enters the per-camera buffer, so nothing downstream pins the appsink.
- **AI / tracking** runs per camera and produces detections (field, ball, eventually players).
- **Physics** consumes both cameras' detections and computes trajectory / world-coordinate state.
- **Decision** picks which camera's frame + a crop / zoom rect, then hands them to postprocess.
- **Postprocess** runs once, on the chosen frame only — NV12→BGR, crop, resize, format-convert. Pushes into the final buffer.
- **Overlay** runs in parallel (banner, scoreboard, event info from user/app state).
- **Storage** and **streaming** each consume the final buffer independently and composite the overlay on top if the user enabled it. Either can be on or off.

---

## Module status

Built and working:

- [x] **Config** — JSON device / calibration / storage configs
- [x] **Capture** — GStreamer adapter for dual IMX477
- [x] **Buffer** — `LatestOnlySlot`, `DropOldestRing`, plus a `MaterializeFrame` helper so producers can release the GstBuffer the moment a frame enters a downstream buffer
- [x] **Network / control** — WiFi and Bluetooth BLE modules
- [x] **Processing** — `IPreprocessor` / `IPostprocessor` ports + OpenCV adapter (Grayscale / Binary / RGB AI modes; crop + resize + format-convert post)
- [x] **Pipeline orchestration** — `PipelineOrchestrator` wires capture → preprocess → materialize → buffer → postprocess → fan-out (two worker threads). Single-camera + inline full-frame crop today; the dual-camera + `IDecision` seam is the hardware-demo work
- [x] **Storage** — `RecordingService` + GStreamer recorder write MP4s; `DownloadServer` enumerates them and mints TTL tokens
- [x] **Streaming** — `StreamingService` fans out to RTSP app-stream + RTMP adapters (RTSP not yet started in production)
- [x] **Overlay** — cairo renderer + GStreamer compositor + proto→domain mapper, built but not yet wired into the final-frame path

Not started:

- [ ] **Decision** — picks which camera's frame + crop / zoom rect; hands off to postprocess (hardware demo adds a static cam-0 `StaticDecision` behind the `IDecision` port)
- [ ] **AI / tracking** — TensorRT model + adapter; field and ball first, players + jersey numbers later. One inference per camera
- [ ] **Physics** — ball trajectory / world-coordinate projection from both cameras' detections
- [ ] **Microphone** — MAX4466 dual-mic integration

> No database module exists (no SQLite, no `db/`); recordings are filesystem-enumerated. The Orin Nano has no NVENC — encode is software `x264enc` (see CLAUDE.md).

---

## Development Notes

This is a personal project developed solo and iteratively.

- The architecture evolves as hardware, performance constraints, and field testing shape the design
- Expect work-in-progress quality and breaking changes
- Feedback, critiques, and performance insights are welcome

---

## Hardware & 3D Parts

Custom 3D-printed parts are used for mounting and assembly.

If you are building your own unit, the STL files can be shared upon request.

---

## Contributing

- Feedback and ideas are welcome
- Code reviews and performance suggestions are appreciated
- If you are experimenting with similar Jetson hardware setups, sharing results is encouraged

---

## Releases & branch model

Development follows the SST branch model `feat/* → development → release/X.Y.Z → main`
with a three-rung release maturity ladder:

- **alpha** (`vX.Y.Z-alpha.N`) — pushed to `development`; the devcontainer
  cross-build + container `ctest` in isolation, no hardware.
- **beta** (`vX.Y.Z-beta.N`) — cut on a `release/X.Y.Z` branch; the aarch64
  binary flashed to a **real Jetson** and tested **with the app**.
- **stable** (`vX.Y.Z`) — promoted on merge to `main`: the verified beta binary
  re-published unchanged (its SHA-256 is checked before promotion).

Three branch-scoped product workflows drive this — `release-alpha.yml` owns
`development` (PR checks + the alpha build), `release-beta.yml` owns `release/**`
(PR checks + the beta build), and `release.yml` owns `main` (promote only). The
cross-build devcontainer is built **once per `.devcontainer/**` content** by an
in-CI `image` job (content-hash tag in GHCR) and pulled by `tidy`/`test`/the
release build — not rebuilt per run; docs/workflow-only PRs skip the heavy
checks (see `CLAUDE.md`). Two invariants hold: **`main` runs no failable build**
— it only promotes the already-built beta binary — and **no CI job commits back
to `main`**. The aarch64 binary is published as a GitHub
Release asset, `sst_cam_firmware-<tag>-aarch64`. `deploy/install.sh` installs a
released (stable or beta) binary onto a Jetson. See `CLAUDE.md` and `docs/ci/`
for the workflow, ruleset, and version-reset runbooks.

---

## Related repos

- [`sst-cam-app`](https://github.com/ScoutSportTechnology/sst-cam-app) — Flutter companion app (BLE control + WiFi preview)
- [`sst-cam-proto`](https://github.com/ScoutSportTechnology/sst-cam-proto) — shared BLE/WiFi wire contract (consumed as a submodule at `proto/`)
- [`sst-cam-emulator`](https://github.com/ScoutSportTechnology/sst-cam-emulator) — cross-stack bridge to test app ↔ firmware without a Jetson

---

⚽ **Goal:** Build a fully open-source sports camera capable of streaming, tracking, capturing, and broadcasting with overlays — bringing professional-style coverage to any field.

<!-- ci path-filter skip test -->
