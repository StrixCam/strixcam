---
title: Live streaming never started — two bugs (firmware stream_key contract + app kickoff missing destination)
date: 2026-07-09
category: integration-issues
module: streaming
problem_type: integration_issue
component: service_object
symptoms:
  - "App shows the match as live-streaming but nothing arrives at the RTMP ingest server; VLC/ffprobe read nothing"
  - "No Jetson TCP connection to the ingest host:1935 (ss shows no ESTAB to the RTMP port)"
  - "Firmware log: GstRtmpStreamer::Start rejected: url/stream_key required (stream_key=\"\")"
  - "Streaming started from the mid-match manual toggle works, but starting it from the kickoff modal (no recording, yes streaming) does nothing"
root_cause: logic_error
resolution_type: code_fix
severity: high
tags: [streaming, rtmp, gstreamer, session-actions, ble-contract, kickoff, jetson, app-firmware]
related_components: [gst-rtmp-streamer, rtmp-launch, session-actions, streaming-handler]
---

# Live streaming never started — two bugs (firmware stream_key contract + app kickoff missing destination)

## Problem

Live streaming to an external RTMP server (e.g. a self-hosted nginx-rtmp / mediamtx) never reached the server. Two independent bugs, one on each side of the app↔firmware boundary, each of which alone silently blocked the stream.

## Symptoms

- App reports "streaming" but the ingest server receives nothing; VLC/ffprobe on `rtmp://<host>/live/<key>` show nothing.
- On the Jetson, `ss -tnp | grep :1935` shows **no** established connection to the ingest host.
- Firmware log: `GstRtmpStreamer::Start rejected: url/stream_key required (... stream_key="" ...)`.
- The **manual mid-match stream toggle works**, but starting the stream from the **kickoff modal** ("no recording, yes streaming") does nothing.

## What Didn't Work

- Assuming "no firmware streaming log = command never arrived." `StreamingHandler` had **zero** logging on its reject paths (uplink/health/empty-destination), so a rejected start left no trace — the absence of logs was ambiguous, not proof. The real signal was `GstRtmpStreamer::Start`'s own error log once the command got that far.
- Suspecting the internet-uplink gate (`HasInternetUplink()` → `ip route show default`). The Jetson had a default route, so that gate passed; it was not the blocker even though it *also* rejects silently.
- Trusting the app UI ("streaming" = on). The app's `sendStartIfConnected` only surfaces `DEVICE_INOPERABLE` and **swallows generic ERROR** responses, so a firmware rejection flipped the UI to "streaming" while nothing egressed.

## Solution

**Bug 1 — firmware stream_key contract mismatch.** The app inlines the stream key into the URL (`joinRtmp(base, key)` → `rtmp://host/live/key`) and sends **no separate** `stream_key`. But the firmware required a non-empty `stream_key` and, when building the location, appended `"/" + key` — producing a trailing-slash URL (a different stream name on the ingest server) when the key was empty.

`src/adapters/streaming/gst_rtmp/gst-rtmp-streamer.cpp` — require only `url`:

```cpp
// before
if (config.url.empty() || config.stream_key.empty()) {
    spdlog::error("GstRtmpStreamer::Start rejected: url/stream_key required ({})", config);
    return false;
}
// after — key is optional (the app inlines it into the URL)
if (config.url.empty()) {
    spdlog::error("GstRtmpStreamer::Start rejected: url required ({})", config);
    return false;
}
```

`src/adapters/streaming/gst_rtmp/rtmp-launch.cpp` `BuildRtmpLocation` — empty key means the URL is already complete:

```cpp
// The app inlines the key into the URL and sends no separate key, so an empty
// stream_key means `url` is the complete ingest location — use it verbatim.
// Appending "/" here would push to "<url>/", a DIFFERENT stream on the server.
if (cfg.stream_key.empty()) {
    return cfg.url;
}
// (existing: url.empty() -> key; url ends "/" -> url+key; else url+"/"+key)
```

**Bug 2 — app kickoff omits the destination.** The mid-match toggle resolved the per-match destination and passed `rtmpUrl`; the kickoff path sent the START command with only `quality` and **no `rtmpUrl`** — so the firmware saw an empty destination and rejected `"no destination provided or configured"`.

`lib/features/match/session/session_actions.dart` — extracted the toggle's resolution into a shared `resolveStreamWireUrl(context, ref)` and used it in kickoff:

```dart
// kickoff — before: StreamingControlCommand(action: start, quality: ...)  // no rtmpUrl!
// after:
var streamingStarted = false;
if (choice.$2 == true) {
  final wireUrl = await resolveStreamWireUrl(context, ref);
  if (wireUrl != null && context.mounted) {
    streamingStarted = sendStartIfConnected(context, ref,
      StreamingControlCommand(action: StreamingControlAction.start,
        rtmpUrl: wireUrl, quality: state.streamQuality));
  }
}
// reflect the ACTUAL started state, not the prompt choice (no UI desync)
ctl.startPeriod(startRecording: choice.$1, startStreaming: streamingStarted);
```

Metal-confirmed after both fixes: the Jetson pushes `rtmp://10.10.1.96/live/test`, `ss` shows the ESTAB to `:1935`, and `ffprobe` from the PC reads h264 1080p30 + aac.

## Why This Works

The streaming START is a cross-boundary contract: the app owns the destination format (key inlined in the URL) and must ship the URL on every START; the firmware must treat the key as optional and use the URL as-is. Bug 1 broke the firmware's half of that contract (demanded a field the app never sends); Bug 2 broke the app's half (a START path that forgot to attach the URL). Either alone yields an empty/invalid destination → the streamer rejects → nothing egresses.

## Prevention

- **Ground-truth tools, in order:** firmware `journalctl -u sst-cam-firmware` for `GstRtmpStreamer::Start` / `StartPlatformStream` (add logging to *reject* paths so failures aren't silent); `ss -tnp | grep :1935` on the Jetson to see if it's actually pushing; `ffprobe rtmp://host/live/key` from a third box to read the stream back (proves the whole path independent of VLC quirks).
- **Surface generic ERROR responses in the app**, not just `DEVICE_INOPERABLE` — a swallowed ERROR is exactly what made this invisible (open follow-up in `sendStartIfConnected`).
- **One destination-resolution path** (`resolveStreamWireUrl`) shared by every START call site, so no path can dispatch a START without the URL.
- Test `RtmpLaunchTest.LocationUsesUrlVerbatimWhenKeyInlined` pins the empty-key behavior (URL verbatim, no trailing slash).

## Related Issues

- `docs/solutions/integration-issues/camera-undiscoverable-ble-after-connect-2026-07-09.md` — same metal session; app↔firmware BLE/WiFi contract debugging with the same ground-truth-tooling lesson.
- `docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md` — the software-encode context behind streaming (no NVENC; the follow-up quality rework builds on it).
