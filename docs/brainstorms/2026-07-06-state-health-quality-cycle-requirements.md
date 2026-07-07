---
date: 2026-07-06
topic: state-health-quality-cycle
---

# State Model, Device Health & Whole-Codebase Quality Cycle

## Summary

Full quality cycle across firmware and app on the active `feat/capture-transfer-pipeline` branches: a whole-codebase polish pass with the known bugs fixed as part of it, anchored by one explicit app⇄firmware state-ownership model (reconnect handshake, silent rejoin, unsupervised auto-stop), per-camera health gating with a diagnostics screen, a fresh root-cause of the "both"-mode preview flicker, and the CPU-encode offload that cures Argus starvation. Work lands subsystem-by-subsystem, each sweep metal-validated.

---

## Problem Frame

The app and firmware each manage their own state independently — preview mode, selected camera, and session state are never reconciled. Every connect path behaves differently (first connect, manual disconnect/connect, app kill and reopen, camera reboot), and the only reliable recovery today is killing the app or rebooting the camera. It is currently unknown what happens to a recording when the app disconnects mid-match, whether the app learns on reconnect that the camera is mid-session, or how scoreboard state survives an app restart mid-game.

On top of that: the "both"-mode live preview still occasionally flashes a single-camera frame even with the prior flicker fixes deployed; a camera detection failure produces no clear error; there is no per-element health visibility, so a dead camera fails silently instead of gating the features that depend on it; and a recurring ~50s Argus `INVALID_SETTINGS` starvation is currently only masked by the capture watchdog. Both codebases have accumulated structure debt across a fast-moving branch (41 firmware / 31 app commits ahead of release).

---

## Actors

- A1. Operator: records matches with the dual-camera device; connects/disconnects the phone app freely mid-game.
- A2. App (phone): ultimate source of truth — owns desired state (preview mode, selected camera, settings, start/stop commands) and match data (scoreboard, teams, score).
- A3. Firmware (Jetson): temporary source of truth — owns actual capture state (recording/streaming running, session identity, elapsed time, per-element health) and keeps executing unsupervised.

---

## Key Flows

- F1. Universal connect handshake
  - **Trigger:** Any app→camera connection (first connect, manual reconnect, app reopen, camera reboot).
  - **Actors:** A2, A3
  - **Steps:** App connects → reads firmware actual state (session running? which mode/camera/settings? element health?) → rehydrates UI from actual state plus app-persisted match data → pushes any desired-state diff.
  - **Outcome:** App UI reflects reality; no stale or divergent state on either side. All connect paths are the same code path.
  - **Covered by:** R2, R3, R4

- F2. Mid-match app disconnect
  - **Trigger:** App disconnects (user action, app kill, WiFi blip) while recording/streaming is active.
  - **Actors:** A1, A2, A3
  - **Steps:** Firmware continues session unsupervised → operator reopens app and connects → handshake (F1) finds running session → app silently rejoins: shows in-progress state, restores scoreboard from local storage.
  - **Outcome:** No footage lost; operator continues as if never disconnected. If no app returns within the auto-stop window, firmware ends the session cleanly.
  - **Covered by:** R1, R3, R4, R5

- F3. Camera failure gating
  - **Trigger:** A camera fails detection or stops delivering frames (at boot or mid-operation).
  - **Actors:** A2, A3
  - **Steps:** Firmware health marks the camera down (frame-truth) → reports to app → app surfaces "device inoperable" error → blocks live preview, recording, streaming → diagnostics screen shows per-element status.
  - **Outcome:** Operator sees exactly what is broken; only downloading previous matches remains available.
  - **Covered by:** R6, R7, R8, R9

---

## Requirements

**State model & reconnect**
- R1. State ownership is explicit: app owns desired state (preview mode, selected camera, settings, commands) and match data (scoreboard, teams, score); firmware owns actual capture state (recording/streaming running, session identity, elapsed time, health) and keeps executing its last command through app disconnects.
- R2. Every connect path uses one identical handshake: read firmware actual state → rehydrate app UI → reconcile desired state. No special-case logic per connect scenario.
- R3. When the handshake finds a session running, the app silently rejoins it — no prompt; UI shows recording/streaming in progress.
- R4. Match/scoreboard data persists in app-local storage and survives app kill/restart; on rejoin, the scoreboard is restored where it left off.
- R5. Firmware auto-stops an unsupervised session after a prolonged period with no app connection — a safety net against a forgotten camera filling the disk. Timeout is configurable and generous enough that a mid-game disconnect never kills a recording.

**Device health & gating**
- R6. Firmware tracks per-camera health that is frame-truth-based: a pipeline that reports running but delivers no frames counts as camera down (closes the known silent-stall gap).
- R7. A camera detection or health failure surfaces in the app as an explicit "device inoperable" error — not a silent blank preview.
- R8. When ANY camera is down, the app blocks live preview, recording, and streaming; downloading previous matches remains available.
- R9. The diagnostics screen shows a live status indicator per camera, plus per-microphone indicators as placeholders permanently showing offline (mic hardware unsupported).

**Known bugs**
- R10. The "both"-mode preview flicker (occasional full single-camera frame) is root-caused and fixed. Existing fixes (hold-don't-switch, cadence-follows-selection) are deployed and insufficient — this needs fresh on-metal investigation, not a guessed fix.
- R11. The recurring ~50s Argus `INVALID_SETTINGS` starvation gets its real cure — CPU-encode offload per the capture-transfer-pipeline direction — instead of relying on the watchdog restart as a mask.

**Capability additions**
- R15. Continuous autofocus ships this cycle: firmware drives the motorized focuser from a sharpness measure automatically; manual focus control remains. AF defaults off during recording unless on-metal validation proves it stable (focus hunting mid-match is worse than fixed focus).
- R16. Wall-clock sync ships this cycle: the app pushes phone time to the firmware at connect so device timestamps (file times, session summaries) are trustworthy; firmware syncs via NTP opportunistically when an uplink exists. Authoritative session/match clocks on the wire remain monotonic/relative.
- R17. BLE auto-reconnect ships this cycle: the app automatically reconnects to the last-connected camera after an unexpected drop; manual connect remains for first-time and different-device cases. Auto-reconnect only ever re-targets the last-connected device.

**Codebase quality**
- R12. Whole-codebase structural polish on both repos: organization, duplication removal, dead code, compaction — brought up to standard, not just branch-touched files.
- R13. A broad bug/optimization review runs inside each subsystem sweep (no separate global review pass); findings are fixed in that sweep.
- R14. Work is structured as an up-front state/health model design (spanning proto wire messages, firmware, app), then subsystem-by-subsystem fix+polish sweeps; each subsystem is metal-validated before moving on, and no PRs are opened until the cycle is complete and validated.

---

## Acceptance Examples

- AE1. **Covers R1, R3, R4.** Given a match is recording and the scoreboard reads 2–1, when the app is killed and reopened 5 minutes later and connect is hit, the app shows the recording still in progress with the scoreboard at 2–1, and the resulting video file contains the full uninterrupted match.
- AE2. **Covers R2.** Given the camera was powered off and back on while the app stayed open, when connect is hit, the app rehydrates from firmware's fresh (idle) state — same handshake as a first connect, no stale preview mode or camera selection from before the reboot.
- AE3. **Covers R2.** Given the operator hits disconnect then connect (manual reconnect), when the handshake completes, preview mode and selected camera match what the firmware is actually doing — not defaults, not the app's stale memory.
- AE4. **Covers R5.** Given a session is recording and no app connects for the full auto-stop window, when the timeout elapses, the firmware ends the session cleanly and the recording is a valid, downloadable file.
- AE5. **Covers R6, R7, R8.** Given one camera stops delivering frames mid-preview, when the firmware's frame-truth health marks it down, the app shows a "device inoperable" error, disables preview/record/stream, and still allows downloading previous matches.
- AE6. **Covers R9.** Given the diagnostics screen is open with both cameras healthy, the operator sees two camera indicators as online and the microphone indicators as offline placeholders.
- AE7. **Covers R10.** Given "both" (side-by-side) preview mode is active for a full match length, no single-camera frame ever appears in the preview.

---

## Success Criteria

- The operator's recovery ritual (kill app / reboot camera) is dead: any of the four connection scenarios ends in a correct, fully-rehydrated UI within one connect.
- A mid-game phone disconnect provably loses zero footage and zero scoreboard state.
- A dead camera is impossible to miss (explicit error + diagnostics) and impossible to record through (gating).
- "Both"-mode preview runs a full match with zero flicker on metal.
- Argus starvation no longer recurs under sustained recording (offload in place, watchdog becomes true last-resort).
- Both codebases pass their lint/tidy gates and a reviewer can navigate either repo without tribal knowledge; ce-plan can pick this doc up without inventing product behavior.

---

## Scope Boundaries

- Real microphone support — indicators are placeholders only, permanently offline this cycle.
- Degraded single-camera operation — rejected: any camera down blocks everything.
- sst-cam-emulator repo polish — not part of this cycle.
- New feature work beyond the listed bugs, health/state model, and polish.

---

## Key Decisions

- App = ultimate source of truth, firmware = temporary source of truth: app owns intent and match data; firmware owns runtime capture facts and operates unsupervised. Chosen over camera-stops-on-disconnect because a WiFi blip must never destroy a match recording.
- Silent rejoin over prompt-on-reconnect: zero-friction mid-game beats explicit control; the handshake makes rejoin safe.
- Block-everything gating over degraded single-camera mode: simpler and matches the operator's mental model of "device is broken."
- One handshake for all connect paths: kills the four scenario-specific bugs as a class instead of patching each.
- Auto-stop safety net added (reversing earlier keep-going-forever choice): protects disk/battery from a forgotten camera; generous timeout protects mid-game disconnects.
- Fix + polish per subsystem sweep (approach C with an architecture-first head): every merge point is validated and polished once; a trailing global polish pass would re-review everything twice.
- Auto-stop is an app-configurable setting with a 30-minute default (resolves the deferred timeout question): tight disk/thermal protection by default, adjustable for operators who leave the phone away longer.
- Mid-recording camera failure = hold-then-finalize: the camera is marked "recovering" for one watchdog-restart window; if it recovers the match continues, otherwise the firmware finalizes the recording cleanly and ends the session with end-reason = camera failure. Protects footage without letting transient stalls kill matches.

---

## Dependencies / Assumptions

- State/health handshake needs new proto wire messages — sst-cam-proto is in scope for the design unit.
- Jetson + phone access required throughout: flicker root-cause, handshake scenarios, health gating, and offload all need on-metal validation (workspace norm: metal validation gates PRs).
- Existing capture-transfer-pipeline plan (`docs/plans/2026-07-02-001-feat-capture-transfer-pipeline-plan.md`, firmware repo) already frames the encode-offload direction; R11 completes it rather than re-deciding it.
- Branch state: 41 firmware / 31 app commits ahead of release on `feat/capture-transfer-pipeline`; this cycle continues on these branches.

---

## Outstanding Questions

### Deferred to Planning

- [Affects R10][Needs research] Root cause of the surviving "both"-mode flicker — prior fixes addressed decision cross-switch and cadence; the remaining single-frame flash needs on-metal investigation (compositor path? pane fallback on a missed frame?).
- [Affects R5][Technical] Auto-stop timeout default and where it's configured (firmware config vs app setting).
- [Affects R6][Technical] Frame-truth health mechanics: N-consecutive-missed-frames threshold vs frame-timestamp watchdog, and how health propagates over the wire.
- [Affects R12][Technical] Subsystem slicing for the sweeps (connection/session, capture pipeline, diagnostics/health, storage/downloads, remainder) — finalize per repo at plan time.
