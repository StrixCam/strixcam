# Handoff — Capture & Transfer Pipeline (training proxy + color + scale/size/transport)

**Status:** idea / pre-brainstorm. Start a fresh conversation with `ce-brainstorm` (then `ce-plan`) using this as the seed.
**Date:** 2026-07-02
**Primary repo:** `sst-cam-firmware` (GStreamer capture/encode). Small app-side piece in `sst-cam-app` (raw-capture lifecycle wiring).
**Predecessor:** the "make-it-real" batch is merged to `release/0.1.0` (U8 runtime record/stream quality, U9.2 telemetry, U12 app quality + streaming picker). This is the follow-on the user flagged: *"a complete plan to handle and improve the way we capture and transfer the frames (color, scale, size, and all that)."*

---

## Goal

Improve how the camera **captures, encodes, sizes, colors, and transfers** frames. Four workstreams, roughly in priority order. Lead item is the training proxy.

---

## Hardware reality (drives everything)

Jetson **Orin Nano** — confirmed on device:
- **No NVENC** (`nvv4l2h264enc`/`nvv4l2h265enc` absent, no `/dev/nvhost-msenc`). All H.264 encode is **software `x264enc` on the CPU**.
- **NVDEC present** (`nvv4l2decoder`) — hardware decode (not our bottleneck; the phone decodes).
- **VIC present** (`nvvidconv`) — hardware scale + colorspace convert.
- **HW JPEG present** (`nvjpegenc`) — hardware MJPEG (intra-only, large files).
- **Software x264 ceiling (measured):** 1080p60 ≈ **0.64× realtime** (can't sustain → unplayable file); **1080p30 and 720p60 sustain**. One CPU encoder is **shared** across record + RTSP preview + RTMP stream.
- Deep-dive: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`.

**Implication:** any new concurrent encode (e.g. 2× training-proxy) competes with record+preview+stream for the *same CPU*. Budget is the hard constraint of this whole plan.

---

## Workstream 1 (LEAD) — Compressed low-res dual-camera **training proxy**, tied to match lifecycle

**Want:** every match recording ALSO produces small, compressed, low-res footage of **both** cameras — for training a model, reviewing camera angles, and general dev. Lifecycle-tied: match record **start → proxy start**, match record **stop → proxy stop**.

**Current reality (important):**
- The raw dual-capture mechanism **exists end-to-end but is DEAD-WIRED** — nothing triggers it.
  - Firmware: `RawCaptureControlCommand` → `src/app/control/services/handlers/raw-capture.handler.cpp` → `FilesystemRawCaptureSink` (`src/adapters/storage/raw_capture/filesystem-raw-capture-sink.cpp`). Writes **uncompressed NV12**, one file per camera, `~186 MB/s` for both (~11 GB/min). Independent of match recording (`is_recording` untouched).
  - App: `lib/features/camera/raw_capture_state.dart` has `start()`/`stop()` that send the command — **but they have ZERO callers.** `main_page.dart:437` comment admits *"#6 wires it to the match record/stream lifecycle"* — never implemented.
  - **Zero raw files exist on the Jetson** (confirmed). It has never run.
- Save path when it runs: `/var/lib/sst/cam/videos/raw__<capture_group_id>__cam0.nv12` + `__cam1.nv12` (flat at the video root, `raw__` prefix; final-match MP4s live in per-match subdirs).

**Two parts:**
- **Part A (app, easy):** wire `rawCaptureProvider.start(<minted capture_group_id>)` alongside the match `RecordingControl` START (and `stop()` on STOP). Mint a UUID group id.
- **Part B (firmware, the hard/CPU-risk part):** change the raw sink from **uncompressed NV12** to a **small compressed low-res proxy**. Uncompressed is unusable for "small files." But compressing = **2 more software x264 encodes** on a board already near its CPU ceiling. Design options to weigh:
  - Very low res + fps (e.g. 480p/540p @ 10–15 fps) H.264.
  - **HW `nvjpegenc` MJPEG** (offloads encode to hardware — no CPU cost — but larger, intra-only; maybe fine at low res/fps for training).
  - Proxy **only when not streaming** (free up the stream's encode budget).
  - Downsample via **VIC (`nvvidconv`)** so the scaling itself is off-CPU (see Workstream 4).
  - **Retention/cleanup policy** — even small, 2 streams × every match adds up; need rotation / max-size.

**Open questions:** target res/fps/codec for training usefulness vs CPU/disk; is MJPEG acceptable for the model?; per-camera or side-by-side?; retention policy; does it run during streaming or defer?

---

## Workstream 2 — U10: IMX477 color / ISP magenta-pink cast

**Symptom:** the dual-IMX477 shows a pink/magenta cast; **per-sensor** — `cam1` corrects acceptably, **`cam0` keeps a magenta hue**.
**Root cause (diagnosed, documented):** it is **per-sensor ISP white-balance/hue bias**, NOT over-saturation.
- `wbmode` already defaults to `auto` (1); non-auto modes are worse. `saturation` only scales intensity — cannot correct a hue offset (desaturating cam0 just greys the magenta).
- `nvarguscamerasrc` exposes **no per-channel WB gains** (only `wbmode`, `awblock`, `saturation`, `gainrange`, `ispdigitalgainrange`).
- Per-sensor ISP tuning caches exist on device: `/var/nvidia/nvcam/settings/nvcam_cache_{0,1}.bin`, `imx477.nito`.
**Real fix (the research):** vendor **ISP tuning** (`.nito` / per-sensor `nvcam_cache`) or a **custom color-matrix / `videobalance`-style correction stage** in the pipeline — likely per-sensor. This is genuine research, was ruled NOT-a-quick-fix and deferred (a `saturation=0.85` attempt fixed cam1 but not cam0 and was NOT shipped).
**Refs:** `[[camera-color-isp-and-motorized-focus]]` (memory); the finding doc `imx477-magenta-cast-is-isp-tuning-not-saturation` (written on the U9.2 branch — verify it's on `release/0.1.0`, else it's in the app repo / needs porting). Also noted: **motorized focus** is an unbuilt firmware feature (adjacent camera-config work).
**Files:** `src/adapters/capture/frame/gstreamer/gstreamer.cpp` (nvarguscamerasrc caps string), `src/domain/capture/models/camera-config.hpp` (`white_balance`/`saturation` fields).

---

## Workstream 3 — Scale / size / transport (the broad "capture & transfer" cleanup)

The general ask: rationalize how frames are **captured → scaled → sized → transferred** across the pipeline. Today: capture 1080p → postprocess resizes to a fixed **1280×720** output (`main.cpp:80` `kOverlayWidth/Height`) → fan-out to record + stream + (raw taps capture directly). Independent record/stream res is done via per-branch `videoscale`/`videorate` (U8). Areas to examine:
- Where resizing happens and how many times (capture res vs postprocess output vs per-branch scale) — redundant scales cost CPU.
- Output/transport sizing for preview (RTSP) vs stream (RTMP) vs record vs proxy — one coherent size policy.
- Frame transport/materialization (`MaterializeFrame`, `LatestOnlySlot`, appsink 5-buffer cap) under the added proxy load.

---

## Workstream 4 — VIC offload optimization (enabler for the above)

Move `videoscale` + `videoconvert` (NV12↔I420/BGR) from **software** (CPU) onto **`nvvidconv` (VIC hardware)** so scaling/color-convert stops burning CPU that the software encoders need. **Won't unlock 1080p60** (the entropy encode still ceilings on CPU), but frees budget for 1080p30-under-load and the proxy encodes. Caveat: `nvvidconv` outputs NVMM memory; `x264enc` reads system memory — need the NVMM→sysmem hop (the pipeline dropped `nvvidconv` originally for exactly this reason; re-add just for the scale with a final copy). Weigh per branch.

---

## Key files

**Firmware:**
- `src/adapters/capture/frame/gstreamer/gstreamer.cpp` — capture (`nvarguscamerasrc`), color/WB caps.
- `src/adapters/storage/raw_capture/filesystem-raw-capture-sink.cpp` — raw sink (NV12 → change to proxy encode).
- `src/app/control/services/handlers/raw-capture.handler.cpp` — RawCaptureControl handler.
- `src/adapters/storage/gstreamer/recorder-launch.{hpp,cpp}`, `src/adapters/streaming/gst_rtmp/rtmp-launch.{hpp,cpp}` — encode chains (leaky queue + I420 pattern).
- `src/app/pipeline/services/orchestrator/pipeline-orchestrator.cpp` — fan-out (record/stream/raw taps), the shared ConsumerLoop thread.
- `src/domain/capture/models/camera-config.hpp`, `src/domain/common/models/video-quality.hpp` (`kSupportedVideoModes`).

**App:**
- `lib/features/camera/raw_capture_state.dart` (`rawCaptureProvider.start()/stop()` — currently uncalled).
- `lib/features/match/session/session_screen.dart` (the 4 record/stream START/STOP sites to hook the proxy onto).
- `main_page.dart:437` (the "#6 wires it" comment).

## Relevant learnings
- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — the CPU encode ceiling + advertise-sustainable-modes + leaky queue.
- `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md` — never block the shared fan-out thread; `try_to_lock` producer + drain-outside-lock (RecordingService example).
- IMX477 magenta-cast finding doc (locate: written 2026-06-30, may be on the U9.2 lineage — confirm it's reachable from `release/0.1.0`).

## Suggested kickoff
1. `ce-brainstorm` this doc — nail the training-proxy codec/res/fps/retention against the CPU budget first (it gates the rest), then U10 approach, then scale/transport + VIC offload.
2. `ce-plan` the agreed scope into units.
