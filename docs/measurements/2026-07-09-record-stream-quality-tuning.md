# Record + Stream Quality Tuning — On-Metal Measurement Note

**Plan:** `docs/plans/2026-07-09-001-feat-record-stream-quality-rework-plan.md` (U6)
**Rung:** beta (real Jetson `ssh sst@10.10.1.30` + app over USB adb; RTMP push to
`rtmp://10.10.1.96/live`). Hardware-bound — cannot be measured in the dev container.

## What shipped as the starting (committed) defaults

The quality profile (record + stream, NOT preview/proxy) drops `tune=zerolatency`
and gains B-frames + lookahead + a deepened leaky pre-encode queue. Committed
starting values, to be replaced by the measured slowest-sustainable point:

| Knob | Constant | Start value | Runtime dial (no rebuild) |
|------|----------|-------------|---------------------------|
| Record preset | `recorder-launch.cpp` `default_preset` | `superfast` | `SST_X264_PRESET` (global) |
| Stream preset | `rtmp-launch.cpp` `default_preset` | `superfast` | `SST_X264_PRESET` (global) |
| Record B-frames / lookahead | `kRecorderBframes` / `kRecorderRcLookahead` | `3` / `20` | — |
| Stream B-frames / lookahead | `kRtmpBframes` / `kRtmpRcLookahead` | `3` / `20` | — |
| Record bitrate | `kRecorderBitrateKbps` | `14000` | `SST_REC_BITRATE_KBPS` |
| Stream bitrate | `kDefaultBitrateKbps` | `14000` | `SST_STREAM_BITRATE_KBPS` |
| Record dip buffer | `kRecorderQueueMaxTimeMs` | `3000` ms | `SST_REC_QUEUE_MS` |
| Stream dip buffer | `kRtmpPreEncodeQueueMaxTimeMs` | `3000` ms | `SST_STREAM_QUEUE_MS` |
| Finalize timeout | `kFinalizeTimeoutSeconds` | `20` s | — (scales with buffer depth) |

## Measurement protocol (beta rung)

Run record + stream + live preview concurrently over a match-length interval, then:

1. **Sustained realtime** — encoder fps vs source fps; pre-encode queue backlog
   must not grow unbounded.
2. **CPU headroom** — total CPU% must stay below the ~249% Argus-starvation
   threshold; watch `journalctl -u sst-cam-firmware` for `INVALID_SETTINGS`
   (capture starvation, see the wifi-direct/argus watchdog learning).
3. **Egress** — `ss -tnp | grep :1935` (socket up) and
   `ffprobe rtmp://10.10.1.96/live/<key>` from a third box + visual check.
4. **Preset walk** — `ultrafast → superfast → veryfast/faster`; stop at the
   slowest preset that holds ≥1× realtime with no unbounded backlog and no Argus
   starvation. If 1080p×2 + preview can't hold at any acceptable preset, drop the
   **stream** to 720p (R10) via config (`cfg.width/height`) — record master stays
   1080p.
5. **Fix** `bframes` / `rc-lookahead` / bitrate / buffer-depth at the best-quality
   values inside the sustained-realtime envelope.
6. **Match-end flush (R7/U5)** — a full-match STOP must yield a playable MP4 with
   the buffered tail encoded (valid moov) within `kFinalizeTimeoutSeconds`; the
   live stream must never freeze during a record stop (`RecordingService::Push`
   stays `try_to_lock`-drop-frame).

## After sign-off

Replace the starting constants above with the measured values and capture the
final preset/bframe/lookahead/bitrate/buffer rationale as a `docs/solutions/`
learning via `/ce-compound` — cross-link
`software-h264-encode-ceiling-no-nvenc-2026-07-01.md`.
