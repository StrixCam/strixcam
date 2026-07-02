---
title: "feat: Capture & transfer pipeline — encode budget, training proxy, color & AF"
type: feat
status: active
date: 2026-07-02
origin: docs/brainstorms/2026-07-01-capture-transfer-pipeline-requirements.md
---

# feat: Capture & Transfer Pipeline — Encode Budget, Training Proxy, Color & AF

## Summary

Free CPU-encode budget on the Orin Nano so AI can run next: offload encode-side scale/color-convert to VIC hardware and measure whether 1080p60 H.264 sustains under real match load, wire the (already-plumbed) raw-capture path into a tiny always-on H.264 training proxy driven firmware-side by the record lifecycle, boot headless, and land two camera-quality fixes (ISP-level pink cast via `.nito`, continuous autofocus via an I2C VCM loop). Keep H.264 everywhere; no MJPEG, no 4K on this silicon.

---

## Problem Frame

The device is near its CPU ceiling with record + preview + stream + control + overlays, and the most expensive job (AI detection) has not started — every encode is software `x264enc` on one shared CPU because the Orin Nano has no NVENC. See origin for the full pain narrative (Sources & References). This plan is the HOW: the binding constraint is *concurrent encode count on one CPU*, so sequencing is spike-first — the VIC combined-load measurement gates the advertised record default before any mode-table change lands.

---

## Requirements

- R1. Recording stays H.264 (`x264enc`) end-to-end — no MJPEG/transcode path. `kSupportedVideoModes` remains the single source of truth. (origin R1)
- R2. Move encode-side `videoscale` + color-convert onto VIC (`nvvidconv`), managing the NVMM→sysmem hop into `x264enc`; measure freed CPU. (origin R2)
- R3. Target 1080p60 as default, contingent on a VIC combined-load spike; else default stays 1080p30 and 1080p60 stays unadvertised. (origin R3)
- R4. No 2K/4K recording on this hardware. (origin R4)
- R5. Reversible headless boot; must not block the BLE control surface at bring-up. (origin R5)
- R6. Keep GPU (CUDA/TensorRT) reserved for AI inference; do not offload video encode to it (no NVENC exists). This cycle frees CPU budget for AI to run next; building the model itself is out of scope. (origin R6 — deferred / non-goal this cycle)
- R7. Wire the dead raw-capture trigger so the match record lifecycle drives the proxy start/stop. (origin R7)
- R8. Proxy = tiny always-on H.264 ~480p@15fps, per-camera (both). (origin R8)
- R9. Proxy runs on every match regardless of main record resolution. (origin R9)
- R10. Bounded retention/cleanup for proxy files. (origin R10)
- R11. Fix the both-cameras pink cast as an ISP/capture-level defect (`.nito` + IR-cut), not a firmware convert bug. (origin R11)
- R12. Continuous autofocus: I2C VCM driver + Laplacian sharpness + sweep/peak/hold, `CameraFocus{kAuto,kManual}` wired, manual override. (origin R12)
- R13. Correct the stale `CLAUDE.md` RTSP-caller claim. (origin R13)

**Origin actors:** A1 Operator, A2 App (`sst-cam-app`), A3 Firmware.
**Origin flows:** F1 (match record + training-proxy lifecycle).
**Origin acceptance examples:** AE1 (proxy runs at high-res — covers R7/R9), AE2 (VIC fails → default 1080p30 — R3), AE3 (instant MP4, no transcode — R1), AE4 (neutral color both cams — R11), AE5 (AF settles at peak / manual holds — R12; refined: hunts occur between plays, not mid-record).

> R6 (GPU reserved for AI inference) is a constraint, not build work this cycle — carried as a non-goal below.

---

## Scope Boundaries

- No MJPEG recording, offline transcode, disk partition, or "saving" modal (origin dropped these).
- No 2K/4K recording — needs NVENC hardware (Orin NX/AGX).
- No AI/tracking model build — this cycle frees budget for it (R6).
- No H.265/HEVC; no phase-detect AF; no runtime per-frame VIC auto-probe (static mode table committed from the spike).
- On-demand preview is already built — no rework beyond the R13 doc fix.

### Deferred to Follow-Up Work

- AI inference integration (TensorRT model + `IDecision`): separate cycle; this plan only preserves CPU headroom for it.
- 4K/high-fps capture: future hardware decision (Orin NX/AGX with NVENC).
- AF phase-detect / advanced hunt heuristics: after the contrast-detect loop ships.

---

## Context & Research

### Relevant Code and Patterns

- **Encode chains (VIC target):** `src/adapters/storage/gstreamer/recorder-launch.cpp` (`BuildRecorderLaunch`, conditional per-branch `videoscale ! videorate`, `x264enc` + `leaky=downstream` queue), `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp` (unconditional `videoconvert ! videoscale ! videorate`), `src/adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.cpp` (fixed-BGR appsrc, no scale). Launch strings are pure builder functions unit-tested without hardware (`tests/storage/recorder_launch.test.cpp`, `tests/streaming/rtmp_launch.test.cpp`).
- **Capture:** `src/adapters/capture/frame/gstreamer/gstreamer.cpp` — `nvarguscamerasrc ! ...NV12 ! nvvidconv ! appsink`; capture already uses `nvvidconv` (NVMM→sys). appsink capped at 5 buffers (drop).
- **Raw-capture path (LIVE plumbing, dead trigger):** proto `RawCaptureControlCommand` (`proto/bluetooth.proto:450`); `src/app/control/services/handlers/raw-capture.handler.cpp`; port `src/app/storage/ports/raw-capture-sink.hpp` (non-blocking contract); impl `src/adapters/storage/raw_capture/filesystem-raw-capture-sink.cpp` (NV12 `ofstream`, per-camera bounded ring + writer thread); fan-out tap already at `src/app/pipeline/services/orchestrator/pipeline-orchestrator.cpp:143-145` (post-`MaterializeFrame`); wired `src/main.cpp:265-266,360`. App controller `sst-cam-app/lib/features/camera/raw_capture_state.dart` (mints UUID, expects exactly 2 files) — **no caller**. Record START/STOP: `sst-cam-app/lib/features/match/session/session_screen.dart:362,420` + `session_state.dart`.
- **Recording handler / phase gate:** `src/app/control/services/handlers/recording.handler.cpp` (gates START on `SessionPhase::kReady`, writes MP4 to `video_output_path`, no group id).
- **Orchestrator / postprocess:** `pipeline-orchestrator.cpp` (`ProducerLoop`/`ConsumerLoop`, `LatestOnlySlot`, `MaterializeFrame`); `src/adapters/processing/opencv/opencv-postprocessor.cpp` (`cvtColor` NV12→BGR, crop, resize — CPU color+scale cost).
- **Preprocess Y-plane (AF source):** `src/adapters/processing/opencv/opencv-preprocessor.cpp:64-72` — `cv::Mat y_full(h, w, CV_8UC1, planes[0].data, stride)`, no color convert. AF Laplacian samples the `source_frame` Y-plane directly (independent of AI ColorMode, which may not run this cycle).
- **Mode table / camera config:** `src/domain/common/models/video-quality.hpp` (`kSupportedVideoModes` = {1080p30, 720p60, 720p30}); `src/domain/capture/models/camera-config.hpp` (`CameraFocus{kAuto,kManual}` + `focus` field already exist, compiled defaults, undriven).
- **Handler registration:** `src/main.cpp` dispatcher (`dispatcher.Register(...)`), `ICommandHandler` (`src/app/control/ports/handler.hpp`); reference `src/app/control/services/handlers/preview-layout.handler.cpp` (shared mutable state + oneof accessor + response). Session disconnect cleanup: `src/app/session/services/session_cleanup/session-cleanup.cpp`.
- **Config (retention model):** `src/domain/config/models/storage.hpp`, serde `src/adapters/config/json/serde/storage.hpp`, loader `src/app/config/services/config_loader/config-loader.cpp` (`EnsureDefault`), existing policy consumer `FilesystemDiskGuard` (`src/main.cpp:148`).
- **Deploy/boot:** `deploy/install.sh` (`embedded_unit()` heredoc → `multi-user.target`, `ensure_setup()`); `deploy/sst-cam-firmware.service`.
- **Proto:** no focus/CameraConfig message exists; free non-reserved oneof slots (44-49). Adding focus + `capture_group_id` on the record command = additive `feat:` across the `proto/` submodule + firmware + app.

### Institutional Learnings

- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — 1080p60 x264 ≈0.64× realtime; proxy is an ADDITIONAL concurrent encode; pin `format=I420` + `leaky=downstream` queue before every `x264enc`; VIC frees CPU but does not raise the encode ceiling; the proxy resolution is a dedicated internal constant (`kProxyVideoMode`) kept OUT of `kSupportedVideoModes` — that set is app-controllable modes only (per the existing raw-dual-recording precedent).
- `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md` — the proxy sink shares the fan-out thread; use `std::try_to_lock` in `Push` (drop on contention), hold the mutex only for an O(1) swap in `Stop`, drain/join outside the lock. A bare `lock_guard` across finalize repeats the 10s live-stream freeze.
- `docs/solutions/tooling-decisions/imx477-magenta-cast-is-isp-tuning-not-saturation-2026-06-30.md` — fix in `.nito`/`nvcam_cache` (+ IR-cut), NOT `saturation`/`wbmode`/convert; characterize each sensor independently (Argus exclusive — stop the service first); false-positive trap: `saturation=0.85` mutes the bad sensor and reads as fixed on one camera.
- `docs/solutions/logic-errors/proto-contract-logic-alignment-2026-06-09.md` — gate new non-`optional` scalars through `has_*()` at the single proto→domain mapper; no empty-OK skeleton handlers; `system_clock` epoch-ms for wire timestamps, `steady_clock` for durations.
- `sst-cam-app/docs/solutions/architecture-patterns/mock-must-mirror-real-firmware-contract-2026-06-10.md` — every new proto field changes THREE sites: firmware producer, app consumer, `MockBleService`; prefer full-flow assertions over mock flag checks.
- `docs/solutions/architecture-patterns/bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md` — boot-time apply runs off-thread so BLE/WiFi-Direct comes up regardless; headless + proxy startup must not block the BLE surface.

### External References

- NVIDIA "Software Encode in Orin Nano" (JetPack docs): confirms no NVENC, recommends `nvvidconv → video/x-raw,format=I420 → x264enc`; ultrafast ≈96 fps @1080p on 6 cores — the benchmark motivating R2.

---

## Key Technical Decisions

- **Spike-first sequencing:** VIC offload + a *combined-load* 1080p60 measurement (record + RTMP + preview + 2 proxies, headless) gates the mode table. Setting the default on an isolated 1080p60-record benchmark would ship an unsustainable default (flow analysis Q5).
- **Firmware-side record↔proxy coupling (user decision):** `RecordingHandler` START/STOP drives the sink; the app passes a minted `capture_group_id` on the record command (new optional proto field); firmware stops the proxy on BLE disconnect; the standalone `kRawCapture` trigger is retired. Atomic lifecycle, one phase gate, no orphaned files.
- **Proxy-start failure is non-fatal:** the match record proceeds with a warning; training coverage is best-effort (flow analysis Q3).
- **Proxy runs on record OR stream, ref-counted:** stops only when both are off (flow analysis Q4; satisfies R9 "every match" incl. streaming-only). The **BLE-disconnect force-stop resets the ref-count to zero atomically** with the forced `Stop` — otherwise a reconnect's record START increments from a stale non-zero count, never hits the 0→1 transition, and the proxy stays silently dead for the rest of the session.
- **Group via filename/naming, not wire metadata** so F1's "same id" holds across the MP4 + 2 proxies without breaking the proto contract. `RecordingMetadata` pins a joint invariant — for a final (`is_raw=false`) recording, `capture_group_id` MUST be absent, and the app/`MockBleService` raw-vs-final branching keys off exactly that. So the MP4 carries the group id in its **filename/on-disk naming**, not in `RecordingMetadata`; the invariant is left intact (flow analysis Q8; feasibility review).
- **Static mode table (not runtime VIC-probe):** commit the spike-proven value into `kSupportedVideoModes`; simpler, YAGNI (flow analysis Q on runtime vs static).
- **AF samples `source_frame` Y-plane directly:** no dependency on AI ColorMode (which may not run this cycle). AF re-hunts are suppressed while recording/streaming (user decision). **Be honest about the consequence:** with F1's continuous per-match record, record is ON for nearly the whole match, so AF effectively corrects only at match setup / between sessions — it does **not** self-correct continuously-recorded footage mid-match. R12's value is "focus is right when the match starts," not "focus tracks during play." Rejected alternatives to revisit if this proves too weak: a bounded/rate-limited nudge during record, or a one-shot refocus at record START. This refines origin AE5.
- **Color fix: `.nito`/IR-cut is the real fix; `videobalance` is a cosmetic stopgap, not a guaranteed floor.** The cast is in the raw sensor output before any convert, so a `videobalance` hue/sat correction only *masks* it — the exact "saturation false-positive trap" the learning warns about — and if the cause is IR-cut contamination, no software convert removes it cleanly. So **IR-cut inspection gates** relying on the fallback. If `.nito` can't be sourced AND the cause is IR, there is no adequate software fix — that remains a real open risk, not a mitigated one. Bind any tuning to the sensor mode R3 selects.
- **Keep H.264 / drop MJPEG-record:** offline transcode cost is minutes-to-longer-than-the-match even at ultrafast (origin Key Decisions).

---

## Open Questions

### Resolved During Planning

- Is the raw-capture path dead? — No; firmware plumbing is live (tap at `pipeline-orchestrator.cpp:143`). Only the trigger is missing, and the sink writes NV12 not H.264.
- Coupling location? — Firmware-side (user decision).
- AF during a live match? — Suppress re-hunts while recording (user decision).
- AF sharpness source? — `source_frame` Y-plane directly.

### Deferred to Implementation

- Exact VIC element ordering / NVMM↔sysmem caps negotiation on JP7.2 — resolve on-metal at `gst_parse_launch` runtime (U2).
- The concrete sustained 1080p60 verdict — decided by the U2 spike; U3 encodes the result.
- ArduCAM VCM I2C bus/address/protocol — from ArduCAM SDK/GitHub during U8 (their focus doc 403s).
- `.nito` source/calibration feasibility — U10 research; `videobalance` fallback is the committed floor.
- Retention thresholds (max age vs total size vs per-match cap) — U7, tuned against real proxy file sizes.
- Does the capture-adapter color correction (`.nito` or `videobalance`) propagate to the AI RGB preprocess path, or only record/stream/proxy? Placing the fallback in `gstreamer.cpp` capture should cover it — assert on-metal.
- Is the U2 spike's "headroom for AI" adequate once the real TensorRT inference workload lands? It's a host-side-overhead estimate now; a 1080p60 default proven sustainable pre-AI could still be squeezed when AI arrives — re-validate in the AI cycle.

---

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

**VIC offload (U2) — encode-branch shape.** Today (software): `appsrc ! videoconvert ! videoscale ! videorate ! I420 ! leaky-queue ! x264enc`. Target (VIC): move scale+convert onto `nvvidconv` producing NVMM, then a single sysmem download into I420 before the (sysmem-consuming) `x264enc`, preserving the `leaky=downstream` queue:

```
appsrc ! nvvidconv (scale + NV12→I420, NVMM out)
       ! video/x-raw,format=I420 (sysmem download hop)
       ! queue leaky=downstream ! x264enc ...
```

**Proxy lifecycle (U4/U5) — firmware-side coupling.**

```
Record START (RecordingControlCommand + capture_group_id)
  → RecordingHandler: phase-gate → start MP4 recorder → sink_.Start(group_id)   [proxy encode, 2 cams]
Stream START (no record) → sink_.Start(group_id) too   [ref-count++]
Record STOP / Stream STOP → ref-count--; sink_.Stop() when both off
BLE disconnect (session cleanup) → force sink_.Stop() + recorder stop
Terminal: 1 MP4 (group-id in filename) + 2 proxy H.264 files (same group id)
```

**AF loop (U9) — suppressed during record.**

```
steady state: low-rate Laplacian sample on source_frame Y
  if sharpness < threshold AND not recording/streaming:
      sweep VCM over range, sample Laplacian, settle at peak (or timeout→hold last)
manual command: abort in-flight sweep, jump to set position, mode=kManual
VCM I2C write error / lens absent (boot detect): disable AF, report manual-only
```

---

## Implementation Units

- U1. **Headless boot**

**Goal:** Boot to `multi-user.target` (no desktop) to reclaim baseline CPU/RAM, reversibly, without delaying the BLE control surface.

**Requirements:** R5

**Dependencies:** None

**Files:**
- Modify: `deploy/install.sh` (`ensure_setup()` — add `systemctl set-default multi-user.target` + a documented revert path; confirm the `WantedBy=multi-user.target` unit still enables/starts)
- Modify: `deploy/README.md` (document the headless default + how to revert)

**Approach:**
- Add an idempotent set-default step and a revert note (`graphical.target`). Verify `nvargus-daemon` ordering (`After=`) still holds. No separate boot-config file exists; the embedded unit is authoritative.

**Patterns to follow:** `deploy/install.sh` `embedded_unit()` + `ensure_setup()`.

**Test scenarios:**
- Test expectation: none (deploy script change) — validated on-metal: after install, `systemctl get-default` = `multi-user.target`, the service comes up, and BLE control is reachable within the normal window. Revert restores `graphical.target`.

**Verification:** Fresh install boots headless; BLE control reachable; documented revert works.

---

- U2. **VIC/`nvvidconv` encode-side offload + combined-load 1080p60 spike**

**Goal:** Move encode-side scale/convert onto VIC and measure whether 1080p60 H.264 sustains under real concurrent match load headless.

**Requirements:** R2, R3

**Dependencies:** U1 (measure under the headless target the device actually runs)

**Files:**
- Modify: `src/adapters/storage/gstreamer/recorder-launch.cpp`, `.hpp`
- Modify: `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp`, `.hpp`
- Modify: `src/adapters/processing/opencv/opencv-postprocessor.cpp` (secondary, optional under R2's CPU-reclaim intent — postprocess is the shared chosen-frame stage, not an encode branch; evaluate VIC for its resize/convert and weigh per branch, only if the spike shows it's worth the added NVMM hop)
- Test: `tests/storage/recorder_launch.test.cpp`, `tests/streaming/rtmp_launch.test.cpp`

**Approach:**
- Insert `nvvidconv` for scale + NV12→I420, add the single NVMM→sysmem download hop before `x264enc`, keep the `leaky=downstream` queue. Fallback to the current software `videoscale`/`videoconvert` if the VIC caps fail to negotiate on-device (element resolution is runtime). VIC frees CPU; it does not raise the encode ceiling.
- **Spike:** measure sustained throughput (wall-time-vs-duration) for 1080p60 record **concurrent with** RTMP + RTSP preview + **2 synthesized 480p@15 x264 proxy pipelines** — the real proxy sink doesn't exist until U4, so stand up two throwaway encoders fed from the same capture tap to emulate the load — headless, leaving headroom for AI. **The verdict is invalid if measured without the two proxy encodes present**: omitting them under-measures CPU and would ship an over-optimistic default (the proxy is an additional concurrent encode; VIC does not raise the encode ceiling). Record the verdict for U3.

**Execution note:** On-metal measurement gates U3 — run the combined load, not an isolated record.

**Patterns to follow:** existing builder-function structure + `leaky=downstream` queue; NVIDIA `nvvidconv → I420 → x264enc` reference.

**Test scenarios:**
- Happy path: `BuildRecorderLaunch` emits an `nvvidconv`-based chain with the I420 sysmem hop and the leaky queue preserved, for each supported mode.
- Edge case: quality unset → default caps path still valid.
- Happy path: `BuildRtmpLaunch` emits the VIC chain + retains the audio branch for flvmux.
- Integration (on-metal, documented, expected-fail in container): pipeline negotiates and encodes at 1080p30 and (if the spike passes) 1080p60 under combined load; NVMM→sysmem hop links.

**Verification:** Builder tests green; on-metal combined-load measurement produces a clear sustained/not-sustained verdict for 1080p60.

---

- U3. **Advertise the spike-proven record ladder**

**Goal:** Make `kSupportedVideoModes` and the default reflect U2's verdict.

**Requirements:** R1, R3, R4

**Dependencies:** U2

**Files:**
- Modify: `src/domain/common/models/video-quality.hpp` (`kSupportedVideoModes`, default)
- Modify: tests asserting supported modes / `IsSupportedMode`
- Add: a dedicated `kProxyVideoMode` (`~480p@15`) constant used by the proxy builder, **kept OUT of `kSupportedVideoModes`** so `DeviceInfoResponse.supported_modes` and app record/stream validation stay app-controllable-only (mirrors the raw-dual-recording precedent) — see U4

**Approach:**
- If the combined-load gate passed, add 1080p60 and set it default; else keep 1080p30 default and leave 1080p60 unadvertised. Static commit — no runtime probe. No 2K/4K entries.

**Patterns to follow:** existing `constexpr` array + `IsSupportedMode` helper.

**Test scenarios:**
- Covers AE2. Happy path: `IsSupportedMode(1080p60)` reflects the committed verdict; the default is a member of `kSupportedVideoModes`.
- Edge case: an unsupported/rejected mode (e.g. 4K) is refused by mode validation.
- Edge case: the proxy mode (480p15) validates as an allowed encode mode.

**Verification:** Mode table matches the spike verdict; default is always an advertised, sustainable mode.

---

- U4. **Proxy encode sink (NV12 `ofstream` → H.264 480p@15fps per camera)**

**Goal:** Turn the raw sink into a tiny always-on per-camera H.264 encode sink, non-blocking on the shared fan-out thread.

**Requirements:** R8

**Dependencies:** U3 (proxy mode advertised); benefits from U2 freed budget

**Files:**
- Modify: `src/adapters/storage/raw_capture/filesystem-raw-capture-sink.cpp`, `.hpp` (per-camera GStreamer H.264 encoder instead of raw `ofstream`; mirror `GstContinuousRecorder`)
- Create: `src/adapters/storage/raw_capture/proxy-launch.{hpp,cpp}` (pure builder: `appsrc ! (nvvidconv scale to 480p) ! I420 ! leaky-queue ! x264enc ! h264parse ! mp4mux ! filesink`)
- Test: `tests/storage/proxy_launch.test.cpp`, and update `tests/storage/raw_capture_sink.test.cpp`

**Approach:**
- Downscale to ~480p and drop to ~15fps (`videorate`), via VIC where possible. `leaky=downstream` queue + `format=I420` before `x264enc`. Per-camera encoder + writer, bounded ring, drop-oldest. `PushCamera` uses `std::unique_lock`+`try_to_lock` (drop on contention); `Stop` holds the lock only for an O(1) state swap, drains/joins/finalizes outside the lock.

**Execution note:** Characterize the non-blocking `Push`/`Stop` contract first (this sink shares the live-stream fan-out thread).

**Patterns to follow:** `recorder-launch.cpp` builder; `GstContinuousRecorder`; non-blocking-sink learning doc.

**Test scenarios:**
- Happy path: builder emits an 480p@15 H.264 chain with the leaky queue + I420 pin, per camera.
- Happy path: `Start(group)` → `PushCamera` frames → `Stop` yields 2 playable H.264 files (valid moov) named per `RawCaptureIdentity`.
- Edge case: `Start` while already capturing → rejected.
- Error path: contended `PushCamera` drops the frame, never blocks (assert no `lock_guard` across finalize).
- Error path: disk-full mid-write logs once and truncates cleanly; other camera unaffected.
- Integration: `Stop` does not stall a concurrent record/stream sink on the same thread.

**Verification:** Two small playable per-camera H.264 files per session; `Stop` never freezes the fan-out thread.

---

- U5. **Firmware-side record↔proxy coupling + `capture_group_id` proto**

**Goal:** Drive the proxy from the record lifecycle atomically; group the MP4 and proxies; stop on disconnect.

**Requirements:** R7, R9

**Dependencies:** U4; proto submodule change

**Files:**
- Modify: `proto/bluetooth.proto` (add optional `capture_group_id` to `RecordingControlCommand`; regenerate bindings)
- Modify: `src/app/control/services/handlers/recording.handler.cpp` (on START: phase-gate → recorder → `sink_.Start(group_id)`; on STOP: ref-count → `sink_.Stop()` when record+stream both off; name the MP4 with the group id — do NOT put it in `RecordingMetadata`, which forbids `capture_group_id` on `is_raw=false` records)
- Modify: streaming start/stop path to participate in the proxy ref-count (`src/app/streaming/...` + orchestrator wiring)
- Modify: `src/app/session/services/session_cleanup/session-cleanup.cpp` (BLE disconnect → force recorder + `sink_.Stop()`)
- Modify: `src/main.cpp` (wire the sink into `RecordingHandler`; retire the standalone `kRawCapture` trigger while keeping the sink)
- Modify: proto→domain mapper (gate `capture_group_id` via `has_*()`, documented default)
- Test: `tests/control/recording_handler.test.cpp`, `tests/app/proxy_lifecycle.test.cpp`

**Approach:**
- One record command carries the app-minted group id. Ref-count proxy across record+stream; the BLE-disconnect force-stop resets the ref-count to zero atomically so a reconnect's START re-triggers the 0→1 Start. On START, if the group-id path already has files on disk, reject/rename rather than opening the filesink in truncate mode (guards the flagged group-id collision/overwrite of a complete-but-undownloaded group). Proxy-start failure is non-fatal (record proceeds, warn). No skeleton/empty-OK responses.

**Test scenarios:**
- Covers AE1. Happy path: record START at 1080p60 → both proxies start; STOP → both stop; MP4 + 2 proxies share the group id via filename (MP4 `RecordingMetadata` still reports `is_raw=false` with no `capture_group_id` — invariant intact).
- Happy path: stream-only START starts the proxy; stream STOP with no record stops it (ref-count to zero).
- Edge case: record STOP while stream still active → proxy keeps running.
- Error path: `sink_.Start` fails → record still starts, response warns, no rollback.
- Error path: STOP racing a slow START → no deadlock, terminal state consistent.
- Integration: BLE disconnect mid-match → firmware stops recorder + proxy (no orphaned open files).
- Edge case: START rejected when `SessionPhase != kReady` (proxy not left half-started).
- Integration: BLE disconnect while ref-count=2 (record+stream) → reconnect → record START restarts both proxies (ref-count reset landed, not stuck non-zero).
- Error path: record START with an already-present group id on disk → rejected/renamed; the prior group's files are not truncated.
- Error path: forced stop on disconnect finalizes the main MP4 `moov` cleanly (playable up to the disconnect), not just "no orphaned files".

**Verification:** Match record deterministically produces a group-id-stamped MP4 + 2 proxies; disconnect cleans up; partial failures behave per decisions.

---

- U6. **App wiring + mock mirror**

**Goal:** App passes the minted group id on the record command; retire the standalone raw-capture trigger; keep list/download working.

**Requirements:** R7

**Dependencies:** U5 (proto)

**Files:**
- Modify: `sst-cam-app/lib/features/match/session/session_state.dart` (mint UUID, attach `capture_group_id` to the record START command)
- Modify/retire: `sst-cam-app/lib/features/camera/raw_capture_state.dart` (remove the now-unused standalone trigger; keep any list/download helpers still used)
- Modify: `sst-cam-app/lib/mock/emulator/` `MockBleService` (mirror `capture_group_id` on the record command exactly as firmware consumes it)
- Modify: `sst-cam-app/main_page.dart` (remove the "#6 wires it" stale comment)
- Test: app widget/service tests covering record→proxy-files→download; mock-mirror test

**Approach:**
- Full-flow assertions (start → list → download 2 proxies) over mock side-effect flags. Field isn't done until the mock produces it as firmware does.

**Test scenarios:**
- Covers AE1. Happy path: starting a match sends the record command with a `capture_group_id`; after stop, 2 proxy files are listable/downloadable under that id.
- Edge case: BLE disconnect mid-match resets app state (`onInterrupted`) without leaving a dangling group.
- Integration: `MockBleService` returns the same `capture_group_id`/metadata shape the real firmware does (no false-green).

**Verification:** App drives the proxy purely via the record command; mock mirrors the real contract.

---

- U7. **Proxy retention/rotation**

**Goal:** Bound proxy accumulation without breaking the app's completion/download contract.

**Requirements:** R10

**Dependencies:** U4, U5

**Files:**
- Modify: `src/domain/config/models/storage.hpp` (retention fields), `src/adapters/config/json/serde/storage.hpp`, `src/app/config/services/config_loader/config-defaults.hpp`
- Create: `src/app/storage/services/proxy_retention/` (rotation policy adapter) + port if needed
- Modify: `src/main.cpp` (construct + wire the policy, like `FilesystemDiskGuard`)
- Test: `tests/storage/proxy_retention.test.cpp`

**Approach:**
- Delete oldest complete-and-downloaded groups first; **exclude the active capturing group and any group with a live download token**. **When the cap is reached and every group is complete-but-never-downloaded**, delete oldest regardless of download state with a warning (bounded storage wins over retaining un-fetched training data) rather than blocking new capture — so R10's bound always holds even for an operator who never downloads. Model on `FilesystemDiskGuard`.

**Test scenarios:**
- Happy path: over the cap → oldest complete+downloaded group deleted.
- Edge case: the active/capturing group is never deleted.
- Edge case: a group with an open download token is skipped.
- Error path: a partial (1-file / 0-byte) group is handled per the partial-group policy, not silently counted as complete.
- Edge case: over cap with nothing downloaded → oldest deleted regardless (with warning); the storage bound holds and new capture is not blocked.

**Verification:** Storage stays bounded; the app's exactly-2-files completion invariant never breaks from rotation.

---

- U8. **VCM I2C driver + focus proto/handler + manual control**

**Goal:** Drive the motorized lens over I2C, detect hardware presence, and expose manual focus + auto/manual mode.

**Requirements:** R12

**Dependencies:** proto submodule change; hardware presence is a precondition to AF (U9)

**Files:**
- Create: `src/adapters/capture/focus/i2c-vcm-focus.{hpp,cpp}` (I2C VCM adapter) + port `src/app/capture/ports/focus-controller.hpp`
- Modify: `proto/bluetooth.proto` (new `CameraFocusControlCommand`: mode `{auto,manual}` + optional position; new oneof field in a free slot; regenerate)
- Create: `src/app/control/services/handlers/focus.handler.{hpp,cpp}` (mirror `preview-layout.handler`)
- Modify: `src/domain/capture/models/camera-config.hpp` (drive the existing `CameraFocus{kAuto,kManual}`/`focus`), `src/main.cpp` (register handler + shared focus state)
- Modify (app): focus control UI + BLE command + `MockBleService` mirror
- Test: `tests/capture/i2c_vcm_focus.test.cpp`, `tests/control/focus_handler.test.cpp`

**Approach:**
- Boot-time presence detection: if the VCM I2C probe fails, disable AF and report manual-only (no failing loop). Manual command sets an explicit position and aborts any in-flight hunt. Bus/addr/protocol from ArduCAM SDK.

**Test scenarios:**
- Happy path: manual command sets a position; auto/manual mode toggles `CameraFocus`.
- Error path: I2C write failure / absent lens → AF disabled, reported manual-only, no spin.
- Integration: manual command mid-hunt preempts and jumps to position (with U9).
- Edge case: proto `has_*()` gating — mode without position vs position without mode both handled.

**Verification:** App can set focus and toggle mode; missing hardware degrades cleanly.

---

- U9. **Continuous AF loop (contrast-detect, record-aware)**

**Goal:** Keep focus sharp between plays without blurring recorded footage.

**Requirements:** R12

**Dependencies:** U8 (VCM driver + presence)

**Files:**
- Create: `src/app/capture/services/autofocus/` (sharpness metric + sweep/peak/hold loop)
- Modify: preprocess/orchestrator wiring to expose the `source_frame` Y-plane to the AF service (`src/adapters/processing/opencv/opencv-preprocessor.cpp`, `pipeline-orchestrator.cpp`)
- Modify: `src/main.cpp` (construct + wire AF; share record/stream state so AF can suppress)
- Test: `tests/capture/autofocus.test.cpp`

**Approach:**
- Low-rate Laplacian-variance sample on the `source_frame` Y-plane (`planes[0]`); trigger a hunt when sharpness drops below threshold **and not recording/streaming**; sweep VCM, settle at peak, or timeout→hold last position on a flat/low-light curve. Per-camera loops. Steady-state sampling cadence kept cheap (budget-aware).

**Test scenarios:**
- Covers AE5. Happy path: sharpness drop while idle → sweep → settle at the Laplacian peak.
- Edge case: recording/streaming active → no re-hunt (footage stays clean); hold last position.
- Edge case: flat/low-light curve → timeout, hold last position, no VCM thrash.
- Edge case: two cameras hunt independently without coupling flicker.
- Error path: manual override during a hunt aborts and jumps (with U8).

**Verification:** Sharp focus between plays; no visible sweep during a live match; bounded steady-state CPU.

---

- U10. **ISP pink-cast fix (both cameras)**

**Goal:** Neutral color on both cameras, fixed at ISP/capture level with a shippable fallback.

**Requirements:** R11

**Dependencies:** R3 sensor mode (U3) — bind tuning to the mode actually used

**Files:**
- Create/modify: `deploy/install.sh` (ship the `.nito`/ISP tuning to `/var/nvidia/nvcam/settings/`; document IR-cut inspection)
- Add: tuning asset under `deploy/` (module-matched `.nito` when sourced)
- Modify (fallback): `src/adapters/capture/frame/gstreamer/gstreamer.cpp` (optional `videobalance` correction stage, config-gated) + `src/domain/capture/models/camera-config.hpp` (correction params)
- Test: `tests/capture/color_correction.test.cpp` (fallback params applied); on-metal color validation (documented)

**Approach:**
- Research-gated, in order: (a) source/calibrate a module-matched `.nito` for `jakku_*_RBPCV3`; (b) **inspect the IR-cut filter — this gates the software fallback** (an IR-contamination cause is not fixable in `videobalance`); (c) only if `.nito` can't be sourced and the cause is not IR, ship the `videobalance` **cosmetic stopgap** (explicitly not a guaranteed fix). One module-wide tuning (cast is uniform — not per-sensor differential). Characterize each sensor independently with the service stopped; avoid the `saturation` false-positive trap (a mask can visually pass AE4 while leaving capture wrong).

**Execution note:** Validate on both cameras, not one.

**Test scenarios:**
- Covers AE4. On-metal (documented): a neutral subject renders neutral on **both** cameras after the fix.
- Happy path (unit): the `videobalance` fallback stage applies its config params to the capture caps string when enabled.
- Edge case: fallback disabled by default when a valid `.nito` is present.

**Verification:** Both cameras neutral on-metal; a color fix ships regardless of `.nito` availability.

---

- U11. **Doc fix**

**Goal:** Correct the stale `CLAUDE.md` RTSP-caller claim and adjacent drift.

**Requirements:** R13

**Dependencies:** None

**Files:**
- Modify: `CLAUDE.md` (remove "RTSP StartAppStream has no production caller yet" — `WifiDirectHandler::HandleStart` is a live caller; purge any lingering `nvv4l2h264enc`/NVENC references and phantom SQLite/`DbSeeder` claims if encountered)

**Approach:** Targeted edit; align with the existing no-NVENC / `kSupportedVideoModes` canonical note.

**Test scenarios:** Test expectation: none (docs) — reviewer confirms the claim matches `wifi-direct.handler.cpp`.

**Verification:** No false claims remain about RTSP callers or absent modules.

---

## System-Wide Impact

- **Interaction graph:** the proxy sink and (later) AF both touch the shared `ConsumerLoop`/`ProducerLoop` fan-out threads; VIC changes touch every encode branch. `RecordingHandler` now also owns proxy lifecycle.
- **Error propagation:** proxy-start failure is non-fatal to record; VIC negotiation failure falls back to software scale; VCM I2C failure degrades AF to manual — none abort the match.
- **State lifecycle risks:** proxy ref-count across record+stream; BLE-disconnect cleanup; retention vs open download tokens; group-id collisions (trunc-overwrite) — U5/U7 must guard.
- **API surface parity:** two additive proto changes (`capture_group_id` on the record command; new focus command) must land in the `proto/` submodule + firmware + app + `MockBleService` together.
- **Integration coverage:** on-metal only — VIC 1080p60 under combined load, headless-target camera bring-up, both-camera color, VCM hardware presence.
- **Unchanged invariants:** RTSP preview stays on-demand/lazy per-viewer (no rework); appsink 5-buffer cap and `MaterializeFrame` boundary unchanged (proxy taps the materialized frame, not the pinned appsink); no MJPEG/transcode path introduced.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| VIC doesn't lift x264 to sustained 1080p60 under combined load | Spike-first (U2) gates U3; default stays 1080p30 — no shipped regression |
| Proxy adds a concurrent encode that starves record/stream/AI | 480p@15 + VIC scale; measure concurrent load; if x264 headroom is gone, degrade the proxy **within H.264** (lower fps, single camera, or gate at high main-res) — **not** MJPEG/`nvjpegenc` (rejected in brainstorm; would violate the no-MJPEG boundary) |
| Proxy `Stop` runs a dual mp4mux `moov` finalize on the BLE/dispatcher thread → slow STOP response under combined load | Run the finalize/drain off the dispatcher thread (mirror the existing recorder `Stop`); the `try_to_lock` Push discipline (U4) already protects the fan-out thread; watch combined-load finalize latency on-metal |
| `videobalance` color fallback (U10) adds a CPU op on both always-on capture streams — counter to the CPU-reclaim thesis | Prefer the `.nito` fix; `videobalance` is a **cosmetic stopgap gated by IR-cut inspection** (an IR cause is not fixable in software), measure its cost on-metal, remove once a module-matched `.nito` ships |
| `.nito` unsourceable AND cause is IR-cut contamination → no adequate software color fix exists | Inspect IR-cut hardware first; if it's the cause, escalate as a hardware issue — do NOT ship a `videobalance` mask that passes a visual check while leaving capture wrong (AE4 could false-pass) |
| Proxy `Stop` freezes the shared fan-out thread (repeat of the 10s bug) | Enforce `try_to_lock` Push + drain-outside-lock (U4); grep every new sink for bare `lock_guard` |
| VIC / camera fails under `multi-user.target` (needs display context) | Validate U2 under U1 headless together on-metal before committing |
| `.nito` can't be sourced for the ArduCAM module | Committed `videobalance` fallback (U10) so a color fix ships regardless |
| Motorized-focus hardware absent/unverified | U8 boot presence-detect → degrade to manual-only; AF loop (U9) gated on presence |
| Proto drift across submodule/firmware/app/mock → false-green | Three-place field wiring + full-flow assertions (U5/U6/U8) |
| Retention deletes an active/downloading group | Exclude active group + open download tokens; oldest-complete-first (U7) |

---

## Documentation / Operational Notes

- Update `deploy/README.md` for headless default + revert (U1) and `.nito`/IR-cut install steps (U10).
- After U9 lands, capture the AF/VCM approach via `/ce-compound` — it's greenfield with no prior learning.
- Validation is on-metal (Jetson + phone) per project policy; container `ctest` covers builder/unit tests only, hardware-bound tests are expected-fail in the container.

---

## Phased Delivery

### Phase 1 — CPU reclaim & record ceiling
- U1 headless → U2 VIC + combined-load spike → U3 mode table. Spike gates the default.

### Phase 2 — Training proxy
- U4 encode sink → U5 firmware coupling + proto → U6 app wiring + mock → U7 retention.

### Phase 3 — Camera quality
- U8 VCM driver + focus proto/handler → U9 AF loop; U10 color `.nito` (research-gated, parallelizable).

### Phase 4 — Docs
- U11 doc fix (any time).

---

## Sources & References

- **Origin document:** [docs/brainstorms/2026-07-01-capture-transfer-pipeline-requirements.md](docs/brainstorms/2026-07-01-capture-transfer-pipeline-requirements.md)
- Encode ceiling: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`
- Non-blocking sink: `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md`
- Pink cast: `docs/solutions/tooling-decisions/imx477-magenta-cast-is-isp-tuning-not-saturation-2026-06-30.md`
- Proto contract: `docs/solutions/logic-errors/proto-contract-logic-alignment-2026-06-09.md`
- Mock mirror (app): `sst-cam-app/docs/solutions/architecture-patterns/mock-must-mirror-real-firmware-contract-2026-06-10.md`
- External: NVIDIA "Software Encode in Orin Nano" (no NVENC; `nvvidconv → I420 → x264enc`)
