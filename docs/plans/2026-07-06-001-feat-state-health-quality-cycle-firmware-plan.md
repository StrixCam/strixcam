---
title: "feat: Sessions that outlive connections — state model, health, flicker, offload, AF, polish"
type: feat
status: active
date: 2026-07-06
origin: docs/brainstorms/2026-07-06-state-health-quality-cycle-requirements.md
---

# feat: Sessions that outlive connections — state model, health, flicker, offload, AF, polish

## Summary

Rework the session model so sessions survive app disconnects (orthogonal connection/wifi-group/session axes, auto-stop safety net, last-session summary), serve the new snapshot/reconcile/time contract, add frame-truth per-camera health with firmware-side command gating, fix the both-mode flicker with a last-good-composite hold, finish the CPU-offload work (VIC postprocess, proxy ref-count, shared encode builder), build the continuous-AF loop, and close with a structural polish sweep. Contract prerequisites: `sst-cam-proto/docs/plans/2026-07-06-001-feat-state-health-contract-plan.md`; app counterpart: `sst-cam-app/docs/plans/2026-07-06-001-feat-state-health-quality-cycle-app-plan.md`.

---

## Problem Frame

Today `SessionManager::OnDisconnect` finalizes and tears down everything, and `OnConnect` rejects any phase but idle — a reconnect into a running session is impossible by construction, and a WiFi blip mid-match destroys the recording workflow. Camera health has no representation (a stalled pipeline reads healthy because `is_running_` is set before the prime pull), the both-mode preview degrades to a full single-camera frame on any per-tick miss, and sustained CPU encode load starves nvargus-daemon (~50s `INVALID_SETTINGS`, currently masked by the watchdog). See origin doc for the full frame.

---

## Requirements

Origin R-IDs: R1–R3, R4 (wire side — snapshot carries the match state the app persists), R5–R8 (state/health), R10 (flicker), R11 (offload), R12–R14 (quality/process), R15 (continuous AF), R16 (time sync). R9 and R17 are app responsibilities (see app plan). Origin flows F1–F3; acceptance examples AE1–AE7.

---

## Scope Boundaries

- App-side behavior (handshake consumption, gating UX, scoreboard persistence) — app plan.
- Contract definition — proto plan; this plan consumes the bumped submodule.
- No mic capture, no degraded single-camera mode, no NVENC (hardware lacks it).
- `_old/` Python tree removal is polish-optional; deleting it is fine, porting anything from it is not in scope.

### Deferred to Follow-Up Work

- Emulator repo parity with the new contract: separate cycle in `sst-cam-emulator`.

---

## Context & Research

### Relevant Code and Patterns

- Session: `src/app/session/services/session_manager/session-manager.cpp` (phase ladder `kIdle→…→kRecording`; `OnConnect` rejects non-idle; `OnDisconnect` full teardown; `OnWifiStopped` drops `kRecording→kConnected` **without finalizing** — data-loss transition), `src/app/session/services/session_cleanup/`, `src/domain/session/models/`.
- BLE: `src/adapters/control/ble/bluez/bluez-ble-transport.cpp` (connect = first GATT write; disconnect = StopNotify → detached worker), dispatcher + 20 handlers in `src/app/control/services/handlers/`.
- Pipeline: `src/app/pipeline/services/orchestrator/pipeline-orchestrator.cpp` (`ProducerLoop` watchdog, serialized `Restart()` via `restart_mtx_`; `ConsumerLoop`/`BuildStreamFrame` — both-mode falls back to full single-camera frame when the other slot is empty; no last-good-composite hold).
- Capture: `src/adapters/capture/frame/gstreamer/gstreamer.cpp` (`is_running_` set before prime pull; prime timeout only warns; `IsRunning()` is control-state only; plain `bool` cross-thread).
- Encode: `recorder-launch.cpp` / `rtmp-launch.cpp` / `proxy-launch.cpp` / `gst-rtsp-app-stream-server.cpp` — four divergent copies of the convert-scale + leaky-queue + x264 fragment; `use_vic` branches exist for encode-side scale/convert; `opencv-postprocessor.cpp` (NV12→BGR) is still full-CPU and feeds every branch.
- Proxy: `src/adapters/raw_capture/proxy-retention.cpp`; proxy stops unconditionally on RECORDING_STOP — planned record-OR-stream ref-count unimplemented.
- Focus: `src/adapters/focus/i2c-focuser.cpp`, `handlers/camera-focus.handler.cpp`, `src/domain/focus/models/focus-state.hpp` (manual works; no AF loop exists).
- Patterns to reuse: background poller (`telemetry-probe.cpp`), non-blocking sink (`try_to_lock` push / async stop), bounded subprocess off dispatcher, pure launch-string builders unit-tested without hardware, shared-mutable state objects written by handlers and read per-tick.

### Institutional Learnings

- `docs/solutions/integration-issues/wifi-direct-reform-kills-argus-capture-needs-watchdog-2026-07-03.md` — P2P group re-formation kills Argus; serialize per-camera re-init; watchdog masks the starvation layer this plan cures.
- `docs/solutions/integration-issues/wifi-direct-data-plane-idempotency-2026-06-26.md` — abrupt drop skips graceful teardown; reconcile at start, idempotently.
- `docs/solutions/architecture-patterns/non-blocking-sink-with-async-stop-2026-06-10.md`, `telemetry-probe-background-poller-off-the-dispatcher-thread-2026-06-30.md`, `bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md`.
- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — x264 ceiling; `kSupportedVideoModes` single source of truth; leaky pre-encoder queue or no `moov`.
- `docs/solutions/tooling-decisions/imx477-magenta-cast-is-isp-tuning-not-saturation-2026-06-30.md` — validate on both sensors; Argus is exclusive.

---

## Key Technical Decisions

- **Three orthogonal state axes** replace the single phase ladder: `app_connected` (BLE), wifi-group (up/down), session (idle/configured/ready/recording). The ladder conflates them and cannot express "recording with no app". Disconnect flips one axis; it no longer implies session teardown.
- **P2P group stays up while a session is active**: group re-formation is the known Argus killer, so rejoin must re-associate to the existing group. Group teardown happens only at session end (including auto-stop) or when idle-with-no-app times out.
- **Auto-stop is session-scoped intent** (from `PushSessionConfig`, default 30 min, app-configurable) covering recording, raw capture, and streaming-only; a preview-only group (nothing running, no app) tears down on the same timer. Auto-stop counts as session end: finalize cleanly, record summary, then teardown.
- **Firmware enforces health gating too** (reject start-class commands when any camera is not OK): app-side gating alone leaves a poll-latency hole. Stop/finalize/downloads/wifi-start are never gated — the operator must always be able to end and retrieve.
- **Health is frame-truth with hysteresis**: OK / RECOVERING / DOWN; a stall enters RECOVERING for one watchdog-restart window before DOWN, so transient Argus stalls don't flap the app UI. Mid-recording DOWN ⇒ hold-then-finalize (origin decision), end-reason = camera failure.
- **Flicker fix = hold last good composite**: in both-mode, a per-tick miss of the non-cadence camera repeats the previous composite instead of degrading to a single-camera frame — the same "hold, don't switch" principle already applied to camera selection, now applied to composition.
- **Offload = reduce CPU pressure feeding x264**, not a new encoder: move the NV12→BGR postprocess to VIC where possible, implement the record-or-stream proxy ref-count, and collapse the four duplicated encode fragments into one builder so presets/env-overrides stop diverging.
- **AF loop is a decision-style consumer**: sharpness metric computed from frames already flowing through the orchestrator, driving `I2cFocuser` from its own off-dispatcher loop; manual focus commands always preempt. Defaults off during recording pending metal validation (origin R15).
- **Wall-clock sync applies app-pushed epoch time at connect**; opportunistic NTP when uplink exists. Session/match clocks stay monotonic — time sync only fixes device-local timestamps (file mtimes, summary fields, logs).

---

## High-Level Technical Design

> *This illustrates the intended approach and is directional guidance for review, not implementation specification. The implementing agent should treat it as context, not code to reproduce.*

```
Axes (replacing the phase ladder):
  app_connected: false ⇄ true            (BLE transport events)
  wifi_group:    down ⇄ up               (held up while session active)
  session:       idle → configured → ready → recording → finalizing → idle
                                  ↑ finalize claimed by CAS to `finalizing` under the session
                                    mutex (app stop | auto-stop | camera failure | reboot scan);
                                    losers of the CAS return immediately — one finalize, ever

Disconnect (app_connected → false):
  session active?  yes → keep session + group; arm auto-stop timer
                   no  → teardown group after same timeout
Connect (app_connected → true):
  cancel auto-stop timer; accept in ANY session state
  (handshake reads: snapshot ← session axes + selections + match + health [+ last-session summary if idle])

Health per camera:  OK → (frame stall) → RECOVERING → (watchdog window expires) → DOWN
                    RECOVERING/DOWN → (frames flow again) → OK
  session recording + any DOWN → finalize(end_reason=camera_failure)
  any != OK → reject start-class commands (start recording/streaming/raw)
```

---

## Implementation Units

### U1. Session state model rework — sessions outlive connections

**Goal:** Replace the phase ladder with orthogonal axes; disconnect keeps an active session (and its WiFi group) alive; auto-stop safety net; last-session summary; orphan-file handling.

**Requirements:** Origin R1, R2, R5; F1, F2; AE1, AE2, AE4

**Dependencies:** None (contract fields consumed in U2; the model lands first)

**Files:**
- Modify: `src/domain/session/models/session-phase.hpp`, `session-state.hpp` (axes model, end-reason, last-session summary model + formatter)
- Modify: `src/app/session/services/session_manager/session-manager.{hpp,cpp}`
- Modify: `src/app/session/services/session_cleanup/` (split "session end" from "connection lost"; retire `ResetSelections`-on-disconnect)
- Modify: `src/app/control/services/handlers/wifi-direct.handler.cpp` (idempotent Start: return the existing group's credentials without re-forming when the group is already up)
- Modify: `src/main.cpp` (wiring)
- Test: `tests/session/` (flat module layout — the test tree has no app/adapters level)

**Approach:**
- `OnConnect` accepts any session state and cancels the auto-stop timer. `OnDisconnect` no longer finalizes or tears down WiFi when a session is active; it arms the auto-stop timer (from session config, default 30 min).
- Fix the `OnWifiStopped`-from-recording path to finalize before dropping state (current data-loss transition).
- Auto-stop covers recording + raw capture + streaming-only; fires → finalize cleanly (recording EOS, streams stopped), record last-session summary (match uuid, end reason, end clock, file-valid), then teardown group. Idle-with-no-app uses the same timer for group teardown.
- Last-session summary survives in memory across idle (and optionally a small JSON in the config dir so it survives a crash — decide at implementation).
- Boot-time scan flags orphaned/unfinalized MP4s (crash artifacts); when one is found, it writes the last-session summary with end-reason = unknown-reboot and file-valid=false — this is the fourth finalize path alongside app-stop / auto-stop / camera-failure.
- Recording elapsed time tracked monotonically for the snapshot (U2).
- Finalize is claimed by compare-and-swap of the session axis to `finalizing` under the session mutex — the four triggers (app stop, auto-stop, camera failure, boot scan) all go through it; losers return immediately. `finalizing` is representable in the snapshot so a connect during finalize never reads a torn state.
- `StartWifiDirect` becomes idempotent: when the group is already up (rejoin path), the handler returns the existing group's SSID/PSK without touching wpa_supplicant — group re-formation is the Argus killer, and the rejoining app's default path goes through this handler. (The app also persists credentials; both paths must agree.)
- Connect/disconnect events carry a generation counter validated under the transport mutex at delivery time: the detached disconnect worker re-checks before invoking `OnDisconnect` and skips if a newer connect superseded it — otherwise a fast resubscribe can arm auto-stop *after* the reconnect cancelled it.

**Execution note:** Characterization tests for current SessionManager transitions first — this is the highest-blast-radius change in the cycle.

**Patterns to follow:** cv-interruptible timer/loop lifecycle from `telemetry-probe.cpp`; idempotent start-side reconciliation per the data-plane-idempotency learning.

**Test scenarios:**
- Happy path: disconnect during recording → session stays recording, group stays up, timer armed; reconnect → timer cancelled, session unchanged. (Covers F2/AE1 firmware half.)
- Happy path: auto-stop fires after configured minutes → recording finalized, summary populated with end-reason auto-stop, group torn down. (Covers AE4.)
- Edge: disconnect with no active session → group teardown after timeout, no summary written.
- Edge: reconnect at timeout−ε → cancel wins deterministically (no stop after a successful connect).
- Edge: reconnect at timeout+ε while finalize is in flight → snapshot is either the pre-finalize truth or idle-with-summary, never a torn intermediate. (Covers AE2's "reboot/idle looks like a first connect" shape from the firmware side.)
- Edge: StopNotify followed by immediate resubscribe+write → no `OnDisconnect` delivered after `OnConnect` (generation counter).
- Happy path: StartWifiDirect while the group is up → same SSID/PSK returned, zero group re-formation, capture uninterrupted.
- Covers AE3: manual disconnect→connect → snapshot reflects the firmware's actual selections and session state, not defaults.
- Covers AE2: firmware reboot then connect → snapshot identical in shape to a first-ever connect (idle, no carryover), plus last-session summary when the reboot orphaned a recording.
- Edge: `OnWifiStopped` while recording → recording finalized (not dropped), end-reason recorded.
- Error: auto-stop finalize when recording already stopped by command race → idempotent, single summary.
- Integration: full connect→configure→record→disconnect→auto-stop→reconnect sequence ends with idle snapshot + summary readable.

**Verification:**
- All four origin connect scenarios pass against the new model in tests; no path finalizes twice or loses a running recording; ctest green in devcontainer.

---

### U2. Snapshot, reconcile, and time handlers — serve the new contract

**Goal:** Proto submodule bump consumed; handlers for session snapshot, absolute match-state set, device time set; auto-stop minutes from session config; protocol version bump.

**Requirements:** Origin R1, R2, R4 (wire side), R16; F1; AE1–AE3

**Dependencies:** U1; proto plan U1

**Files:**
- Modify: `proto/` submodule pin
- Create: `src/app/control/services/handlers/session-snapshot.handler.{hpp,cpp}`, `set-match-state.handler.{hpp,cpp}`, `set-device-time.handler.{hpp,cpp}`
- Modify: `src/app/control/services/handlers/session.handler.*` (handles PushSessionConfig — auto-stop minutes), `match.handler.cpp` (absolute set path shares LiveMatch mutation), `device.handler.cpp` (protocol version)
- Modify: `src/main.cpp` (registration + provider wiring)
- Test: `tests/control/` (flat `*_handler.test.cpp` files, matching existing layout)

**Approach:**
- Snapshot handler assembles from the U1 axes + current selections (active camera, preview layout — the app rehydrates these from the snapshot) + activity flags (is_recording / is_streaming / is_raw_capturing, same sources the telemetry handler reads) + recording elapsed seconds (monotonic, from U1 tracking — the app shows recording duration after rejoin) + `LiveMatch` (absolute scores, period, monotonic elapsed, clock-running) + health getters (U3) + last-session summary when idle. Pure read; no side effects.
- `SetMatchState` overwrites `LiveMatch` absolutely; existing delta `ScoreUpdate` untouched.
- Device-time handler applies epoch ms to system clock (bounded subprocess or direct syscall — decide at implementation; if subprocess, use the bounded-subprocess pattern). Opportunistic NTP enable when uplink active rides the existing network adapter surface.
- All new fields `optional` + `has_*()` checks per contract discipline.

**Patterns to follow:** existing handler structure + `dispatcher.Register` wiring; provider-lambda style in `main.cpp`.

**Test scenarios:**
- Happy path: snapshot during recording carries session=recording, correct elapsed, absolute match state, per-camera health. (Covers AE1 read half.)
- Happy path: snapshot when idle after auto-stop carries last-session summary. (Covers AE4 read half.)
- Happy path: SetMatchState overwrites score/period/clock; subsequent GetMatchState agrees.
- Edge: snapshot with no prior session → summary absent (`has_` false), not zero-filled.
- Edge: SetMatchState with only some fields present → absent fields untouched.
- Error: set-device-time with implausible epoch (pre-2020) → rejected, clock untouched.
- Integration: handshake order protocol-check → time push → snapshot works against the dispatcher end-to-end.

**Verification:**
- App plan U1 can complete its handshake against this firmware in the devcontainer contract tests; protocol version reads bumped.

---

### U3. Frame-truth per-camera health + firmware-side gating

**Goal:** Per-camera OK/RECOVERING/DOWN health from actual frame flow; hysteresis tied to the watchdog window; start-class commands rejected when unhealthy; mid-recording camera death = hold-then-finalize; health in telemetry + snapshot.

**Requirements:** Origin R6, R7 (wire side), R8 (enforcement); F3; AE5

**Dependencies:** U1 (end-reason path), U2 (wire exposure)

**Files:**
- Create: `src/domain/health/models/camera-health.hpp` (+ formatter), `src/app/health/services/health_monitor/health-monitor.{hpp,cpp}` (or co-locate in pipeline module — decide at implementation, follow module layout)
- Modify: `src/adapters/capture/frame/gstreamer/gstreamer.{hpp,cpp}` (last-sample timestamp; `is_running_` → atomic; prime-pull failure no longer leaves "running")
- Modify: `src/app/pipeline/services/orchestrator/pipeline-orchestrator.{hpp,cpp}` (nullopt-streak / last-frame-age feed; watchdog-restart signals RECOVERING)
- Modify: `src/app/control/services/handlers/` start-class handlers (recording/streaming/raw-capture start rejection), `device.handler.cpp` (telemetry health fields)
- Modify: `src/app/session/services/session_manager/` (DOWN-while-recording → finalize with camera-failure reason)
- Test: `tests/pipeline/`, `tests/control/`

**Approach:**
- Health derives from last-frame age per camera (frame truth), not pipeline control state. Stall → RECOVERING immediately; frames resume → OK. DOWN triggers on N completed-and-failed restart attempts for that specific camera — NOT on elapsed time from stall onset: restarts are serialized under `restart_mtx_` (dual-camera radio events queue camera B behind camera A's full Argus reacquisition), so an elapsed-time window would false-DOWN a camera that never got its turn. Thresholds (N, stall age) env-tunable for metal calibration.
- `PipelineOrchestrator::Start()` is in scope: it currently rolls back all cameras when any `IsRunning()` is false, so the prime-pull semantic change would turn a cold-boot prime timeout (common with a cold nvargus-daemon) into a permanently dead pipeline with no watchdog. Start tolerates an unprimed camera — spawn producers anyway, that camera begins life as RECOVERING, and the watchdog + health monitor own recovery.
- Gating: start recording/streaming/raw rejected with a typed error when any camera != OK. Never gate stop/finalize/downloads/wifi-start/reboot/diagnostics reads.
- Session hook: recording + any camera DOWN → hold-then-finalize (single watchdog window already consumed by RECOVERING), end-reason = camera failure.

**Patterns to follow:** background-poller pattern (`telemetry-probe.cpp`) for sampling; shared-state object read by handlers.

**Test scenarios:**
- Happy path: frames flowing both cameras → both OK; telemetry + snapshot report OK.
- Happy path: stall then recovery within window → RECOVERING → OK, no session impact, no DOWN ever reported. (Anti-flap.)
- Edge: prime-pull timeout at start → camera not "running-healthy"; health reflects no frames.
- Error path: start-recording while camera RECOVERING → rejected with typed status; stop-recording while DOWN → accepted. (Covers AE5 enforcement half.)
- Error path: camera DOWN mid-recording → recording finalized cleanly (valid file), summary end-reason = camera failure.
- Integration: watchdog restart sequence produces RECOVERING (not DOWN→UP flap) visible through telemetry.
- Edge: both cameras stall simultaneously, restarts serialize, second camera recovers after 2× a single window → RECOVERING throughout, no DOWN, no finalize.
- Edge: prime timeout on one camera at boot → orchestrator still starts, camera reported RECOVERING, watchdog recovers it, no all-camera rollback.

**Verification:**
- Simulated stall in tests drives the full OK→RECOVERING→DOWN→finalize chain; on metal, unplugging/blinding one camera yields app-visible DOWN within the window and a valid finalized file.

---

### U4. Both-mode flicker: last-good-composite hold

**Goal:** Both-mode preview never emits a single-camera frame; per-tick misses repeat the last composite.

**Requirements:** Origin R10; AE7

**Dependencies:** None (independent of state work)

**Files:**
- Modify: `src/app/pipeline/services/orchestrator/pipeline-orchestrator.{hpp,cpp}` (`BuildStreamFrame` fallback semantics)
- Test: `tests/pipeline/`

**Approach:**
- In side-by-side mode, when the non-cadence camera's slot is empty (or postprocess/composite fails), push the previous successful composite instead of the bare single-camera frame — for short per-tick misses this repeats both panes and is invisible at 30fps.
- Past a bounded staleness cap (a few ticks, tuned on metal), switch to per-tick re-composition: the live chosen camera keeps advancing in its pane while the stale pane holds its last-good frame — a sustained one-camera stall (RECOVERING can last a full serialized restart window) must not freeze the healthy camera's pane. Never degrade to a bare single-camera frame while layout=both.
- Layout switches (single↔both) invalidate the held composite so intentional mode changes stay instant.
- On metal, verify against the exact reported symptom (occasional full single-cam frame in both mode) on both sensors; if a residual flicker source remains (e.g., layout race from a stale `preview_layout_` read), root-cause there next — this unit is the highest-probability fix, not a guess-and-close.

**Test scenarios:**
- Happy path: both slots filled → composite emitted.
- Edge: other-slot empty for 1–2 ticks → previous composite repeated; output frame never equals the bare chosen-camera frame while layout=both.
- Edge: other camera stalls for many ticks (past the cap) → chosen pane keeps advancing per tick, stale pane holds last-good; healthy camera never freezes.
- Edge: composite failure (postprocess nullopt) → same hold behavior.
- Edge: layout switch to single → held composite dropped, single frames flow immediately.
- Integration: simulated alternating misses at consumer cadence produce a stable composite stream (no single-frame interleaves).

**Verification:**
- AE7 on metal: full match length in both mode, zero single-camera flashes, both sensors checked.

---

### U5. Encode offload completion — cure the starvation, not mask it

**Goal:** Cut steady-state CPU enough that the ~50s Argus `INVALID_SETTINGS` starvation stops recurring: VIC-offload the postprocess, implement the record-or-stream proxy ref-count, collapse the four encode fragments into one shared builder.

**Requirements:** Origin R11

**Dependencies:** None (parallel with state work; land before long-session metal validation)

**Files:**
- Modify: `src/adapters/processing/opencv/opencv-postprocessor.*` (internal VIC conversion stage; OpenCV keeps calibration math only — capture stays NV12)
- Create: shared encode-fragment builder (new file beside `recorder-launch.cpp`; pure launch-string builder)
- Modify: `recorder-launch.cpp`, `rtmp-launch.cpp`, `proxy-launch.cpp`, `gst-rtsp-app-stream-server.cpp` (consume shared builder)
- Modify: recording/streaming services for proxy ref-count (proxy runs while recording OR streaming; stops when both idle)
- Test: `tests/storage/`, `tests/streaming/`, `tests/raw_capture/` (new module dir for proxy ref-count)

**Approach:**
- Postprocess is the biggest per-frame CPU cost feeding every branch; the VIC hop sits INSIDE the postprocess adapter (internal appsrc→nvvidconv→appsink stage doing NV12→BGR + scale on VIC), with OpenCV reduced to the calibration math only. Capture keeps emitting NV12 — the preprocessor's NV12-only contract and the AI path's direct Y-plane wrap stay untouched (decision: capture-side BGRx was rejected — it breaks preprocess for every frame and puts 2.7× the bytes through the appsink 5-buffer cap). Keep `SST_DISABLE_VIC` escape hatch.
- Shared builder carries preset/bitrate/env-override handling once (recorder's env overrides become uniform); leaky pre-encoder queue mandatory everywhere (no-`moov` learning).
- Proxy ref-count: start on first of {recording, streaming}, stop on last-out. Keeps the YOLO-bridge proxy alive during stream-only sessions (prior plan's unimplemented U5).
- Measure before/after: steady-state CPU during record+stream on metal; success = no recurring INVALID_SETTINGS watchdog restarts over a full match.

**Patterns to follow:** pure launch-string builders with unit tests; non-blocking sink invariants untouched.

**Test scenarios:**
- Happy path: builder produces valid launch strings for all four consumers (params: preset, bitrate, VIC on/off, resolution) — string-level unit tests.
- Edge: `SST_DISABLE_VIC` set → software path strings.
- Happy path: proxy ref-count — record start → proxy up; stream start → still up; record stop → still up (stream holds); stream stop → proxy down.
- Edge: rapid start/stop interleavings never double-start or orphan the proxy.
- Integration (metal): record+stream full match → zero watchdog restarts from INVALID_SETTINGS; CPU% logged before/after.

**Verification:**
- Origin success criterion: Argus starvation no longer recurs under sustained recording; watchdog becomes true last-resort (log-verified over a full-match metal run).

---

### U6. Continuous autofocus loop

**Goal:** Firmware AF service drives the I2C focuser from a sharpness metric; manual focus preempts; AF off during recording by default.

**Requirements:** Origin R15

**Dependencies:** U3 (don't hunt on a RECOVERING/DOWN camera). The AF-mode wire surface already exists (`CameraFocusControlCommand.mode`: `FOCUS_MODE_AUTO` = continuous, `FOCUS_MODE_MANUAL`; `FocusState` already carries per-camera mode atomics) — no U2/proto dependency.

**Files:**
- Create: `src/app/focus/services/autofocus/autofocus-service.{hpp,cpp}` (or extend `src/app/capture/` — follow module layout), sharpness metric helper
- Modify: `src/adapters/focus/i2c-focuser.*` (if step/position API needs range sweep support), `handlers/camera-focus.handler.cpp` (AF mode), `src/domain/focus/models/focus-state.hpp` (mode)
- Modify: `src/main.cpp` (wiring)
- Test: `tests/focus/` (new module dir)

**Approach:**
- Own off-dispatcher loop (background-poller pattern): sample a sharpness measure (e.g., Laplacian variance on a downscaled center crop) from frames already in the orchestrator path; hill-climb the focuser position; converge and hold with a re-trigger threshold on sharpness drop.
- Manual `CameraFocus` command sets mode=manual and preempts immediately; mode persists in `FocusState`.
- Recording-active → AF loop paused unless env flag enables it (metal validation decides the default flip, origin R15).
- Sharpness sampling must not add per-frame CPU to the hot path — sample at low rate (a few Hz) via a tap on the per-camera producer path (downscaled center crop of the materialized source frame). NOT the proxy: the U5 ref-count stops the proxy exactly in AF's main window (pre-match framing, nothing recording/streaming), and the orchestrator only postprocesses the chosen camera, so the proxy can't serve per-camera sharpness for the unselected camera either.

**Test scenarios:**
- Happy path: synthetic frame sequence with known sharpness peak → loop converges to peak position and holds.
- Edge: sharpness plateau/noise → no oscillation (hysteresis on re-trigger).
- Edge: manual command mid-sweep → sweep aborts, manual position applied, mode=manual.
- Edge: recording starts → loop pauses (default config); recording stops → resumes.
- Error: focuser I2C write fails → loop backs off, health of focus subsystem logged, no crash.
- Integration: AF mode command round-trip flips the loop on/off via dispatcher.

**Verification:**
- On metal: camera converges to sharp focus from a defocused start within a few seconds; no hunting during a recorded match with default config.

---

### U7. Firmware polish sweep

**Goal:** Whole-repo structural polish delivered as bounded per-area sweeps (origin R12), with the broad bug/optimization review riding each area (origin R13) — closed area list below, no open-ended residual hunting.

**Requirements:** Origin R12, R13

**Dependencies:** U1–U6 (last, so it polishes final shapes)

**Files (per-area closed list — together these cover the repo):**
- Area A, composition root: `src/main.cpp` (extract wiring/config/thread-lifecycle into composition units; retire ad-hoc `static` locals; group handler registration)
- Area B, thread lifecycle: `bluez-ble-transport.cpp` (detached disconnect worker → owned/joined), proxy-retention + uplink boot threads (shutdown-aware), clock thread (cv wait instead of 1s sleep)
- Area C, control plane: `src/app/control/` + `src/adapters/control/` review (handler sprawl, dispatcher seams)
- Area D, capture/pipeline/decision: `src/adapters/capture/`, `src/app/pipeline/`, `src/app/decision/`, `src/adapters/processing/` review. `StaticDecision` STAYS — it is the production-wired IDecision in `main.cpp` until AI/decision lands, not a deletion candidate.
- Area E, storage/streaming/raw_capture/export: `src/app/storage/`, `src/adapters/storage/`, `src/{app,adapters}/streaming/`, `src/adapters/raw_capture/`, `src/app/export/` review
- Area F, network/wifi/session/domain: `src/{app,adapters}/network/`, `src/adapters/control/wifi/`, `src/domain/` models + formatters review
- Delete: `src/app/capture/ports/audio-src.hpp` (zero references), `_old/` tree
- Docs drift: raw_capture path naming in older plan references, stale CLAUDE/README pointers
- Test: existing suites stay green; add tests only where a touched seam was untested

**Approach:**
- Behavior-preserving. Each area gets the R13 broad bug/optimization review as it is swept — findings triaged: trivial → fix in that area's pass; behavioral → surface before fixing. Re-check clang-tidy floor NOLINTs against the new code at the end.

**Test scenarios:**
- Test expectation: behavior-preserving refactor — full existing suite green is the gate; new tests only for touched untested seams.

**Verification:**
- Clean shutdown joins every thread (no detached leaks); tidy + format gates green; `src/main.cpp` reduced to composition; ctest green.

---

## System-Wide Impact

- **Interaction graph:** SessionManager callbacks fan into recording/streaming/wifi/selection services — U1 changes the fan-out conditions; every consumer of `OnDisconnect` semantics re-validated. Dispatcher gains three handlers; telemetry gains health fields consumed by the app 1 Hz poll.
- **Error propagation:** start-class rejections become typed responses the app maps to gating UX; camera-failure finalize surfaces via last-session summary end-reason.
- **State lifecycle risks:** double-finalize races (auto-stop vs manual stop vs camera-failure) — all finalize paths must be idempotent through one gate. Group-stays-up changes WiFi teardown assumptions in `wifi-direct.handler` (redundant Start must not cycle the group — Argus killer).
- **API surface parity:** protocol version bump; app + MockBleService land the same fields (app plan U1/U7); emulator repo deferred.
- **Integration coverage:** connect→record→disconnect→reconnect→snapshot chain; health→gating→finalize chain — both covered as integration scenarios in U1–U3.
- **Unchanged invariants:** pull model (no firmware push); appsink 5-buffer cap and `MaterializeFrame` boundary; non-blocking sink pattern; `kSupportedVideoModes` as the single mode authority; watchdog restart serialization (`restart_mtx_`).

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| Session-model rework destabilizes the validated watchdog/recording paths | Characterization tests first (U1 execution note); metal checkpoint after U1–U3 before building on top |
| VIC postprocess conflicts with baked color-calibration stage | Keep OpenCV stage minimal rather than forcing full VIC; escape hatch env; A/B on metal against image-quality baselines |
| Health hysteresis mis-tuned → flapping or slow detection | Thresholds env-tunable; calibrate on metal against real ~50s stall traces |
| AF hunting during matches | Default off during recording; metal validation gates enabling it (origin R15) |
| Auto-stop vs reconnect race | Deterministic cancel-on-connect ordering, tested at timeout−ε (U1) |

---

## Open Questions

### Resolved During Planning

- Mid-recording camera death policy: hold one watchdog window, then finalize with camera-failure reason (user decision).
- Auto-stop default/ownership: app-configurable, 30 min default, rides session config (user decision).
- Group lifecycle across disconnect: stays up while session active — re-formation is the Argus killer.

### Deferred to Implementation

- Exact health thresholds (stall age, watchdog window multiple): env-tunable, calibrated on metal.
- Last-session summary persistence (memory-only vs small JSON surviving crash): decide when touching the config-dir code.
- Device-time apply mechanism (syscall vs bounded subprocess): decide at implementation against permissions on target.
- How much of the OpenCV color stage can move to VIC without breaking baked calibration: measured on metal.

---

## Sources & References

- **Origin document:** [docs/brainstorms/2026-07-06-state-health-quality-cycle-requirements.md](docs/brainstorms/2026-07-06-state-health-quality-cycle-requirements.md)
- Companion plans: `sst-cam-proto` + `sst-cam-app`, same date/seq.
- Prior plan (offload context): `docs/plans/2026-07-02-001-feat-capture-transfer-pipeline-plan.md`
- Learnings: see Context & Research above.
