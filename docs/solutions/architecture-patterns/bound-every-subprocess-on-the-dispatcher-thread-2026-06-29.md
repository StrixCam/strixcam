---
title: "Bound every subprocess (fork/exec, popen) on the single BLE dispatcher thread with a wall-clock deadline"
date: 2026-06-29
category: architecture-patterns
module: adapters
problem_type: architecture_pattern
component: service_object
severity: high
applies_when:
  - "An adapter shells out to a system tool (nmcli, ip, wpa_cli, ffprobe, …) to read or change device state"
  - "The call runs on the single synchronous BLE CommandDispatcher thread (every control command is handled inline on one thread)"
  - "The subprocess can legitimately block for a long time or hang (NetworkManager mid-restart, DHCP wait, saturated D-Bus, a wedged daemon)"
tags:
  - subprocess
  - nmcli
  - timeout
  - ble
  - dispatcher
  - reliability
  - fork-exec
  - popen
---

## Problem

The uplink adapters drove `nmcli`/`ip` with `::waitpid(pid, &status, 0)` (blocking,
no `WNOHANG`) and `popen()` + `fgets()` read loops with no deadline. Every BLE
control command is dispatched **synchronously on one thread**
(`CommandDispatcher::Dispatch` calls the handler inline). So a single hung
`nmcli con up` — NetworkManager restarting, a DHCP server that never answers,
single-radio GO+STA contention wedging NM — stalls **every** BLE command
(recording, preview, streaming, match control), not just the network handler.
`ApplyEthernet` makes two `nmcli` calls plus two captures per `SetNetworkConfig`,
and the boot path called `Apply` synchronously **before** BLE started, so a hang
at boot could leave the camera with no BLE at all.

This is invisible in unit tests (a `FakeConfigurator` never execs) and on a happy
network on the bench. It only bites in the field, which is exactly where it is
unrecoverable.

## Fix

Wrap every exec in a bounded helper that enforces a wall-clock deadline and
SIGKILLs + reaps the child on expiry, returning failure instead of hanging.
See `src/adapters/control/network/subprocess.{hpp,cpp}` (`RunBounded`,
`CaptureBounded`):

- **State change** (`waitpid`): loop `waitpid(pid, &status, WNOHANG)` + short
  `nanosleep` until a generous deadline (~45 s — `con up` legitimately waits on
  NM + DHCP), then `kill(pid, SIGKILL)` + reap, return false.
- **Read query** (was `popen`): fork/execvp into a pipe, read bounded by
  `poll()` with a short deadline (~5 s); on timeout SIGKILL + reap, `ok = false`.
- Use a **fork/exec argv vector, never a shell** — this also closes the
  command-injection surface for any BLE-supplied tokens (IP/gateway/DNS) for free.
- **Boot-time apply runs on a detached thread** so the BLE/WiFi-Direct layer
  comes up regardless of how long (or whether) the uplink settles.

Pick the deadline per call class (long for activation that waits on the network,
short for read-only discovery). A timeout returning "uplink down" is the correct,
honest degradation — far better than a frozen control plane.

## Why it matters / how to apply

Any new adapter that execs a tool on the dispatcher thread inherits this hazard.
Before adding one: bound it with a deadline, never block unboundedly, and prefer
the shared `subprocess.{hpp,cpp}` helpers over a fresh `popen`. If the work can be
slow and is not on the response's critical path, run it off-thread (detached) like
the boot apply. For a **recurring per-request** fork (e.g. telemetry sampling `ip
route`/`iw` on every `GetTelemetry`), bounding the inline call is not enough — move
it to a background poller that caches off-thread: see
[[telemetry-probe-background-poller-off-the-dispatcher-thread-2026-06-30]].
Related: [[non-blocking-sink-with-async-stop-2026-06-10]] (the
same "never block the hot path" discipline for the capture/encode side).
