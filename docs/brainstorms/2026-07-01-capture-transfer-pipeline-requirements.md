---
date: 2026-07-01
topic: capture-transfer-pipeline
---

# Capture & Transfer Pipeline — Encode Budget, Training Proxy, Color & Focus

## Summary

Reclaim CPU-encode budget on the Orin Nano so AI can run next: keep H.264 everywhere, move scale/colour-convert onto VIC hardware to chase a sustained 1080p60 default, ship the always-on tiny H.264 training proxy on every match, boot headless, and fix two non-encode camera-quality defects (the both-cameras ISP-level pink cast, and add continuous autofocus).

---

## Problem Frame

The Jetson Orin Nano is already near its CPU ceiling with recording, streaming, preview, BLE/WiFi control, and overlays running — and the most expensive job, AI detection, has not even started. The board has **no NVENC**, so every H.264 encode is software `x264enc` on the CPU, and one encoder is shared across record + RTSP preview + RTMP stream. Software x264 cannot sustain 1080p60 (~0.64× realtime measured on-device), so the current advertised ceiling is 1080p30 / 720p60. The training-footage feature that would feed the model is wired end-to-end but **dead** — nothing triggers it, and its sink writes uncompressed NV12 (~186 MB/s for both cameras), which is unusable. Separately, both cameras render a magenta/pink cast, and the lens has no focus control. Every ambition here — higher resolution, training data, AI — competes for the same CPU encode budget, so the budget is the constraint that shapes the whole cycle.

---

## Actors

- A1. Operator: runs a match from the app — starts/stops record and stream, opens live preview, adjusts focus.
- A2. App (`sst-cam-app`): sends control commands over BLE / WiFi-Direct; connects to the preview stream.
- A3. Firmware (`sst-cam-firmware`): the GStreamer capture/encode/record/stream runtime on the Jetson.

---

## Key Flows

- F1. Match record + training proxy lifecycle
  - **Trigger:** Operator hits "start match record" (and/or "start stream") in the app.
  - **Actors:** A1, A2, A3.
  - **Steps:** App mints a capture-group id and sends record START → firmware begins the H.264 match recording → the same START also triggers the training-proxy start (both cameras, tiny H.264). On STOP, both the match recording and the proxy stop together.
  - **Outcome:** One final H.264 MP4 per match, plus two tiny per-camera proxy files, all tied to the same capture-group id.
  - **Covered by:** R7, R8, R9.

---

## Requirements

**Record path & encode budget**
- R1. Recording stays H.264 (`x264enc`) end-to-end — no MJPEG-record, no offline MJPEG→H.264 transcode, no "saving" transcode step. `src/domain/common/models/video-quality.hpp` (`kSupportedVideoModes`) remains the single source of truth for advertised modes and record/stream validation.
- R2. Move `videoscale` + colorspace-convert off the CPU onto VIC (`nvvidconv`) for the encode branches, re-adding the NVMM→sysmem copy `x264enc` needs, and measure the CPU freed on-device.
- R3. Target **1080p60 as the default** record/stream mode, contingent on the VIC spike (R2) sustaining it on-device. Floor modes 1080p30 and 720p60 remain. If 1080p60 is not sustained, the default stays 1080p30 and 1080p60 remains unadvertised.
- R4. Do not add 2K/4K recording on this hardware — the record ladder is capped at what x264 + VIC sustains.

**CPU reclaim**
- R5. Boot the Jetson headless (no desktop UI) to reclaim baseline CPU/RAM. Reversible target switch, not a reflash.
- R6. Keep the GPU (CUDA/TensorRT) reserved for AI inference; do not offload video encode to it (no NVENC exists). This cycle frees CPU budget so AI can run next; building the AI model itself is out of scope.

**Training proxy**
- R7. Wire the currently dead raw-capture path so a match record START triggers proxy start (with a minted capture-group id) and record STOP triggers proxy stop. App caller in `lib/features/camera/raw_capture_state.dart` (currently uncalled) hooked to the record/stream START/STOP sites; firmware lifecycle in the raw-capture handler.
- R8. Change the raw sink from uncompressed NV12 to a tiny always-on compressed proxy: **H.264, ~480p @ ~15 fps, per-camera (both cameras independently)** — training/archive footage, not viewing footage.
- R9. The proxy runs on **every** match regardless of the main record resolution (not gated to low tiers) — training coverage must include high-resolution matches.
- R10. Add a retention/cleanup policy for proxy files (age/size-capped rotation) so accumulation stays bounded.

**Camera quality (non-encode)**
- R11. Diagnose and fix the pink/magenta cast affecting **both** cameras as an **ISP/capture-level** defect. A prior on-device test proved the cast is present in the raw `nvarguscamerasrc → nvjpegenc` output *before* any firmware conversion — so `opencv-postprocessor.cpp`'s NV12→BGR convert is not the cause and is not the target. Pursue: (a) a `.nito` ISP tuning calibrated for **this** ArduCAM module (badged `jakku_*_RBPCV3`; the stock NVIDIA reference-imx477 tuning does not match, and installing it did not fix the cast), (b) the **IR-cut filter** (uniform magenta is classic IR contamination), and (c) the capture caps / sensor mode. A pipeline-level `videobalance`-style correction is the fallback only if module-matched tuning cannot be sourced. Both cameras affected uniformly.
- R12. Add **continuous autofocus**: a firmware I2C/VCM lens-motor driver + a sharpness metric (Laplacian variance on the Y plane, reusing the grayscale AI frame `preprocess` already produces) + a sweep/peak/hold focus loop, per camera. Wire the existing dead `CameraFocus{kAuto,kManual}` field so the app toggles auto vs manual, and in manual sets an explicit position. AF is contrast-detection (no phase-detect hardware); the sharpness metric runs during a hunt, not every frame at steady state.

**Docs**
- R13. Correct the stale `CLAUDE.md` claim that "RTSP StartAppStream has no production caller yet" — `WifiDirectHandler::HandleStart` is a live production caller.

---

## Acceptance Examples

- AE1. **Covers R7, R9.** Given a match is recording at 1080p60, when record is running, then both per-camera training proxies are also recording (proxy is not skipped at high resolution).
- AE2. **Covers R3.** Given the VIC spike does not sustain 1080p60 x264 on-device, when the app queries supported modes, then 1080p60 is absent and the default is 1080p30.
- AE3. **Covers R1.** Given a match ends, when the operator stops recording, then the final MP4 is immediately available with no transcode/"saving" step.
- AE4. **Covers R11.** Given either camera's output, when a neutral/white subject is captured, then it renders neutral (no magenta cast) on both cameras.
- AE5. **Covers R12.** Given autofocus is enabled, when the scene defocuses (subject moves or distance changes), then the VCM sweeps and settles at the sharpness peak; when autofocus is off, the app can set and hold an explicit focus position.

---

## Success Criteria

- Matches record reliably at the new default with measurable CPU headroom left for AI inference to run.
- Training footage exists for every match (both cameras), small enough to retain under the rotation policy.
- Both cameras render natural colour; the operator can set focus from the app.
- `ce-plan` can break this into units without inventing record-tier policy, proxy codec/resolution, or the colour/focus approach — the VIC experiment is defined as the gate on the 1080p60 default.

---

## Scope Boundaries

- MJPEG recording, offline MJPEG→H.264 transcode, reserved disk partition, and the app "saving video" modal — explored and dropped (transcode cost is minutes-to-longer-than-the-match even at ultrafast).
- 2K/4K recording on the Orin Nano — impossible without NVENC; a future hardware decision (Orin NX/AGX), not firmware work this cycle.
- On-demand preview rework — already built (encode is lazy per-viewer, torn down when the app looks away; one composited frame in both-cams mode). No work beyond the R13 doc fix.
- Building the AI/tracking model itself — this cycle frees budget for it.
- H.265/HEVC (slower in software, no HW encode).
- Phase-detect autofocus — no hardware for it; AF is contrast-detection only.
- Per-sensor `nvcam_cache` / cam0-vs-cam1 differential tuning as the colour theory — the cast is uniform across both cameras, so the fix is module-wide, not per-sensor (see R11).

---

## Key Decisions

- **Keep H.264, drop MJPEG-record:** MJPEG-live + offline transcode was the tempting path to 4K/60 at ~0 live CPU, but the post-match transcode runs minutes-to-longer-than-the-match even at ultrafast on 6 cores — the blocking "saving" UX kills it. Realtime x264 gives an instant, streamable MP4.
- **VIC offload is the keystone:** NVIDIA's own recommended Orin Nano pipeline is `nvvidconv → I420 → x264enc` and benchmarks ~96 fps at 1080p; the team's 0.64× ceiling was measured *without* VIC (scale/convert in software). VIC may unlock the 1080p60 default — hence R2 gates R3.
- **H.264 over H.265:** software x265 is ~2–3× slower and there is no HW HEVC encode; HEVC would make the ceiling worse.
- **Proxy = tiny always-on H.264, not MJPEG, not gated:** small files (disk-friendly), always-on preserves training coverage on every match; proxy on CPU x264 and the main record on x264 share the CPU but the proxy is cheap (~480p@15fps ×2).
- **4K is a hardware decision:** the Orin Nano is the only Orin without NVENC; 4K/60 realtime requires an Orin NX/AGX. Roadmap-only.
- **Preview untouched:** the on-demand encode behaviour the operator wanted already exists.

---

## Dependencies / Assumptions

- No NVENC on the Orin Nano (confirmed on-device and in NVIDIA's "Software Encode in Orin Nano" doc). Software `x264enc` is the only H.264 encode path.
- VIC re-add requires the NVMM→sysmem copy (`nvvidconv` outputs NVMM; `x264enc` reads system memory) — the pipeline dropped `nvvidconv` originally for exactly this reason; re-add it just for the scale with a final copy.
- Motorized focus (R12) requires the motorized-focus IMX477 lens variant and a working I2C/VCM driver path — hardware presence is **unverified**.
- Validation is on-metal: the record/encode and camera-quality changes must be confirmed on a real Jetson + phone, not simulated.

---

## Outstanding Questions

### Deferred to Planning

- [Affects R2, R3][Needs research] Does VIC offload get sustained 1080p60 x264 on-device? This spike gates whether 1080p60 becomes the advertised default.
- [Affects R2, R6][Needs research] Measure the CPU headroom under a live streamed match (RTMP + preview + 2 proxies + record with VIC) — is there room left for AI inference, or must the proxy fall back to gating at high resolution?
- [Affects R11][Needs research] Source or calibrate a `.nito` ISP tuning for the `jakku_*_RBPCV3` module (ArduCAM has no JP7.2 tuning; NVIDIA reference-imx477 didn't fix it), and inspect the IR-cut filter, before falling back to a pipeline `videobalance` correction. Firmware NV12→BGR convert is already ruled out on-device.
- [Affects R12][Needs research] Verify the motorized-focus lens hardware + I2C/VCM path (get bus/addr/protocol from ArduCAM's SDK/GitHub — their focus doc 403s to fetch), and tune the AF loop (sharpness threshold that triggers a hunt, sweep step, hold hysteresis) on-device.
- [Affects R10][Technical] Choose the concrete proxy rotation policy (max age vs max total size vs per-match cap) during planning.
