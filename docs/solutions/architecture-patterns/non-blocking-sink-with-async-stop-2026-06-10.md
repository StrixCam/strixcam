---
title: "Non-blocking sink with an async Stop: try_to_lock the producer, swap state out under the lock and drain outside it"
date: 2026-06-10
last_updated: 2026-07-01
category: architecture-patterns
module: adapters
problem_type: architecture_pattern
component: service_object
severity: high
applies_when:
  - "A high-rate producer thread and a control thread share an adapter's state (GStreamer sinks/streamers, recorders, any IFrameSink with start/stop)"
  - "A port contract says the producer (Push/PushCamera) must never block the capture/encode path"
  - "Stop/reconnect must join threads or run a blocking GStreamer state change while the pipeline is still pushing"
  - "A lifecycle op holds the producer's mutex across a slow finalize (mp4mux moov flush, thread join, GST_STATE_NULL teardown)"
tags:
  - concurrency
  - data-race
  - use-after-free
  - mutex
  - try_to_lock
  - gstreamer
  - non-blocking
  - recorder
  - mp4
  - moov
  - finalize
  - fan-out
related_components:
  - filesystem-raw-capture-sink
  - gst-rtmp-streamer
  - recording-service
  - pipeline-orchestrator
---

# Non-blocking sink with an async Stop: try_to_lock the producer, swap state out under the lock and drain outside it

## Context

Output adapters on the capture hot path (`FilesystemRawCaptureSink`, `GstRtmpStreamer`,
the recorder) have two callers on different threads:

- **Producer** — a pipeline worker thread calling `Push`/`PushCamera` at the capture rate.
  Raw NV12 1080p30 is ~93 MB/s per camera, so the port contract is explicit: the producer
  **must never block** the capture/encode path (it drops under backpressure instead).
- **Control** — a BLE handler thread calling `Stop` (or a watcher thread reconnecting),
  which mutates shared state: clears the writer vector, or tears the GStreamer pipeline
  down to `GST_STATE_NULL`.

Both fixes in the raw-capture review were the same shape, and so is the wrong way to
"fix" it — this is the durable pattern, not the one-off.

## Guidance

Two rules, together:

1. **Producer acquires the lock with `try_to_lock` and drops the frame if it can't.**
   ```cpp
   std::unique_lock lock(mtx_, std::try_to_lock);
   if (!lock.owns_lock() || !running_.load() || idx >= writers_.size()) return;
   writers_[idx]->ring.Push(frame);   // the ring is itself lock-free / drop-oldest
   ```
   Dropping a frame when the lock is contended is **correct**, not a compromise — it is the
   same drop-oldest discipline the bounded queue already applies. This keeps the hot path
   strictly non-blocking *and* race-free, no matter how long the control thread holds the lock.

2. **Stop holds the lock only for an O(1) state swap; the slow work runs outside it.**
   ```cpp
   std::vector<std::unique_ptr<CameraWriter>> draining;
   {
     std::lock_guard lock(mtx_);
     if (!running_.load()) return false;
     running_.store(false);
     draining = std::move(writers_);   // O(1) move; member is now empty
   }
   for (auto& w : draining) { w->Close(); w->thread.join(); /* drain, flush, close */ }
   ```
   After the swap, a concurrent `Push` sees `running_ == false` / empty `writers_` and
   no-ops. The joins and blocking teardown never happen under a lock the producer wants.

Corollary: **an atomic flag is not a substitute for the lock.** A lock-free
`if (capturing_.load()) writers_[i]...` reads the *flag* with visibility, but establishes
no happens-before with another thread that *frees* `writers_` under the mutex. That gap is
a use-after-free.

## Why This Matters

The naive non-blocking version (lock-free read guarded only by an atomic) and the naive
safe version (producer takes the mutex normally) are *both* wrong in opposite ways:

- Lock-free read + `Stop` clearing the vector under the lock → **use-after-free** when an
  operator taps Stop mid-capture. Intermittent, and invisible to a single-threaded test
  teardown (process destroys the pipeline before the sink, so no concurrent Push) — it
  only bites on hardware.
- Producer takes the mutex normally → `Stop`/reconnect holds it across a thread join or a
  blocking `GST_STATE_NULL` socket teardown → the producer **stalls**, which stalls the
  shared fan-out and degrades supposedly-independent outputs (an RTMP reconnect freezing
  the RTSP preview).

`try_to_lock` + swap-out-then-drain is the only structure that is both non-blocking and
memory-safe.

## When to Apply

Any adapter where a real-time producer thread and a lifecycle method (`Stop`, reconnect,
teardown) touch the same state behind a mutex. In this codebase: every `IFrameSink` /
`IRawCaptureSink` / `IPlatformStreamer` implementation, the continuous recorder, and any
future GStreamer adapter.

## Examples

**`FilesystemRawCaptureSink` — the use-after-free.** `PushCamera` read `writers_` lock-free
(guarded only by the `capturing_` atomic) while `Stop()` did `writers_.clear()` under the
lock. Fixed with rule 1 (`try_to_lock` in `PushCamera`) + rule 2 (`Stop` moves `writers_`
into a local and joins/drains outside the lock).

**`GstRtmpStreamer` — the fan-out stall.** The reconnect watcher rebuilds the pipeline under
`mtx_`, including a blocking `GST_STATE_NULL` teardown. `Push` taking `mtx_` normally would
block `StreamingService::Push` (and the RTSP branch sharing that fan-out) for the whole
reconnect. Rule 1 (`try_to_lock` in `Push`, drop the frame while the uplink is down — there
is nowhere to send it anyway) decouples them; the reconnect also got capped exponential
backoff so a permanently-down endpoint can't spin into a rebuild storm.

**`RecordingService::Push` — the finalize freeze (2026-07-01).** This is the same stall
as the streamer, on the recorder this doc already named as an apply-here target — and it
still shipped a blocking `lock_guard`. `RecordingService::Push` runs on the shared
`ConsumerLoop` fan-out thread (the very next thing that thread does is feed the live stream
sink), and `Stop()` holds `mtx_` for the **entire** `mp4mux` moov-flush finalize wait
(`gst_bus_timed_pop_filtered`, `kFinalizeTimeoutSeconds`, widened 5s→10s when a leaky
pre-encoder queue was added). So every stop-recording froze the live RTMP/RTSP stream for
up to 10s. Fixed with Rule 1 — `try_to_lock` in `Push`, drop the frame (recording is ending
anyway), mirroring `GstRtmpStreamer::Push`. (Rule 2 wasn't enough on its own here: `Stop`'s
slow work *is* the finalize wait, which must stay under the lock so a concurrent `Push`
can't feed a half-torn-down muxer — so the producer side must be the non-blocking one.)
The finalize-timeout → corrupt-`moov` seam is also how a too-slow software encode ruins a
file; see the software-encode-ceiling note under `tooling-decisions/`.

## Prevention

- The container test suite tears down single-threaded (pipeline destroyed before the sink),
  so it never exercises concurrent `Push`-vs-`Stop`. Add a stress test that spawns a
  producer hammering `PushCamera` while another thread calls `Stop`, run under TSan where
  available; this is the case that only reproduces on-device otherwise.
- Treat "must not block the hot path" and "Stop mutates shared state" as a paired
  constraint: whenever both are true, reach for `try_to_lock` producer + swap-out-then-drain
  Stop, not a bare atomic and not a held-across-teardown mutex.
- **Naming a target isn't auditing it.** This doc listed "the continuous recorder" under
  *When to Apply* on 2026-06-10, yet `RecordingService::Push` still shipped a blocking
  `lock_guard` and froze the live stream on 2026-07-01. Turn the list into an executed
  check: `grep` every `IFrameSink` / `IRawCaptureSink` / `IPlatformStreamer` `Push`/
  `PushCamera` for a bare `std::lock_guard` (as opposed to `std::unique_lock` +
  `std::try_to_lock`), and confirm each one's `Stop`/reconnect can't hold that mutex across
  a slow op. A producer that shares a fan-out thread with another live sink is the acute case.

## Related Issues

- App-side and firmware fixes from the same review: `sst-cam-firmware@6c08bff`,
  `sst-cam-app@fbb03d7`.
- The mock-fidelity angle of the same review (why the green suite hid these):
  `sst-cam-app` `docs/solutions/architecture-patterns/mock-must-mirror-real-firmware-contract-2026-06-10.md`
