---
title: "Cache telemetry on a background poll thread, not per-request on the BLE dispatcher"
date: 2026-06-30
category: architecture-patterns
module: control
problem_type: architecture_pattern
component: service_object
severity: high
applies_when:
  - "A telemetry/status handler runs inline on the single synchronous BLE dispatcher thread and needs a value that is slow to sample (shells out to `ip route`, `iw`, or reads `/proc`)"
  - "A component owns a std::thread whose lifecycle is guarded by a running_ flag plus a mutex (Start/Stop/destructor)"
  - "A proto scalar has no presence bit (sint32/int32, no `optional`) yet must distinguish a real reading from unknown"
  - "A probe parses stdout from a bounded/timeout-wrapped subprocess that can be SIGKILLed mid-write"
tags:
  - telemetry
  - background-poller
  - dispatcher
  - thread-lifecycle
  - subprocess
  - rssi
  - proto-sentinel
  - concurrency
---

# Cache telemetry on a background poll thread, not per-request on the BLE dispatcher

## Context

The firmware runs a **single BLE dispatcher thread** that handles every app command inline, including `GetTelemetry`. Three telemetry signals are slow to sample: internet uplink (`ip route`), WiFi peer RSSI (`iw dev … station dump`), and CPU% (a `/proc/stat` delta against a prior sample). If the handler forked those tools inline — once per telemetry request, on the dispatcher — a single hung tool would stall **every** BLE command behind it, and the device is controllable *only* over BLE, so that bricks the control surface.

This extends [bound-every-subprocess-on-the-dispatcher-thread](./bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md) (the "#42" learning). That doc bounds each inline fork with a wall-clock deadline + SIGKILL, and notes as a corollary: *"if the work can be slow and is not on the response's critical path, run it off-thread."* A recurring per-request fork **is** that case — a deadline caps the worst stall but you still pay it on every request. The fix is to move the fork off the dispatcher entirely and cache the result.

## Guidance

### 1. Background-poller: one owned thread samples into a cache; the handler only reads

`TelemetryProbe` owns a poll thread that samples the blocking signals on a fixed interval into a cache (an atomic for the bool, a mutex-guarded optional for the nullable RSSI). The request handler reads the cache instantly and never forks on its own thread.

```cpp
auto TelemetryProbe::Loop() -> void {
    std::unique_lock lock(run_mtx_);
    while (running_) {
        lock.unlock();
        SampleOnce();          // fork ip/iw here — off the BLE thread
        lock.lock();
        if (run_cv_.wait_for(lock, interval_, [this] { return !running_; })) {
            break;             // Stop() signalled; exit promptly
        }
    }
}
```

The dispatcher-side handler wires cheap getters (`telemetry_probe.InternetReachable()`, `.WifiSignalDbm()`) as provider callbacks — no fork, no blocking, ever.

### 2. std::thread lifecycle: assign `thread_` under the flag's lock; warm up *on* the thread

**Wrong** — set `running_` under the lock, release it, then assign `thread_`:

```cpp
{
    const std::lock_guard lock(run_mtx_);
    if (running_) return;
    running_ = true;
}                                          // lock released
SampleOnce();                              // synchronous warm-up: up to ~15s at boot
thread_ = std::thread([this] { Loop(); }); // assigned OUTSIDE the lock
```

The race: after the unlock, a concurrent `Stop()` (or the destructor) acquires `run_mtx_`, sees `running_ == true`, flips it false, and reaches its `thread_.joinable()` check **before** `thread_` is assigned — so it finds a default-constructed (non-joinable) thread and returns without joining. `Start()` then assigns a live, joinable `thread_` that nobody will ever join; when the object is destroyed, `~thread()` on a still-joinable thread calls **`std::terminate()`**, aborting the process at shutdown.

**Fixed** — establish the flag and the thread handle atomically, and move the first sample onto the loop thread:

```cpp
auto TelemetryProbe::Start() -> void {
    const std::lock_guard lock(run_mtx_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { Loop(); }); // assigned UNDER the lock
}
```

Now any `Stop()` that observes `running_ == true` also observes a joinable `thread_` and joins it. Warming up on the loop thread's first pass (§1) instead of synchronously in `Start()` also drops the up-to-15s startup block on a slow `ip`/`iw` at boot. See [non-blocking-sink-with-async-stop](./non-blocking-sink-with-async-stop-2026-06-10.md) for the broader swap-state-under-lock / drain-outside-lock lifecycle discipline this follows.

### 3. Proto3 scalar without a presence bit → domain sentinel

`wifi_signal_dbm` is a plain `sint32` with no presence bit, so `0` is ambiguous between "unset" and a real value — except a real RSSI is *always negative*, which frees `0` to mean "unknown". Leave the proto default untouched unless there is a real reading:

```cpp
// wifi_signal_dbm has no presence bit (plain sint32), and a real RSSI is
// always negative — leave the proto default 0 to mean "unknown" and only
// set it when we have a reading.
if (providers_.wifi_signal_dbm) {
    if (const std::optional<int> dbm = providers_.wifi_signal_dbm()) {
        telemetry->set_wifi_signal_dbm(*dbm);
    }
}
```

This is the same hazard as [proto-contract-logic-alignment](../logic-errors/proto-contract-logic-alignment-2026-06-09.md) (a proto3 default zero read as a real value). Prefer a `has_*()` check where a presence bit exists; where it does not, pick a domain-impossible sentinel and guard on the reader side.

### 4. Bounded-subprocess probe: gate parsing on `.ok`, trim both ends, range-check

A bounded run that times out is SIGKILLed mid-write, so its partial stdout must not be parsed. Gate on `CaptureResult.ok` before touching output — for **every** call:

```cpp
auto IwStationRssiProbe::SampleSignalDbm() -> std::optional<int> {
    const CaptureResult dev = CaptureBounded({"iw", "dev"}, kQueryTimeout);
    if (!dev.ok) return std::nullopt;                 // timed-out / non-zero → don't parse truncated output
    const std::optional<std::string> iface = ParseP2pGoInterface(dev.output);
    if (!iface) return std::nullopt;
    const CaptureResult dump =
        CaptureBounded({"iw", "dev", *iface, "station", "dump"}, kQueryTimeout);
    if (!dump.ok) return std::nullopt;
    return ParseStationSignalDbm(dump.output);
}
```

Parsing trims **both** line ends (a CRLF `\r` must not break an exact token match) and range-checks the result against the metric's real domain (a non-negative "RSSI" is garbage → unknown, not a bogus strong-signal reading). `CaptureResult.ok` is the flag introduced by #42's `CaptureBounded`; this closes the loop by actually checking it.

### 5. Group interchangeable ctor callbacks into a named struct

Four of `DeviceHandler`'s live-source callbacks share the `FlagProvider` type and were non-adjacent, so passing them positionally invited a **silent transposition** (report `is_streaming` where `is_recording` was meant — compiles clean). Group them into a named struct wired with designated initializers:

```cpp
struct Providers {
    FlagProvider      is_recording;
    FlagProvider      is_streaming;
    FlagProvider      is_raw_capturing;
    WifiStateProvider wifi_state;
    FlagProvider      internet_reachable;
    SignalProvider    wifi_signal_dbm;
};
DeviceHandler(sst::config::DeviceData device, ISystemStats& stats, Providers providers);
// call site: DeviceHandler{dev, stats, {.is_recording = ..., .wifi_signal_dbm = ...}}
```

## Why This Matters

1. **Inline fork on the dispatcher** → one hung `ip`/`iw` stalls *every* BLE command; the device's only control surface is BLE, so this bricks it.
2. **`thread_` assigned outside the flag lock** → `std::terminate()` / process abort at shutdown when a concurrent `Stop()` skips the join of a joinable-but-orphaned thread.
3. **Proto default `0` read as real** → the app shows a fake `0 dBm` (a *strong* signal) when the truth is "no peer / unknown".
4. **Parsing partial or CRLF stdout** → a mis-read metric from a truncated line, or an exact-token match silently failing on a `\r`.
5. **Positional interchangeable args** → silent flag transposition that compiles clean and ships the wrong telemetry bit.

## When to Apply

- **Any single-threaded event loop** needing subprocess-backed or otherwise blocking data per request — move the sample to an owned background poller with a cached result; the handler reads the cache.
- **Any `std::thread` member with idempotent `Start()`/`Stop()`** — establish the running flag and the thread handle under the *same* lock; warm up on the thread, not in `Start()`; join under the flag on `Stop()`.
- **Any proto3 scalar without a presence bit used as a nullable reading** — pick a domain-impossible sentinel, set the field only on a real reading, guard on the reader side.
- **Any bounded subprocess capture** — gate all parsing on the capture's `ok`/exit status; trim both line ends; range-check parsed values.
- **Any constructor taking multiple same-typed, non-adjacent callbacks** — group into a named struct and require designated initializers.

## Related

- Extends [bound-every-subprocess-on-the-dispatcher-thread](./bound-every-subprocess-on-the-dispatcher-thread-2026-06-29.md) — inline bounding stays correct for genuine one-shot forks; this poller is the answer for recurring per-request forks.
- Thread lifecycle mechanics: [non-blocking-sink-with-async-stop](./non-blocking-sink-with-async-stop-2026-06-10.md).
- Proto sentinel / presence: [proto-contract-logic-alignment](../logic-errors/proto-contract-logic-alignment-2026-06-09.md).
- See also (telemetry honesty from a misread result field): [httplib-bind-to-port-returns-bool-not-port](../logic-errors/httplib-bind-to-port-returns-bool-not-port-2026-06-30.md).

Key files: `src/app/control/services/telemetry_probe/telemetry-probe.{hpp,cpp}`, `src/adapters/control/network/iw-station-rssi-probe.cpp`, `src/app/control/services/handlers/device.handler.{hpp,cpp}`, `src/adapters/control/system/proc-system-stats.cpp`.
