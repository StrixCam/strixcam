---
title: Record + Stream Video Quality Rework — Requirements
date: 2026-07-09
status: ready-for-plan
branch: feat/capture-transfer-pipeline
---

# Record + Stream Video Quality Rework — Requirements

## Problem

Record and stream output on the Jetson Orin Nano is poor / pixelated. The Orin
Nano has **no NVENC** — all H.264 encoding is software `x264enc` on the shared
CPU. Today:

- Record and stream each run a **separate** encode via the shared
  `BuildEncodeFragment` (`src/adapters/storage/gstreamer/encode-fragment.cpp:60`),
  which hardcodes `x264enc speed-preset=<preset> tune=zerolatency bitrate=<kbps>`.
- The RTMP egress bitrate is a fixed `kDefaultBitrateKbps = 4000`
  (`src/domain/streaming/models/platform-stream-config.hpp:19`) regardless of
  resolution — 1080p @ 4 Mbps is starved.
- Three software encodes run at once (preview + record + stream), triple-loading
  the CPU and forcing the fastest (worst-quality) presets.

Metal-observed today: 1080p30 stream @ 4 Mbps, `ultrafast` + `zerolatency` →
blocky/pixelated. `ffprobe` confirmed 1080p30 h264 reaching the RTMP server.

## Goal

Record and stream both at **1080p30, "phone-like" good quality**. The clean
recording master must be preserved.

## Key insight driving the design

**Latency does not matter for record + stream.** The operator explicitly accepts
a **30s–60s delay** on both if it buys much better quality. This unlocks the two
biggest quality levers (B-frames/lookahead, a slower preset) that low-latency
encoding forbids. Only the **live preview** must stay low-latency — its current
quality and delay are fine and are **out of scope / must not change**.

## Decisions (settled — do not re-litigate in planning)

1. **Keep record and stream as two SEPARATE encodes** (option C). Record stays a
   **clean** master (L1, no burned-in overlay; the scoreboard is exported
   on-demand later as `<match>-overlay.mp4`); stream stays **live-overlaid**
   (scoreboard composited on the stream branch). Their pixel content differs, so
   a single shared encode is rejected — the clean master is required. Net: no
   CPU-halving from sharing; the preset ceiling is tighter (still 3 encodes).
   - **The stream overlay MUST be burned into the pixels** — streaming targets are
     YouTube-class platforms that show exactly the sent frames, so a viewer-side /
     client-composited overlay is impossible. This also rules out the alternative
     of "one clean encode teed to both + client-side scoreboard." Two full
     burned-in encodes is the settled shape.
   - **CPU lever if two 1080p encodes can't hold realtime with preview:** the
     stream may drop to **720p** while the record master stays **1080p** (the 2nd
     encode is ~2× cheaper, still burned-in / platform-friendly). Prefer both
     1080p; fall back to 720p-stream only if metal measurement forces it.
2. **Drop `tune=zerolatency`** on the record + stream (quality-path) encodes →
   enable B-frames + lookahead + a real rate-control window. Biggest quality gain
   per bit, near-zero extra CPU; the delay budget pays for it. **Preview keeps
   `zerolatency`.**
3. **Raise the quality-path bitrate** to ~12–16 Mbps (local nginx-rtmp / disk =
   effectively free bandwidth). Bitrate should be sane for 1080p; scale with
   resolution rather than a flat 4000.
4. **Better x264 preset** for record + stream (from `ultrafast` toward
   `veryfast`/`faster`) — the exact preset is **whatever sustains ≥1× realtime on
   metal with all three encodes running**. Must be measured on-device, not
   assumed. Prior tuning landed on `superfast`.
5. **A pre-encode delay buffer (~30s target)** ahead of each quality-path encode
   so brief sub-realtime dips never drop frames. Bigger buffer = more headroom +
   more RAM + more lag; 30s is the starting target, tunable.
6. **Match-end flush contract:** on match end the app **blocks** until the camera
   has drained its buffered frames and finalized the recording + stopped the
   stream cleanly (moov atom written, RTMP flushed). The operator accepts the
   wait.

## Non-goals / out of scope

- Changing the **live preview** (latency, quality, encode) in any way.
- **Sharing a single encode** across record + stream (rejected — clean master).
- Independent record-vs-stream **quality settings** — both target 1080p30 good;
  the prior "independent record/stream quality" idea is superseded here (they
  differ only by overlay/clean content, not by quality knobs).
- Hardware encode / NVENC (does not exist on this silicon).

## Open questions for planning (HOW — resolve in /ce-plan)

- **Buffer mechanism + placement:** a GStreamer `queue` (max-time-based) ahead of
  each x264enc, vs a raw-frame ring buffer feeding the appsrc. Where it lives
  relative to the per-output overlay composite. Memory cost at 30s of 1080p raw.
- **Preset selection:** the on-metal measurement procedure (CPU%, sustained fps,
  backlog growth) to pick the slowest preset that holds ≥1× realtime with
  preview + record + stream concurrent. Fallback ladder if it can't hold.
- **Failure isolation:** an RTMP drop / reconnect must **not** stall or corrupt
  the record tee (record and stream are independent pipelines — confirm the
  buffer/queue leak policy keeps record clean while stream recovers).
- **B-frame / rate-control specifics** (bframes count, lookahead depth, vbv/rc
  mode) that fit the delay budget without hurting the clean master.
- **Audio:** whether the silent-AAC track (added for RTMP) needs any change; does
  the record need audio at all.
- **Start/stop divergence:** stream can start mid-record and stop independently —
  confirm each encode's buffer/flush is independent and match-end flush handles
  "record only", "stream only", and "both".
- **Config surface:** how the raised bitrate + preset + buffer size are configured
  (per-resolution defaults vs app-pushed quality).

## Success criteria

- Record and stream both **1080p30**, visibly much better quality than today's
  4 Mbps/ultrafast (target: "phone-like").
- **Clean recording master preserved** (no burned-in overlay in L1).
- Encodes **sustain ≥1× realtime** over a full match — no unbounded backlog, no
  frame drops beyond the buffer's absorption.
- **Live preview unchanged** (quality + latency identical to today).
- **Match-end flush** completes cleanly (playable MP4 + clean RTMP stop) with the
  app blocking until done.

## Constraints & references

- No NVENC; CPU `x264enc` only. VIC offload (`SST_DISABLE_VIC`) for scale/convert
  to save CPU for the encoders.
- Encode paths: stream `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp`; record
  `src/adapters/storage/gstreamer/recorder-launch.cpp`; shared fragment
  `src/adapters/storage/gstreamer/encode-fragment.cpp` (`BuildEncodeFragment`,
  the single place `tune=zerolatency` is set — but preview reuses it too, so the
  zerolatency change must be **parameterized per path**, not global).
- Overlay: composited on the stream branch in the pipeline consumer
  (`src/app/pipeline/services/orchestrator/`); recordings are clean, overlay is a
  separate on-demand export.
- Prior context: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`,
  `docs/solutions/` image-quality-tuning (superfast/14Mbps/TNR landing),
  `kSupportedVideoModes` in `src/domain/common/models/video-quality.hpp`.
