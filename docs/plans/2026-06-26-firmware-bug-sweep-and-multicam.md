# Plan — Firmware bug sweep + multi-cam/overlay (Bug #6 firmware side)

**Date:** 2026-06-26
**Branch:** all work → PRs into `release/0.1.0` (still prerelease — no stable cut yet; bugs and #6 both iterate as betas here)
**Source:** handoff `../../sst-cam-app/docs/handoffs/2026-06-26-bug-sweep-and-local-dev-workflow.md`
**Architecture:** `../../sst-cam-app/docs/brainstorms/2026-06-26-multicam-overlay-architecture-requirements.md`
**Pre-push gates (HARD):** `scripts/fix.sh --all` + `shellcheck deploy/*.sh scripts/ci/*.sh`; build/test devcontainer-only.

---

## Phase A — Bug fixes (ship on release/0.1.0, beta)

### F1 — dnsmasq orphan after crash/OOM
- **Root cause:** `dnsmasq-dhcp-server.hpp:27` `int pid_{-1}` is the *only* handle to the child;
  after SIGKILL/OOM the new process starts `pid_=-1`, so `Start`'s `if(pid_>0)Stop()`
  (`dnsmasq-dhcp-server.cpp:33-36`) is skipped and the stale dnsmasq is invisible. No pre-fork sweep.
- **Fix:** before `fork()` in `Start` (`dnsmasq-dhcp-server.cpp:51`), sweep any stale instance bound
  to the group iface. Pass `--pid-file=<known path>` and kill its contents on next start; fall back to
  `pkill -f "dnsmasq.*--interface=<iface>"`. Independent of the lost in-memory `pid_`.
- **Verify:** start firmware, `kill -9` it, restart → exactly one dnsmasq on the group iface; DHCP serves.

### F2 — SendCommand treats zero-length datagram as a reply
- **Root cause:** `wpa-wifi-manager.cpp:192` guards `bytes < 0`; an empty datagram falls through and is
  returned as the command reply → `StartsWith("OK")` fails → spurious "P2P_GROUP_ADD failed". `ReadUntil`
  guards `bytes <= 0` (`:210`).
- **Fix:** change `:192` to `bytes <= 0` (treat empty datagram as non-reply: `continue` in the bounded
  loop, or return `nullopt`), matching `ReadUntil`.
- **Verify:** unit test feeding a 0-byte datagram → not surfaced as a reply.

### F3 — P2P join race ("wifi failed", app must retry)
- **Root cause:** `wifi-direct.handler.cpp:38-101` brings up GO → addr → dnsmasq synchronously with
  **zero settle**. `ip link set up` failure only warns (`ip-network-configurator.cpp:65-67`); dnsmasq
  readiness is a 0-delay `waitpid(WNOHANG)` probe (`dnsmasq-dhcp-server.cpp:77`). Phone races
  carrier-up / DHCP-listen.
- **Fix:** (1) after `AssignGroupOwnerAddress`, poll the iface for `IFF_UP|IFF_RUNNING`/carrier (bounded
  retry) before `dhcp_.Start`; (2) make `ip link set up` failure fatal-with-retry, not warn-only
  (`ip-network-configurator.cpp:65`); (3) after `dhcp_.Start`, confirm dnsmasq is actually listening
  (brief retry loop) before returning OK to the app.
- **Note:** compounds with F1 post-crash — land F1 + F3 together.
- **Verify:** repeated connect/disconnect from phone with no manual retry; on-device `ip -br addr`, `iw dev`,
  `journalctl -u sst-cam-firmware`.

### F4 — RTSP connects but no frames ("WIFI WAITING FOR FRAMES")
- **Root cause:** `gst-rtsp-app-stream-server.cpp` — `appsrc_` only binds in `OnMediaConfigure` (`:242`)
  on client-connect; `Push()` early-returns while `appsrc_==nullptr` (`:175-177`), dropping pre-connect
  frames. Likely BGR caps/plane-size mismatch (`:35`) stalls x264enc → no SPS/PPS keyframe; `config-interval=1`
  + `key-int-max` (`:42`) means client waits. Risk that `x264enc` (plugins-ugly) doesn't resolve on-device.
- **Fix:** add a leaky `queue` before `x264enc`; assert pushed buffer size == `width*height*3` (BGR);
  verify `x264enc` resolves on the Jetson (the `gst_parse_launch` warning path at `:233`); confirm appsrc
  `max-bytes`/`block` lets buffers through.
- **Verify:** on-device, VLC/`ffprobe rtsp://<go-ip>:<port>/preview` shows decoded frames within ~2s; pairs
  with app A1.

---

## Phase B — Bug #6 firmware (new minor; coordinate with app + proto)

### F6a — Split recorder (clean L1) from stream (overlaid) — **core change**
- **Correction (review):** there is **no GStreamer `tee`** here. Overlay is composited in **plain CPU code**
  inside `ConsumerLoop` on the BGR `Frame` (`pipeline-orchestrator.cpp` ~`:183-189`, `CompositeOverlay`)
  **before** it reaches any GStreamer element. The "fan-out" is the C++ `FanOutSink` object
  (`main.cpp:229-230`) distributing one already-composited `Frame*` to `recording_service` +
  `streaming_service`, which are **three independent `gst_parse_launch` pipelines** (recorder, RTSP, RTMP),
  each with its own `appsrc`. `FanOutSink`'s current contract is "both branches get identical pixels".
- **Change (real shape):** restructure the frame flow at the **`IFrameSink` boundary, not a graph edit**.
  Push the **clean** `final_frame` to the recorder branch *before* `CompositeOverlay`; run `CompositeOverlay`
  and push the result to the streaming branch (RTSP + RTMP). Either move the composite out of `ConsumerLoop`
  into a streaming-only sink, or replace `FanOutSink` with a two-tap sink that composites on the stream tap.
  **Retire the "identical pixels" invariant** (`fan-out-sink.hpp`, `main.cpp:227-228`). `GstContinuousRecorder`
  now writes the clean MP4.
- **Verify:** recorded MP4 has no overlay; live RTSP/RTMP does; no per-frame copy regression in the consumer.

### F6b — Persist overlay-timeline (+ replay clock anchor)
- Timestamp & persist every `PushOverlayLayout` (today only cached-latest in `CachingOverlaySink`) tied to
  the recording's match-uuid / `capture_group_id`. Store as `<capture_group_id>.timeline.json` alongside the
  L1 MP4; created on first `PushOverlayLayout` during recording; **never auto-deleted** (kept for re-export).
  Format: `[{ at_ms: int64, layout: OverlayLayout }]`.
- **Replay alignment (critical):** the live composite samples `LatestOverlay()` per frame against a
  monotonic clock, but the recorder is `appsrc do-timestamp=true` (`gst-continuous-recorder.cpp:75`) — wall-time
  PTS with **no mapping back to the overlay `NowMs` clock**. To make F6c replay faithful (±1 frame), persist a
  recording-start `NowMs` anchor; replay = for each L1 frame, pick the timeline event active at
  `start_ms + pts`. Without this anchor the overlay drifts on replay.
- **Verify:** after a match, the timeline file exists; replay onto L1 reproduces the live overlay timing within
  ±1 frame at a known event boundary.

### F6c — On-demand overlayed burn (background job)
- New flow: app requests "overlayed export" → firmware replays overlay-timeline onto L1 via the existing
  Cairo/`CompositeOverlay` path, encodes L2 (software x264), exposes it for download, then **deletes** it.
  Run as a **background job** (job-id + poll).
- **HARD INVARIANT — no retrieval during a live session.** While a match is live (recording and/or
  streaming active — `is_recording || is_streaming`), the firmware **rejects** any past-video retrieval or
  overlayed-burn request with a busy/`LIVE_SESSION_ACTIVE` error. The software x264 burn would contend with
  the live encode on the no-NVENC Orin Nano and risk dropping the broadcast. The block is enforced
  firmware-side (authoritative) AND surfaced app-side (app A6c hides/disables retrieval). Never burn during
  a live session.
- Proto: new command + job/poll/transfer + the `LIVE_SESSION_ACTIVE` rejection (coordinate with app A6c).
- **Verify:** (1) request on a past clip with no record-time overlay → correctly overlaid L2; file gone after
  transfer; (2) request *while recording/streaming* → rejected with `LIVE_SESSION_ACTIVE`, live session
  unaffected (no frame drops, encode CPU unchanged).

### F6d — Side-by-side preview composite + set-preview-layout  **(DEFER candidate — see Scope note)**
- **Correction (review):** `GstOverlayCompositor` is **NOT** a dual-camera compositor — it is the
  overlay-over-video *source* half (a single `appsrc` emitting RGBA overlay buffers,
  `gst-overlay-compositor.hpp:13-20`). Do **not** reuse it for side-by-side. There is no existing dual-input
  compositor.
- **Real shape:** build a new dual-input stage — either `compositor`/`videomixer` with two appsrc pads, or
  (simpler) CPU-side `cv::hconcat` of the two BGR frames before the existing single RTSP `appsrc` (avoids a
  new GStreamer pad graph, reuses the single-stream encoder). cam1 is materialized as a second `FrameBundle`
  and **aged out in the consumer** (`pipeline-orchestrator.cpp` ~`:147-151`) — route it through
  postprocess (NV12→BGR, resize) into the compositor instead of dropping it.
- New proto `set-preview-layout` (SINGLE | SIDE_BY_SIDE); response carries the updated
  `PreviewStreamDescriptor` (new width/height). Takes **no** camera param — SINGLE uses the firmware's
  active-camera state; SIDE_BY_SIDE shows cam0 (left) | cam1 (right) per `camera_index`, no overlay.
- **GATE — encode budget (do BEFORE building):** a live match already runs **up to 3 concurrent
  `x264enc ultrafast`** (recorder + RTSP + RTMP; the raw sink stays NV12 *specifically* to avoid more
  encodes — `filesystem-raw-capture-sink.hpp:26-28`). Side-by-side makes the preview encode ingest a 2×-wider
  composite. Benchmark on the real Jetson (`sst@10.10.1.30`) per-branch x264 cost for (a) recorder+RTSP-single+RTMP
  and (b) recorder+RTSP-**side-by-side**+RTMP. If (b) doesn't fit, side-by-side must cap res/fps or drop a
  branch — decide here, with a number, not at verify time.
- **Verify:** dropdown switches firmware layout; both feeds visible; measured x264 CPU within budget (cite the
  number).

### F6e — Portrait / 9:16 preview
- Support 9:16 / 1080×1920 in the preview descriptor (firmware-driven width/height). App stops hardcoding
  16:9 (app A2/A6b).

### F6 housekeeping
- Update `../../sst-cam-app/docs/firmware-spec.md`: single recording → clean L1 + overlay-timeline; overlay
  firmware-only; preview layouts; on-demand overlayed export. (App owns the spec file.)
- L0 raw encoding (currently NV12) — decide compression during this phase (storage vs training fidelity).

---

## Sequencing
1. F1+F3 together (P2P reliability), F2, F4 → beta on `release/0.1.0`, validate on Jetson `sst@10.10.1.30`.
2. **Phase D step 0 — proto first.** `proto/` is a **shared git submodule** (app + firmware). New #6 messages
   (`SetPreviewLayoutCommand`, the overlayed-export command/poll/response trio, `LIVE_SESSION_ACTIVE` error
   enum) **do not exist yet**. Land them in `sst-cam-proto` in one PR, then bump the submodule pointer in
   **both** repos' `release/0.1.0` work that consumes them. The bug-fix betas (step 1) ship first and need no
   new proto, so #6's proto bump lands only when the #6 wire surface is ready — keeping in-flight beta
   bug-fix builds clean.
3. Then F6a→F6b→F6c (core L1/timeline/burn — sequential, high-risk). F6d+F6e are independent and gated on the
   encode-budget benchmark; can proceed in parallel with Phase C testing.
4. Every push: `scripts/fix.sh --all` + shellcheck before CI (tidy/shellcheck are hard gates).
