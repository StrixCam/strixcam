---
title: WiFi-Direct radio reform kills Argus capture; the producer loop needs a restart watchdog
date: 2026-07-03
category: integration-issues
module: pipeline
problem_type: integration_issue
component: service_object
symptoms:
  - "Live preview is blank/black, then the app reports \"waiting for frames\" — but only after the phone connects (WiFi-Direct handoff), never before"
  - "Recordings are 0-byte: the .mp4 is a 595-byte header with zero video samples, yet the overlay timeline JSON still populates"
  - "firmware CPU collapses from ~249% to ~1% at the moment it breaks (producer threads blocked, not spinning)"
  - "journal shows \"GStreamer ERROR: INVALID_SETTINGS\" + \"GST_ARGUS: Cleaning up\" on both sensors at the exact instant WpaWifiManager::StartP2pGroupOwner runs"
  - "No \"Starting repeat capture requests\" line ever appears again after the teardown — the cameras stay dead until the process restarts"
root_cause: logic_error
resolution_type: code_fix
severity: critical
tags: [argus, wifi-direct, gstreamer, capture-pipeline, watchdog, jetson, camera]
related_components: [wpa-wifi-manager, gstreamer-adapter, pipeline-orchestrator]
---

# WiFi-Direct radio reform kills Argus capture; the producer loop needs a restart watchdog

## Problem
On the Jetson Orin Nano (single WiFi radio), starting the WiFi-Direct P2P group when the phone connects collaterally tears down the Argus camera capture pipeline, and nothing ever restarts it. Result: blank live preview ("waiting for frames") and 0-byte recordings on every phone connection — the two core functions of the device.

## Symptoms
- Blank preview + "waiting for frames" in the app, appearing only *after* the phone connects.
- 0-byte (595-byte header) recording `.mp4` files, while the overlay-timeline JSON for the same clip still has real data.
- firmware CPU drops from ~249% (2.5 cores, healthy dual-camera capture + postprocess) to ~1% — producers are blocked, not busy.
- journal, at the instant of `WpaWifiManager::StartP2pGroupOwner`:
  ```
  WpaWifiManager::StartP2pGroupOwner reused persistent group ...
  GStreamer ERROR: INVALID_SETTINGS      (x2, both sensors)
  GST_ARGUS: Cleaning up                 (x2)
  CONSUMER: Done Success                 (x2)  ← nvarguscamerasrc capture ending
  ```
- Cameras never re-initialize afterward (no further "Starting repeat capture requests").

## What Didn't Work
The failure hid behind a real-but-secondary problem, and several fixes only moved the symptom around:

- **Blamed CPU saturation (the red herring).** Firmware genuinely ran at 249–436% CPU on 6 cores, and a `videorate` 30fps cap + a `Capture()` busy-spin fix were real wins (dropped CPU, restored preview *while no phone was connected*). But CPU was a *secondary layer*: it explains a recurring ~50s Argus `INVALID_SETTINGS` (nvargus-daemon starved under sustained load), not the connect-time teardown. Tuning CPU never fixed the blank-on-connect.
- **Blamed the app / RTSP stream path.** The app-stream server logged "client connected, appsrc bound," so it looked like a streaming bug. It wasn't — the appsrc had no frames to serve because capture upstream was dead.
- **A `Capture()` spin-fix that only changed the symptom.** Before it, `Capture()` returned a cached `last_frame_` when the appsink was empty, so a dead pipeline showed a *frozen* frame. After it (block for the next real sample, else nullopt), the same dead pipeline showed a *blank* frame. Both were the same dead camera — the fix swapped frozen→blank, it didn't address the death.
- **A retained-frame consumer cache (made it worse).** Retaining a `FrameBundle` across consumer ticks pinned the appsink's capped 5-buffer pool and *froze capture to the first frame*. Reverted.
- **Probing RTSP after force-stopping the app.** Invalid test: the app-stream only starts on the WiFi-Direct handoff, so probing with no phone attached proves nothing.

The linchpin that broke the loop: **split record vs. stream.** Recording writes to disk from the consumer's `final_frame`, completely independent of RTSP/appsrc/app-VLC. A 0-byte recording therefore proved the frames were dying **upstream in capture**, not in the stream path — collapsing the search space from "app + network + stream + capture" to just "capture."

## Solution
Add a producer-side watchdog in `PipelineOrchestrator::ProducerLoop`: when the camera's capture pipeline reports it is no longer running, restart it. Re-init is serialized across cameras (a shared radio reform kills both at once, and two concurrent `nvarguscamerasrc` re-acquisitions race the process-wide nvargus session), and the loop stays shutdown-aware.

```cpp
while (running_) {
    // Watchdog: a capture pipeline can die mid-run and stay dead. Argus posts
    // INVALID_SETTINGS when the WiFi-Direct radio reforms its P2P group on phone
    // connect; HandleBusMessages()->Stop() tears the pipeline down, and nothing
    // else ever restarts it — the producer would otherwise spin Capture()
    // (nullopt) on a NULL pipeline forever (blank preview, 0-byte records).
    if (!capture.IsRunning()) {
        {
            // Serialize re-init: two concurrent nvarguscamerasrc re-acquisitions
            // race the shared nvargus session. Restart() is Stop()+Start().
            std::lock_guard restart_lock(restart_mtx_);
            capture.Restart();
        }
        if (!running_) {
            break;  // shutdown raced the restart — exit now, don't Capture/backoff
        }
        if (!capture.IsRunning()) {
            spdlog::warn("camera {} capture down; restart failed, retrying in {}ms",
                         camera_index, config_.capture_restart_backoff.count());
            // Interruptible backoff so Stop() aborts the wait promptly.
            constexpr auto kBackoffSlice = std::chrono::milliseconds(50);
            for (std::chrono::milliseconds waited{0};
                 waited < config_.capture_restart_backoff && running_;
                 waited += kBackoffSlice) {
                std::this_thread::sleep_for(kBackoffSlice);
            }
            continue;
        }
        spdlog::info("camera {} capture restarted after failure", camera_index);
    }
    auto raw = capture.Capture();
    // ... preprocess → materialize → slot.Push
}
```

The teardown detection already existed and was correct: `GStreamerAdapter::HandleBusMessages()` catches the `INVALID_SETTINGS` bus ERROR and calls `Stop()` (sets `is_running_ = false`, pipeline → NULL). What was missing was any code that acted on `IsRunning() == false`. `ICaptureFrame::Restart()` (default `{ Stop(); Start(); }`) already existed on the port with zero callers — the watchdog is its first real use.

On-metal result: `INVALID_SETTINGS → "camera 0/1 capture restarted after failure"` within ~1–2s, preview live, CPU back to ~249%. Over 8 minutes: 7 deaths, 7 first-try recoveries, 0 failed attempts.

## Why This Works
The full causal chain:
1. Phone connects → `WpaWifiManager::StartP2pGroupOwner` does a **full radio reform** (`P2P_GROUP_REMOVE * → P2P_GROUP_ADD persistent`) while the cameras are capturing.
2. On this single-radio device, the channel bring-up disrupts the CSI/VI capture path → Argus posts `INVALID_SETTINGS` on both sensors (a *correctable* error, but nvarguscamerasrc responds by cleaning up the stream).
3. `HandleBusMessages()` → `GStreamerAdapter::Stop()` tears the pipeline down.
4. **The gap:** `ProducerLoop` looped on the *orchestrator's* `running_` flag, not the *camera's* health. It kept calling `Capture()` on a NULL pipeline, which returns `nullopt` forever → producers idle, consumer starves, every sink (record + stream) gets zero frames.

The watchdog closes the gap: it makes camera health (`capture.IsRunning()`) a loop precondition and recovers from any teardown — the WiFi-triggered one and the recurring ~50s CPU-starvation one alike. It doesn't need to know *why* Argus died, only that it did.

Why serialize the restart: startup starts cameras sequentially on the main thread and never re-inits Argus concurrently. Moving re-init onto per-camera producer threads silently broke that invariant; a shared radio reform knocks out both cameras on the same tick, so without `restart_mtx_` both producers would re-acquire the shared nvargus session at once.

## Prevention
- **Never assume a capture/subprocess pipeline stays up.** A correctable hardware error (Argus `INVALID_SETTINGS`) tore a pipeline down permanently because the control loop had no recovery path. Producer/consumer loops that own an external resource must detect its death and restart it, not loop on an unrelated liveness flag. (Same lesson class as `docs/solutions/architecture-patterns/bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md`.)
- **Diagnostic technique — split independent sinks to localize a fault.** Recording (disk, from the consumer `final_frame`) and preview (RTSP → app VLC) share only the upstream capture→consumer path. When *both* die, the fault is upstream; when only one dies, it is in that sink's tail. This one split collapsed a four-subsystem search (app / network / stream / capture) to one.
- **Regression test** — `PipelineOrchestratorTest.WatchdogRestartsDeadCaptureAndFramesResume`: a `FakeCapture` that flips `IsRunning()` false once after N frames, asserting the watchdog `Restart()`s it (`StartCalls() >= 2`) and frames resume reaching the sink. No hardware needed — the fault injects through the `ICaptureFrame` port.
- **Known follow-up gap (frame-truth watchdog).** `GStreamerAdapter::Start()` sets `is_running_ = true` *before* its 2s prime pull, so a pipeline that reaches PLAYING but never delivers a frame reads as "recovered," and a silent frame-stall (frames stop with *no* bus ERROR) is invisible to any `IsRunning()`-based watchdog. The observed trigger posts a bus ERROR, so it is covered today; making `IsRunning()` frame-truth-based (or treating N consecutive `nullopt` Captures as death) would close the silent-stall path. Flagged unanimously by correctness/reliability/adversarial review.
- **Root vs. resilience.** The watchdog is the resilience floor. The underlying recurring ~50s `INVALID_SETTINGS` is nvargus starvation under sustained CPU (~249%); the real cure is the CPU-offload work (VIC/encode offload, proxy) tracked separately. See `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`.

## Related Issues
- `docs/solutions/integration-issues/wifi-direct-data-plane-idempotency-2026-06-26.md` — the *other* blast radius of the same `StartP2pGroupOwner` radio reform: there it breaks the WiFi data plane's own re-establishment; here it collaterally kills camera capture. Same trigger site, different subsystem.
- `docs/solutions/integration-issues/camera-undiscoverable-ble-after-connect-2026-07-09.md` — the *third* blast radius: a lingering idle P2P-GO starves BLE advertising (RTL8822CE coexistence). Its fix bounds idle groups to a 20s teardown grace, which increases P2P-reform frequency — i.e. more of the Argus restarts *this* watchdog exists to absorb.
- `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md` — producer/control-thread lock discipline in this same orchestrator; "see also" for the restart-mutex reasoning.
- `docs/solutions/architecture-patterns/bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md` — thematic precedent: don't assume a disruptable operation self-heals.
- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — the CPU-ceiling context behind the recurring Argus starvation.
- Commits: `28fcea3` (watchdog), `874b19a` (serialize + `Restart()` + shutdown-aware + test) on `feat/capture-transfer-pipeline`.
