---
title: "feat: Record + Stream Video Quality Rework"
type: feat
status: active
date: 2026-07-09
origin: docs/brainstorms/2026-07-09-record-stream-quality-requirements.md
---

# feat: Record + Stream Video Quality Rework

**Target repo:** sst-cam-firmware (all paths below are repo-relative to it).

## Summary

Trade latency for quality on the record + stream (NOT preview) software x264 encodes: parameterize the single shared encode fragment so record and stream drop `tune=zerolatency` and gain B-frames + lookahead + a real rate-control window, raise the stream bitrate off its stuck 4 Mbps default to ~12–16 Mbps, move both quality paths to a slower measured x264 preset, and deepen the pre-encoder queue to absorb brief sub-realtime dips — while preview keeps zerolatency/shallow-queue untouched, the record master stays clean 1080p, and the moov-safety + match-end-flush contracts hold.

---

## Problem Frame

1080p30 record + stream on the Orin Nano (no NVENC, all software `x264enc`) is blocky/pixelated. Root causes, metal-observed: the stream is pinned at `kDefaultBitrateKbps=4000` regardless of resolution (1080p @ 4 Mbps is starved) and the streaming handler never even sets a bitrate from proto; the shared `BuildEncodeFragment` hardcodes `tune=zerolatency` (kills B-frames/lookahead, the biggest quality-per-bit lever) for *all four* consumers; three concurrent software encodes force the fastest, worst-quality presets. The operator accepts a 30–60 s delay on record + stream — which is exactly the budget that unlocks the quality levers low-latency encoding forbids. See origin: `docs/brainstorms/2026-07-09-record-stream-quality-requirements.md`.

---

## Requirements

- R1. Record + stream both **1080p30, visibly much better** than today's 4 Mbps/ultrafast ("phone-like").
- R2. **Clean 1080p record master preserved** — no burned-in overlay in the L1 recording.
- R3. **Drop `tune=zerolatency`** on record + stream; enable B-frames + lookahead + a real RC window. **Preview keeps `tune=zerolatency`.**
- R4. **Raise the quality-path bitrate** to ~12–16 Mbps; stream bitrate must scale with resolution, not be a flat 4000.
- R5. **Slower x264 preset** for record + stream, **measured on-device** to sustain ≥1× realtime with all three encodes running.
- R6. **Dip-absorption buffer** ahead of each quality-path encode so brief sub-realtime dips don't drop frames; the `leaky=downstream` backstop still guarantees a valid moov under sustained overload.
- R7. **Match-end flush** blocks the app until buffered frames are drained, the recording is finalized (valid moov), and the stream is stopped cleanly — for record-only, stream-only, and both.
- R8. **Live preview unchanged** — quality + latency identical to today.
- R9. Encodes **sustain ≥1× realtime** over a full match — no unbounded backlog, no drops beyond the buffer's absorption, and no regression of Argus capture stability (CPU headroom preserved).
- R10. **720p-stream fallback** available via config if two 1080p encodes + preview can't hold realtime on metal (record master stays 1080p).

---

## Scope Boundaries

- **Live preview** (RTSP app-stream): latency, quality, encode, queue depth — untouched. It keeps `tune=zerolatency` + shallow queue (2).
- The **internal training proxy** encode — untouched (keeps zerolatency/ultrafast).
- **Sharing a single encode** across record + stream — rejected; the clean master requires a separate clean encode from the overlaid stream encode.
- **Independent record-vs-stream quality knobs** — both target 1080p30-good; they differ only by overlay/clean content, not quality settings. No new per-path quality proto surface.
- **Hardware / NVENC** encode — does not exist on this silicon.
- **The on-demand burned-overlay export** (`<match>-overlay.mp4`) — unrelated; not touched.

### Deferred to Follow-Up Work

- **Per-stream bitrate as an app/proto field** — today stream bitrate is a firmware constant and the handler never sets it. This plan raises the constant (+ env knob); threading a real bitrate field through the proto + `PlatformStreamConfig` + `streaming.handler.cpp` is a separate change if the app ever needs runtime control.
- **Capturing the final tuned preset/bframe/bitrate/buffer values as a `docs/solutions/` learning** via `/ce-compound` after metal sign-off (the superfast/14Mbps/TNR rationale is currently only in scattered notes).

---

## Context & Research

### Relevant Code and Patterns

- **Shared encode builder:** `src/adapters/storage/gstreamer/encode-fragment.{hpp,cpp}` — `BuildEncodeFragment(const EncodeFragmentParams&)`. `tune=zerolatency` is a **hardcoded literal** at `encode-fragment.cpp:60`; the leaky pre-encode queue is at `:59`; `SST_X264_PRESET` env override at `:47-49`. Per-path knobs are already carried as `EncodeFragmentParams` fields set via designated initializers — the idiomatic extension point.
- **The four callers** (exhaustive): record `src/adapters/storage/gstreamer/recorder-launch.cpp:27` (`superfast`/14 Mbps/queue 30/`name="enc"`); stream `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp:35` (`ultrafast`/`cfg.bitrate_kbps`/queue 30); **preview** `src/adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.cpp:41` (`ultrafast`/queue 2); proxy `src/adapters/storage/proxy/proxy-launch.cpp:15`.
- **Stream bitrate stuck:** `kDefaultBitrateKbps=4000` at `src/domain/streaming/models/platform-stream-config.hpp:19`; `streaming.handler.cpp:65-80` sets url/w/h/fps but **never `bitrate_kbps`** → always the 4000 default.
- **Record finalize / moov:** `GstContinuousRecorder::Stop` blocks on the bus up to `kFinalizeTimeoutSeconds=10` (`gst-continuous-recorder.cpp:22,:133-135`) after EOS so `mp4mux` writes a valid moov. `RecordingService::Push` uses `try_to_lock` (`recording-service.cpp:197`) so match-end finalize never freezes the still-live stream (see learning below).
- **Clean-vs-overlaid split already exists:** `PipelineOrchestrator::ConsumerLoop` pushes clean frames to `record_sink_` **before** compositing (`pipeline-orchestrator.cpp:319-323`) and overlaid frames to `stream_sink_` (`:336-363`). Two fully independent sinks — no rework needed to keep the master clean.
- **Modes source of truth:** `kSupportedVideoModes` at `src/domain/common/models/video-quality.hpp:37` — `{1080p30, 720p60, 720p30}`; **1080p60 excluded, do not re-add.**
- **Launch-string test pattern:** pure-string `Contains(haystack, needle)` assertions, `Params()` baseline factory, magic-number `NOLINT` wrappers — `tests/storage/encode_fragment.test.cpp` (currently asserts `tune=zerolatency` at `:52,:127`).

### Institutional Learnings

- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — **the load-bearing envelope.** Measured: 1080p30 sustains ~1× realtime standalone; **one CPU encoder shared across record + preview + stream** — size against *concurrent* load. Mandatory on every software path: pin `format=I420` before `x264enc`; `leaky=downstream` queue ahead of the encoder (drop frames, never corrupt moov). A too-slow encode grows the appsrc backlog → moov flush can't drain within the finalize timeout → unplayable MP4.
- `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md` — **finalize timeout is coupled to buffer sizing** (`kFinalizeTimeoutSeconds` was widened 5s→10s *because* the leaky queue was added). `Stop()` holds `mtx_` through the whole moov flush; the producer `Push` must stay `try_to_lock`-drop-frame so a stop can't freeze the live stream. Any buffer/queue resize in this rework must revisit both.
- `docs/solutions/integration-issues/wifi-direct-reform-kills-argus-capture-needs-watchdog-2026-07-03.md` — sustained CPU ~249% starves Argus capture (`INVALID_SETTINGS`). **Encode CPU is a capture-stability constraint, not just a quality one** — the slower preset + B-frames must be budgeted against Argus headroom on metal (R9).
- `docs/solutions/integration-issues/live-streaming-start-two-bugs-2026-07-09.md` — working RTMP baseline is `rtmp://host/live/key` h264 1080p30 + silent AAC; app inlines the key into the URL, sends no separate `stream_key`. Metal validation tooling: `journalctl -u sst-cam-firmware`, `ss -tnp | grep :1935`, `ffprobe rtmp://host/live/key` from a third box. Don't regress this contract.

### External References

None gathered — local patterns are strong (the fragment is already parameterized per-path four ways; x264enc/GStreamer conventions are established in-tree; the encode-ceiling learning covers the domain). Exact x264enc property tuning (`bframes`, `rc-lookahead`, `b-adapt`, `vbv-buf-capacity`) is confirmed at implementation against the on-device GStreamer plugin version.

---

## Key Technical Decisions

- **Parameterize `tune`/B-frames per path via new `EncodeFragmentParams` fields — not the global `SST_X264_PRESET` and not a global tune flip.** Add e.g. `bool low_latency{true}` + `int bframes{0}` + `int rc_lookahead{0}` (default = today's behavior). Preview and proxy inherit the defaults (zerolatency, no reorder); record + stream opt into `low_latency=false` + B-frames/lookahead. Rationale: `tune=zerolatency` at `encode-fragment.cpp:60` is shared by all four callers, so a global change would silently degrade preview latency (R8). The struct-field + designated-initializer pattern is exactly how `default_preset`/`SST_X264_PRESET` already works.
- **The "~30 s delay buffer" is realized as a modestly deepened, still-leaky, time-based pre-encode queue — NOT a literal 30 s raw hold.** Reasoning: (a) the 30–60 s tolerance is primarily *output-latency permission* that pays for B-frames + a slower preset (encode latency), not a mandate to buffer 30 s of frames; (b) 30 s of raw 1080p is ~2.8 GB **per encode** — prohibitive for two encodes; (c) a deep pre-encode buffer lengthens match-end finalize, which must *encode* the whole raw backlog before the moov closes (R7), so buffer depth is bounded by acceptable flush time too; (d) the buffer only needs to smooth *brief* dips — sustained sub-realtime is unfixable by any buffer and must hit the `leaky=downstream` backstop to protect the moov. Net: deepen the existing pre-encode queue on quality paths (time-based, keep `leaky=downstream`), start well below 30 s raw, and tune depth on metal against the RAM + flush-time budget. **Post-encode delay was rejected** — it delays output but gives the encoder zero slack, so it does nothing for encoder dips (R6).
- **Raise stream bitrate by bumping `kDefaultBitrateKbps` (~14000, aligning with the recorder) + a `SST_STREAM_BITRATE_KBPS` env knob** mirroring the recorder's `SST_REC_BITRATE_KBPS`. Rationale: the handler never sets bitrate from proto, so the constant is the real control point; an env knob makes it dialable on metal without a rebuild. A proto field is deferred (Scope Boundaries).
- **Record needs no audio; stream keeps its silent-AAC track unchanged.** The MP4 master is video-only (fine); the RTMP silent-AAC track stays (YouTube-class platforms reject video-only FLV). Resolves the origin's audio open question — no change.
- **720p-stream fallback is config-only, no new code.** The RTMP encode already takes its scale target from `cfg.width/height` — dropping the stream to 720p while the record master stays 1080p is a config/env change, documented as the CPU fallback ladder, not a code path.
- **Preset stays per-path (`default_preset`), committed as a measured value.** Do not reach for `SST_X264_PRESET` to slow record/stream — it's uniform across all four consumers and would drag preview onto the slow preset.

---

## Open Questions

### Resolved During Planning

- **Buffer mechanism + placement** → deepened, leaky, time-based **pre-encode** queue on quality paths (see Key Technical Decisions); post-encode rejected.
- **Audio** → no change; record is video-only, stream keeps silent AAC.
- **Config surface for raised bitrate** → bump `kDefaultBitrateKbps` + `SST_STREAM_BITRATE_KBPS` env; per-mode/proto table deferred.
- **Start/stop divergence** → record and stream are already fully independent sinks/handlers (`RecordingHandler` vs `StreamingHandler`, distinct sinks in `pipeline-orchestrator.cpp`), so record-only / stream-only / both already flush independently; the plan only has to keep each path's new buffer independently flushable.

### Deferred to Implementation

- **Exact x264enc property values** — `bframes` count (start 2–3), `rc-lookahead` depth, `b-adapt`, and whether to set a `vbv-buf-capacity`/rate-control window — confirmed against the on-device plugin and dialed by the U6 metal measurement.
- **Final preset per quality path** — `superfast` is the current record landing and the starting point; the slowest preset that holds ≥1× realtime with all three encodes is picked on metal (U6). Fallback ladder if 1080p×2 + preview can't hold: slower→faster preset, then 720p stream (R10).
- **Final pre-encode queue depth** (seconds/buffers/bytes) and the resulting **`kFinalizeTimeoutSeconds`** value — co-tuned on metal against RAM + flush time (U4/U5).

---

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

Per-path encode profiles, one shared builder:

```
                         BuildEncodeFragment(params)
                                   │
     ┌─────────────────────────────┼─────────────────────────────┐
     │ low_latency=true (default)  │  low_latency=false (quality) │
     │ tune=zerolatency            │  (no tune=zerolatency)       │
     │ bframes=0  rc-lookahead=0   │  bframes=N  rc-lookahead=M   │
     │ shallow leaky queue         │  DEEPER leaky queue (dips)   │
     └─────────────────────────────┴─────────────────────────────┘
        ▲                    ▲            ▲                ▲
     preview (RTSP)        proxy       RECORD (clean)   STREAM (overlaid)
     queue=2               small       superfast*/14M   superfast*/~14M
     UNCHANGED             UNCHANGED    name="enc"       + raised bitrate
                                        (*preset measured on metal, U6)

Clean/overlaid split is upstream and already correct:
  ConsumerLoop ──clean frame──►  record_sink_  ──► record encode (quality profile)
              └─composite overlay─► stream_sink_ ─► stream encode (quality profile)

Match-end flush (R7), per independent path:
  STOP ─► appsrc EOS ─► drain+encode pre-encode backlog ─► mp4mux moov / rtmp flush
          (bounded by queue depth ⇒ kFinalizeTimeoutSeconds must cover it)
          RecordingService::Push stays try_to_lock ⇒ live stream never frozen
```

---

## Implementation Units

- U1. **Parameterize the encode fragment for a quality (non-low-latency) profile**

**Goal:** Make `tune=zerolatency`, B-frames, lookahead, and pre-encode queue depth per-path instead of hardcoded, defaulting to today's exact behavior.

**Requirements:** R3, R6, R8

**Dependencies:** None

**Files:**
- Modify: `src/adapters/storage/gstreamer/encode-fragment.hpp` (add fields to `EncodeFragmentParams`; update the `:79-81` doc comment that claims "always `tune=zerolatency`")
- Modify: `src/adapters/storage/gstreamer/encode-fragment.cpp` (`BuildEncodeFragment` — build `tune=`/`bframes=`/`rc-lookahead=` tokens conditionally; deepen the queue when a time bound is set)
- Test: `tests/storage/encode_fragment.test.cpp`

**Approach:**
- Add `bool low_latency{true}`, `int bframes{0}`, `int rc_lookahead{0}` (and a time/`max-size-time` field for the deeper queue, e.g. `int queue_max_time_ms{0}` meaning "unset → keep current buffer-count-only behavior"). Defaults reproduce today's string byte-for-byte for the three unchanged callers.
- When `low_latency` → emit `tune=zerolatency` and no reorder props (current behavior). When `!low_latency` → omit `tune=zerolatency`, emit `bframes={bframes} rc-lookahead={rc_lookahead}` (and `b-adapt=1`); keep `key-int-max`.
- Keep `leaky=downstream` unconditionally (moov safety). If `queue_max_time_ms>0`, add `max-size-time=<ns>` to the pre-encode queue; leaky stays the backstop.
- Do NOT touch `SST_X264_PRESET` semantics (it stays global) — the slow preset comes from each caller's `default_preset`.

**Patterns to follow:** Existing `default_preset` + `SST_X264_PRESET` handling in `encode-fragment.cpp:47-49`; designated-initializer struct usage at every caller.

**Test scenarios:**
- Happy path: default params (no `low_latency` set) → fragment still contains `tune=zerolatency` and no `bframes=` (byte-compatible with today).
- Happy path: `low_latency=false, bframes=3, rc_lookahead=20` → fragment omits `tune=zerolatency`, contains `bframes=3` and `rc-lookahead=20`.
- Edge case: `low_latency=false` with `bframes=0` → omits `tune=zerolatency`, still valid (no negative/garbage tokens).
- Edge case: `queue_max_time_ms>0` → queue contains `max-size-time=<ns>` AND still `leaky=downstream`; `queue_max_time_ms=0` → no `max-size-time` bloat, leaky present.
- Invariant: `format=I420` still pinned before `x264enc` on every profile.
- Update the two existing assertions at `encode_fragment.test.cpp:52,:127` to reflect the new default/quality split.

**Verification:** `cmake --build --preset test` clean; `encode_fragment.test.cpp` passes; the three unchanged callers' generated strings are unchanged (assert via their own launch tests in U2/U3-regression).

---

- U2. **Record path → quality profile**

**Goal:** Record encode drops zerolatency, gains B-frames/lookahead, keeps the clean 1080p master and `name="enc"`.

**Requirements:** R1, R2, R3, R5

**Dependencies:** U1

**Files:**
- Modify: `src/adapters/storage/gstreamer/recorder-launch.cpp:27` (pass `low_latency=false`, `bframes`, `rc_lookahead`; keep `default_preset=superfast` as the measured starting point, keep 14 Mbps)
- Modify: `src/adapters/storage/gstreamer/recorder-launch.hpp` if new tuning constants land here (`kRecorderBframes`, `kRecorderRcLookahead`)
- Test: `tests/storage/recorder_launch.test.cpp`

**Approach:** Only the fragment params change; the `appsrc ! frag ! h264parse ! mp4mux ! filesink` shape is untouched. Record stays clean because the clean/overlaid split is upstream in the orchestrator (no change here).

**Patterns to follow:** `recorder-launch.cpp` existing param block; `recorder_launch.test.cpp` Contains-assertions.

**Test scenarios:**
- Happy path: recorder launch string omits `tune=zerolatency`, contains `bframes=`/`rc-lookahead=`, still contains `speed-preset=superfast`, `bitrate=14000`, `x264enc name=enc`, `mp4mux`, `leaky=downstream`.
- Invariant: still `format=I420` before `x264enc`; still `is-live=true` appsrc.

**Verification:** `recorder_launch.test.cpp` passes; build clean.

---

- U3. **Stream (RTMP) path → quality profile + raised bitrate; preview/proxy regression-guarded**

**Goal:** RTMP stream drops zerolatency, gains B-frames/lookahead, and encodes at ~12–16 Mbps instead of 4 Mbps — while proving preview and proxy strings are unchanged.

**Requirements:** R1, R3, R4, R8, R10

**Dependencies:** U1

**Files:**
- Modify: `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp:35` (pass `low_latency=false`, `bframes`, `rc_lookahead`; preset starting at `superfast`)
- Modify: `src/domain/streaming/models/platform-stream-config.hpp:19` (bump `kDefaultBitrateKbps` 4000 → ~14000; update the doc comment)
- Modify: `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp` and/or `rtmp-launch.hpp` to honor a `SST_STREAM_BITRATE_KBPS` env override (mirror recorder's `SST_REC_BITRATE_KBPS`)
- Test: `tests/streaming/rtmp_launch.test.cpp`
- Test (regression, no code change): assert `gst-rtsp-app-stream-server` + `proxy-launch` strings still contain `tune=zerolatency` — add/extend cases in `tests/storage/encode_fragment.test.cpp` or the respective launch tests

**Approach:** Fragment params + bitrate constant only; keep the `flvmux ! rtmp2sink sync=false` tail, the post-encode `queue leaky=downstream max-size-buffers=3` uplink queue, and the silent-AAC audio branch exactly as-is. 720p fallback needs no code (encode target already from `cfg.width/height`); document it.

**Patterns to follow:** `rtmp-launch.cpp` param block; recorder's env-override handling for the bitrate knob; `rtmp_launch.test.cpp` assertions.

**Test scenarios:**
- Happy path: RTMP launch string omits `tune=zerolatency`, contains `bframes=`/`rc-lookahead=`, `bitrate=14000` (new default), the silent-AAC branch, and `rtmp2sink ... sync=false`.
- Happy path: `SST_STREAM_BITRATE_KBPS=12000` set → `bitrate=12000` in the string; unset → default.
- Edge case: `cfg.width=1280,height=720` (720p fallback) → scale target reflects 720p, record path unaffected.
- Regression: preview (RTSP) fragment still contains `tune=zerolatency` and `queue ... max-size-buffers=2`; proxy fragment still contains `tune=zerolatency`. (R8 guard.)
- Error path: silent-AAC branch still present (video-only FLV would be rejected downstream).

**Verification:** `rtmp_launch.test.cpp` + regression cases pass; build clean; preview/proxy strings byte-unchanged.

---

- U4. **Dip-absorption buffer on quality paths**

**Goal:** Give record + stream encoders slack to ride out brief sub-realtime dips without dropping frames, keeping the leaky backstop for sustained overload.

**Requirements:** R6, R9

**Dependencies:** U1, U2, U3

**Files:**
- Modify: `src/adapters/storage/gstreamer/recorder-launch.cpp` and `src/adapters/streaming/gst_rtmp/rtmp-launch.cpp` (set the new `queue_max_time_ms` on the quality paths; add tuning constants)
- Modify: `src/adapters/storage/gstreamer/recorder-launch.hpp` / `rtmp-launch.hpp` (new depth constants + optional `SST_*` env for dial-on-metal)
- Test: `tests/storage/recorder_launch.test.cpp`, `tests/streaming/rtmp_launch.test.cpp`, `tests/storage/encode_fragment.test.cpp`

**Approach:** Set a time-based bound (`max-size-time`) on the quality paths' pre-encode queue, starting **well below** 30 s raw (RAM- and flush-bounded; e.g. a few seconds — exact value from U6). Keep `leaky=downstream` so sustained sub-realtime still drops rather than corrupts the moov. Preview/proxy keep their shallow buffer-count queues (untouched). Make the depth env-dialable so metal tuning needs no rebuild.

**Execution note:** Add the fragment-level string assertion first (cheap), then wire the callers.

**Patterns to follow:** Existing `queue_max_buffers` per-path defaults; recorder env-override idiom.

**Test scenarios:**
- Happy path: record + stream fragments contain both `max-size-time=<ns>` and `leaky=downstream`.
- Edge case: env override changes the depth in the generated string.
- Invariant: preview/proxy queues unchanged (still `max-size-buffers=2` / small, no large `max-size-time`).
- Integration (hardware-bound, on-device): a simulated brief CPU dip does not drop record frames (frame count over a fixed interval matches input within the buffer's absorption); a *sustained* overload leaks (drops) rather than growing the backlog unbounded — recording still finalizes a valid moov.

**Verification:** launch tests pass; on-device, brief dips cause no drop and sustained overload still yields a playable MP4 (R9 backstop intact).

---

- U5. **Match-end flush / finalize-timeout reconciliation**

**Goal:** Ensure the app-blocking match-end flush still drains the (now deeper) quality-path buffers and finalizes a valid moov + clean RTMP stop, for record-only / stream-only / both — without freezing the live stream.

**Requirements:** R7, R9

**Dependencies:** U4

**Files:**
- Modify: `src/adapters/storage/gstreamer/gst-continuous-recorder.cpp:22` (`kFinalizeTimeoutSeconds` — must cover encoding the pre-encode raw backlog at EOS; derive from / bound against the U4 buffer depth)
- Verify (likely no change): `src/app/storage/services/recording_service/recording-service.cpp:197` (`try_to_lock` Push stays), `src/adapters/streaming/gst_rtmp/gst-rtmp-streamer.cpp` stop/flush, `src/app/session/services/session_cleanup/session-cleanup.cpp` (`FinalizeRecording`/`StopStreaming`)
- Test: `tests/storage/continuous_recorder.test.cpp` (hardware-bound), `tests/storage/recording_service.test.cpp`

**Approach:** The deeper pre-encode buffer means EOS must *encode* the buffered raw backlog before mp4mux closes the moov — so the finalize timeout has to scale with buffer depth (bounded flush time is itself a cap on how deep the buffer may be, per the Key Technical Decision). Confirm `RecordingService::Push` stays `try_to_lock`-drop-frame so the finalize never freezes the still-live stream. Confirm RTMP stop flushes its own buffer + flvmux/rtmp2sink cleanly and independently of record.

**Execution note:** Characterization-first — assert the *current* finalize behavior (EOS → valid moov within timeout) before changing the timeout, so the deeper-buffer regression is caught.

**Patterns to follow:** `non-blocking-sink-with-async-stop` learning; existing `Stop()` bus-wait loop in `gst-continuous-recorder.cpp:120-145`.

**Test scenarios:**
- Happy path (record-only): STOP after a filled buffer → valid, playable moov written within the (revised) timeout; buffered tail is encoded, not dropped.
- Happy path (stream-only): STREAMING_STOP flushes + closes RTMP cleanly with no record involvement.
- Happy path (both): both flush independently; record moov valid AND RTMP stopped clean.
- Integration: while record `Stop()` holds `mtx_` through finalize, a concurrent fan-out `Push` to the live stream is NOT blocked (frame dropped via `try_to_lock`, stream keeps flowing).
- Edge case: buffer deeper than the old 10 s finalize would drain → revised timeout covers it (no truncated/ invalid moov).

**Verification:** on-device, a full match STOP yields a playable MP4 with the buffered tail intact and the app unblocks only after finalize; the live stream never freezes during a record stop.

---

- U6. **On-metal measurement + commit the validated tuning defaults**

**Goal:** Pick and commit the slowest preset + B-frame/lookahead/bitrate/buffer-depth values that sustain ≥1× realtime for record + stream + preview concurrently, without destabilizing Argus capture.

**Requirements:** R1, R5, R9, R10

**Dependencies:** U2, U3, U4, U5

**Files:**
- Modify: the tuning constants landed in `recorder-launch.hpp` / `rtmp-launch.hpp` / `platform-stream-config.hpp` (replace starting guesses with measured values)
- Add (docs): a short measurement note under `docs/` capturing the procedure + chosen values (feeds a later `/ce-compound`)

**Approach (measurement protocol, beta-rung / real Jetson + app):**
- Run all three encodes concurrently (record + stream + live preview) over a representative match-length interval.
- Measure: sustained encoder fps vs source fps, pre-encode queue backlog growth, total CPU% (watch for the ~249% Argus-starvation threshold + `INVALID_SETTINGS` in `journalctl -u sst-cam-firmware`), RTMP egress via `ss -tnp | grep :1935`, and output quality via `ffprobe rtmp://host/live/key` from a third box + visual check.
- Walk the preset from `ultrafast`→`superfast`→`veryfast`/`faster` and stop at the slowest that holds ≥1× realtime with no unbounded backlog and no Argus starvation. If 1080p×2 + preview can't hold at any acceptable preset, drop the **stream** to 720p (R10) with the record master staying 1080p.
- Fix `bframes`/`rc-lookahead`/bitrate/buffer-depth at the values that give best quality within the sustained-realtime envelope.

**Execution note:** Pure on-device validation + constant-tuning; no new logic. Hardware-bound — cannot pass in the dev container.

**Test scenarios:** `Test expectation: none (on-device measurement + constant tuning; validated by the U4/U5 hardware-bound integration tests and the metal protocol above, not new unit tests).`

**Verification:** record + stream both 1080p30 visibly better than the 4 Mbps/ultrafast baseline; encodes sustain a full match with no unbounded backlog and no Argus `INVALID_SETTINGS`; preview unchanged; committed constants reflect the measured values.

---

## System-Wide Impact

- **Interaction graph:** `BuildEncodeFragment` is shared by 4 consumers — record + stream change, preview + proxy MUST NOT (guarded by U3 regression tests + default params reproducing today's string). The clean/overlaid split in `PipelineOrchestrator::ConsumerLoop` is unchanged (record already gets clean pixels).
- **Error propagation:** RTMP socket drop/reconnect must not stall or corrupt the record tee — record and stream are independent sinks with independent buffers; RTMP already has a post-encode `leaky=downstream` uplink queue + `rtmp2sink sync=false`. Verified as a U3 edge case + U5 stream-only flush.
- **State lifecycle risks:** deeper pre-encode buffer ↔ `kFinalizeTimeoutSeconds` ↔ RAM are a coupled triangle (U4/U5). Under-sized timeout → truncated/invalid moov; over-deep buffer → OOM or long flush. Leaky backstop is the last-line moov guard.
- **API surface parity:** no proto/wire change (stream bitrate stays firmware-side). `kSupportedVideoModes` unchanged — 1080p60 stays excluded.
- **Integration coverage:** concurrent-encode CPU load vs Argus capture stability is the real gate (R9) — only provable on metal (U6), not in unit tests.
- **Unchanged invariants:** preview encode/latency/queue; the on-demand `<match>-overlay.mp4` export; `format=I420`-before-`x264enc` and `leaky=downstream` on every path; `RecordingService::Push` `try_to_lock` contract; the RTMP key-in-URL app contract.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| B-frames + slower preset + higher bitrate on two 1080p encodes push shared CPU past the Argus-starvation threshold (~249%) → capture `INVALID_SETTINGS` | U6 measures total CPU on metal and picks the slowest *sustainable* preset; 720p-stream fallback (R10); leaky backstop keeps output valid even if it can't hold |
| A global `tune`/preset change silently degrades live preview | Per-path params default to today's behavior; U3 regression tests assert preview/proxy strings byte-unchanged (R8) |
| Deeper pre-encode buffer makes match-end finalize exceed `kFinalizeTimeoutSeconds` → invalid moov / lost tail | Buffer depth bounded by acceptable flush time; `kFinalizeTimeoutSeconds` scaled to buffer depth (U5); leaky backstop as last resort |
| 30 s raw buffer taken literally → ~2.8 GB/encode OOM | Key decision: buffer is a modest deepened queue, not a literal 30 s raw hold; env-dialable, measured on metal |
| Stream bitrate raised but handler still can't set it per-stream | Accepted — constant + env knob is the control point; per-stream proto field explicitly deferred |
| Exact x264enc B-frame/RC property names differ on the on-device plugin version | Confirmed at implementation; string-level tests catch typos; metal run (U6) is the final gate |

---

## Documentation / Operational Notes

- After U6 metal sign-off, capture the tuned preset/bframe/lookahead/bitrate/buffer values + rationale as a `docs/solutions/` learning via `/ce-compound` (the encode-ceiling doc is the sibling to update/cross-link).
- Operational dials on metal (no rebuild): `SST_STREAM_BITRATE_KBPS`, `SST_REC_BITRATE_KBPS`, the new buffer-depth env(s), and (with care — global) `SST_X264_PRESET`.
- This ships via the normal maturity ladder: alpha (container build + `ctest`), then **beta is where U6 lives** (real Jetson + app), then stable promotion.

---

## Sources & References

- **Origin document:** [docs/brainstorms/2026-07-09-record-stream-quality-requirements.md](docs/brainstorms/2026-07-09-record-stream-quality-requirements.md)
- Encode builder: `src/adapters/storage/gstreamer/encode-fragment.{hpp,cpp}`
- Callers: `recorder-launch.cpp`, `rtmp-launch.cpp`, `gst-rtsp-app-stream-server.cpp`, `proxy-launch.cpp`
- Finalize/flush: `gst-continuous-recorder.cpp`, `recording-service.cpp`, `session-cleanup.cpp`
- Config: `platform-stream-config.hpp`, `video-quality.hpp`, `quality-mapping.hpp`
- Learnings: `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md`, `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md`, `docs/solutions/integration-issues/wifi-direct-reform-kills-argus-capture-needs-watchdog-2026-07-03.md`, `docs/solutions/integration-issues/live-streaming-start-two-bugs-2026-07-09.md`
