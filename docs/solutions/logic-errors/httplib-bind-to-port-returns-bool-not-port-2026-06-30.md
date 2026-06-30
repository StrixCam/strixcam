---
title: "cpp-httplib bind_to_port returns a bool (1=success), not the bound port — don't store it as the port"
date: 2026-06-30
category: logic-errors
module: adapters
problem_type: bug
component: network_server
severity: medium
applies_when:
  - "Binding a cpp-httplib server with an explicit fixed port and recording the 'bound port' for logs/telemetry/callers"
  - "Mixing fixed-port binds with ephemeral (port 0) binds in the same Start() path"
  - "A test connects to a server via a BoundPort()/GetPort() accessor instead of the literal port it passed in"
tags:
  - httplib
  - bind_to_port
  - bind_to_any_port
  - download-server
  - ephemeral-port
  - api-misuse
  - off-by-api
---

## Problem

`HttpDownloadServer::Start()` stored the return value of cpp-httplib's
`Server::bind_to_port(host, port)` as the server's bound port. But
`bind_to_port` returns a **`bool`** (`true`/`1` on success, `false`/`0` on
failure) — **not** the port it bound. Only `bind_to_any_port(host)` (the
ephemeral bind, used when the caller passes port `0`) returns the actual
assigned port number. The code conflated the two APIs.

## Symptoms

- Logs and telemetry reported the download server "serving on
  `http://<addr>:1`" — the `1` was the truthy success return, not a real port.
- The fixed-port production path still worked end-to-end because the download
  URL is built from the **configured** port (`download_port_ = 8080`), not from
  the mis-stored `BoundPort()`. So real downloads succeeded with no error — the
  `:1` was a pure logging/telemetry artifact, easy to dismiss.
- The loopback HTTP test (`DownloadServerTest.HttpServesByteRangeWithBearerToken`)
  failed because it connects via `http.BoundPort()`, which returned `1`.

## What Didn't Work

- **Blaming qemu.** The test failure was first attributed to "qemu-user can't run
  the threaded accept loop / loopback binds port 1." That is a real, separate
  environment limitation for the *accept loop*, but it was the wrong diagnosis
  here: the test got `BoundPort() == 1` because of the API misuse, not the
  emulator. The tell was that the value was *always exactly 1* (a bool), never a
  plausible ephemeral port — an emulator scheduling issue would not pin the port
  to the integer `1`.

## Solution

Split the two bind APIs by intent and never treat `bind_to_port`'s return as a
port:

```cpp
if (port_ == 0) {
    // Ephemeral: bind_to_any_port DOES return the assigned port.
    const int assigned = server_->bind_to_any_port(bind_address_);
    if (assigned <= 0) { /* error */ return false; }
    bound_port_ = static_cast<std::uint16_t>(assigned);
} else {
    // Fixed port: bind_to_port returns 1=success / 0=failure — a bool.
    if (!server_->bind_to_port(bind_address_, port_)) { /* error */ return false; }
    bound_port_ = port_;   // the port we asked for IS the bound port
}
```

## Why This Works

`bind_to_port` and `bind_to_any_port` have different return contracts:
`bind_to_port` answers "did the bind succeed?" (bool); `bind_to_any_port`
answers "which port did the OS give me?" (the port, or ≤0 on failure). For a
fixed port the bound port is, by definition, the port you passed — there is no
need to read it back. For an ephemeral bind you *must* read it back, and only
`bind_to_any_port` provides it.

## Prevention

- **Read the return-type contract of any bind/connect call before storing its
  result as a number.** A function returning `bool` that you assign to a
  `uint16_t port` is a silent category error the compiler won't catch (implicit
  `1` → port `1`).
- **A value that is "always exactly 1" is a bool, not a measurement.** When a
  "port"/"count"/"id" reads as a constant `1` (or `0`), suspect a success-flag
  being read as a quantity before reaching for an environment explanation.
- **Make tests assert the bound port for the fixed-port branch.** A unit that
  passes a known fixed port and asserts `BoundPort() == <that port>` pins exactly
  this bug. (Skipped in the container suite to avoid fixed-port flakiness, but the
  invariant is worth a native/on-device assertion.)
- **Don't let a "works in production" path mask a wrong internal value.** Here the
  URL came from the config port, so downloads worked while `BoundPort()` was
  garbage. Wrong-but-unused state is still wrong and will bite the next consumer.
