---
title: "Software H.264 encode ceiling on the Orin Nano (no NVENC): advertise only sustainable modes + a leaky pre-encoder queue"
date: 2026-07-01
category: tooling-decisions
module: adapters
problem_type: tooling_decision
component: tooling
severity: high
applies_when:
  - "Choosing or advertising record/stream resolution+fps modes on the Jetson Orin Nano (or any board whose H.264 path is software x264enc, no NVENC)"
  - "A GStreamer appsrc feeds a software encoder that may fall behind realtime (recording, RTMP, RTSP)"
  - "Deciding what capture/encode modes the firmware advertises to the app (DeviceInfoResponse.supported_modes) and validates against"
tags:
  - nvenc
  - x264
  - encode
  - gstreamer
  - jetson
  - orin-nano
  - realtime
  - leaky-queue
  - mp4
  - moov
  - supported-modes
---

# Software H.264 encode ceiling on the Orin Nano (no NVENC): advertise only sustainable modes + a leaky pre-encoder queue

## Context

The Jetson Orin Nano SKU ships with **no hardware video encoder (NVENC)** — confirmed on
device: `gst-inspect-1.0 nvv4l2h264enc` / `nvv4l2h265enc` resolve to *absent*, and
`/dev/nvhost-msenc` does not exist. (`nvv4l2decoder` (NVDEC), `nvvidconv` (VIC scaler), and
`nvjpegenc` (HW JPEG) *are* present — decode/scale/JPEG are hardware, H.264/H.265 **encode**
is not.) So every recorded/streamed H.264 frame is encoded on the **CPU** via `x264enc`.

The record/stream quality feature (U8) let the app pick resolution+fps and advertised a mode
ladder including 1080p60. On device that mode produced a **broken recording**: the operator
recorded a ~28 s match at 1080p60 and the MP4 was unplayable — `gst-discoverer-1.0` reported
`This file contains no playable streams` (the file had `ftyp` + a 16 MB `mdat` but **no valid
`moov`**), even though the recorder logged `stopped + finalized`.

## Guidance

**Measure the software encoder against realtime before advertising a mode, and advertise only
what sustains.** On this board, a standalone `videotestsrc ! videoconvert ! videoscale !
videorate ! video/x-raw,format=I420,W,H,F ! x264enc speed-preset=ultrafast tune=zerolatency`
timing shows:

| Mode | 10 s of video → wall time | Verdict |
|------|---------------------------|---------|
| **1920×1080@60** | **15.7 s** (~0.64× realtime) | ❌ cannot sustain |
| 1920×1080@30 | 10.1 s (~realtime) | ✅ sustains |
| 1280×720@60 | 10.1 s (~realtime) | ✅ sustains |

Concrete decisions that fell out of this:

1. **`kSupportedVideoModes` is the single source of truth**, and it excludes 1080p60:
   `{1080p30, 720p60, 720p30}`. The same constant drives *both* the
   `DeviceInfoResponse.supported_modes` advertisement (so the app only ever offers sustainable
   options) *and* the firmware-side validation of an app-requested record/stream quality
   (unsupported → fall back to default). One list, two consumers — they can't drift.

2. **A `leaky=downstream` queue sits between the scaler and `x264enc`** in both the recorder and
   the RTMP launch. A software encoder that briefly falls behind (transient load, or a mode near
   the ceiling) then **drops frames** instead of accumulating raw ~MB frames unbounded in the
   `is-live` appsrc. The recording always finalizes a valid file; the cost is dropped frames
   under overload, never a corrupt one.

3. **The no-quality *default* must itself be an advertised/sustainable mode.** The streaming
   `PlatformStreamConfig` default was 1080p60 — the exact unsustainable mode — so a
   `STREAMING_START` with no explicit quality ran it anyway. Dropped the default to 1080p30. A
   test now asserts `IsSupportedMode({kDefaultWidth, kDefaultHeight, kDefaultFramerate})`.

## Why This Matters

A too-slow software encode doesn't just look bad — it **destroys the artifact**. At ~0.64×
realtime the `is-live` appsrc backlog grows for the whole recording; on `Stop`, `mp4mux`'s
moov flush can't drain within the finalize wait (`gst_bus_timed_pop_filtered`,
`kFinalizeTimeoutSeconds`), so the muxer never writes a valid `moov` and the MP4 is unplayable.
"`stopped + finalized`" in the log is a lie in that state — the wait timed out. Advertising a
mode the hardware can't sustain is therefore a data-loss bug, not a quality tradeoff. And the
board shares that one CPU encoder across recording + RTSP preview + RTMP — modes must be sized
against the *concurrent* budget, not in isolation.

## When to Apply

- Before adding/advertising any record or stream mode: time it against realtime on-device
  (`gst-launch` the encode chain, compare wall time to media duration), including the concurrent
  load it will actually run under. Only add it to `kSupportedVideoModes` if it sustains.
- Any new software-encoder GStreamer path: pin `format=I420` before `x264enc` (4:2:0, or players
  reject High 4:4:4) and put a `leaky=downstream` queue ahead of the encoder.
- True 1080p60 (or additional concurrent encodes — e.g. a compressed dual-camera training proxy)
  needs a **hardware encoder**: an Orin **NX/AGX** has NVENC; this Nano does not. `nvjpegenc`
  (HW MJPEG) is the only hardware-accelerated encode on this SKU, at the cost of large
  intra-only files.

## Related

- The finalize-timeout → corrupt-`moov` seam is the same one exploited by a blocking producer on
  the shared fan-out thread: [non-blocking-sink-with-async-stop](../architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md)
  (`RecordingService::Push` example).
- Repo `CLAUDE.md` "No hardware H.264 encoder on the Orin Nano" note — the canonical
  discoverability surface; this doc is the deep-dive it points to.

Key files: `src/domain/common/models/video-quality.hpp` (`kSupportedVideoModes`),
`src/adapters/storage/gstreamer/recorder-launch.cpp`,
`src/adapters/streaming/gst_rtmp/rtmp-launch.cpp`,
`src/domain/streaming/models/platform-stream-config.hpp`.
