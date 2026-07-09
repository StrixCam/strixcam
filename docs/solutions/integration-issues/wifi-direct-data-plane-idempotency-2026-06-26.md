---
title: "WiFi-Direct start path must be idempotent — cleanup never runs on abrupt BLE drop"
date: 2026-06-26
category: integration-issues
module: src/adapters/control/wifi/wpa_supplicant
problem_type: integration_issue
component: tooling
severity: high
symptoms:
  - "First preview after a fresh firmware boot works; every preview after fails"
  - "App shows 'wifi failed' / hero card 'WIFI · FAILED'; telemetry may still say AP-ready"
  - "journal: 'WpaWifiManager: no P2P-GROUP-STARTED event received'"
  - "journal: 'P2P_GROUP_ADD failed: <3>WPS-AP-AVAILABLE'"
  - "journal: 'Error: ipv4: Address already assigned.' after StartP2pGroupOwner succeeds"
  - "journal: 'DnsmasqDhcpServer: already running (pid=...)' then app sees 'failed to start DHCP'"
root_cause: logic_error
resolution_type: code_fix
tags: wifi-direct, p2p, wpa_supplicant, dnsmasq, iproute2, idempotency, session-cleanup, jetson, ble
---

# WiFi-Direct start path must be idempotent — cleanup never runs on abrupt BLE drop

## Problem

The WiFi-Direct data plane (P2P group owner + GO IP + dnsmasq) worked on the
first `StartWifiDirect` after a fresh boot, then failed on every subsequent
preview. Each layer of the start path assumed it ran from a clean slate, but the
slate is almost never clean: the only thing that tears the data plane down is
`SessionCleanup → wifi_.Stop()`, and that does **not** run when the phone drops
the BLE link abruptly (app backgrounded, walked out of range, killed). The next
`StartWifiDirect` then collides with the artifacts the previous session left
behind.

## Symptoms

Three distinct failures, one per layer, surfacing as the work progressed (fixing
one revealed the next):

1. `P2P_GROUP_ADD failed: <3>WPS-AP-AVAILABLE` / `no P2P-GROUP-STARTED event
   received` — the radio was already a P2P-GO from the prior session, so a second
   `P2P_GROUP_ADD` never forms a new group.
2. `Error: ipv4: Address already assigned.` — `ip addr add 192.168.49.1/24`
   fails because the address is still bound to the group interface.
3. `DnsmasqDhcpServer: already running (pid=...)` → the handler returns
   `false` → the app receives `failed to start DHCP on the WiFi Direct group`.

All three are the **same bug**: a start step that is not idempotent.

## What Didn't Work

- **Assuming systemd would clean up.** `KillMode=control-group` reaps the forked
  dnsmasq on a *graceful* `systemctl restart`, but not the wpa_supplicant P2P-GO
  (that lives in the daemon, not our cgroup) and not an *abrupt* app disconnect
  (no restart happens at all).
- **Relying on `Stop()` / session cleanup.** It only runs on a clean
  `StopWifiDirect`. The real-world path (BLE link just dies) skips it entirely.
- **Fixing one layer at a time.** Making the group form again only exposed the
  IP-already-assigned failure, which only exposed the dnsmasq-already-running
  failure. The class of bug — not the individual symptom — had to be addressed.

## Solution

Make every start step self-heal. The start path must produce a working data
plane regardless of what the previous (uncleaned) session left behind.

1. **P2P group — remove before add** (`wpa-wifi-manager.cpp`, `StartP2pGroupOwner`):

   ```cpp
   if (!OpenCtrlSocket()) return std::nullopt;
   // Tear down any lingering group BEFORE ATTACH so the reply is not interleaved
   // with the unsolicited P2P-GROUP-REMOVED event it triggers. Best-effort: OK
   // (removed) and FAIL (none present) are both fine.
   SendCommand("P2P_GROUP_REMOVE *");
   SendCommand("ATTACH");
   auto reply = SendCommand("P2P_GROUP_ADD");
   ```

   Order matters: `P2P_GROUP_REMOVE *` runs **before** `ATTACH`. wpa_supplicant
   only delivers unsolicited events to ATTACHed monitor sockets, so before ATTACH
   the socket sees only the command reply — the resulting `P2P-GROUP-REMOVED`
   event cannot pollute a later command's reply.

2. **GO IP — `replace`, not `add`** (`ip-command.cpp`):

   ```cpp
   // `ip addr replace` is idempotent: it adds when absent and is a no-op success
   // when already present, so a lingering GO address no longer fails the call.
   return {"ip", "addr", "replace", cidr, "dev", iface};
   ```

3. **dnsmasq — restart, don't refuse** (`dnsmasq-dhcp-server.cpp`, `Start`):

   ```cpp
   if (pid_ > 0) {
     spdlog::info("DnsmasqDhcpServer: restarting (prior pid={})", pid_);
     Stop();              // SIGTERM + waitpid, sets pid_ = -1
   }
   ```

   Replaces the old `if (pid_ > 0) { warn("already running"); return false; }`
   that surfaced to the app as a DHCP failure.

Supporting fixes from the same bring-up (separate but required for the plane to
work at all): the systemd unit needs `AmbientCapabilities=CAP_NET_ADMIN
CAP_NET_BIND_SERVICE CAP_NET_RAW` (assign IP, bind DHCP :67, dnsmasq raw socket),
`--leasefile-ro` for dnsmasq (non-root service can't write `/var/lib/misc`), and
the wifi interface must be auto-detected (`wlP1p1s0`, not hardcoded `wlan0`).

A second, subtler fix in `SendCommand`: after `ATTACH`, skip `<priority>`-tagged
unsolicited events so a `<3>WPS-AP-AVAILABLE` is never mistaken for a command
reply (`IsWpaUnsolicitedEvent` predicate, unit-tested).

## Why This Works

The lifecycle guarantee the original code assumed — "cleanup ran, so I start
clean" — does not hold on real hardware, because the dominant teardown trigger
(BLE disconnect) is exactly the one that skips cleanup. Idempotent start steps
remove the dependency on cleanup entirely: each step converges to the desired
state whether or not the prior session was torn down. `P2P_GROUP_REMOVE *`,
`ip addr replace`, and `Stop()`-then-fork each turn "fail if dirty" into
"converge regardless."

## Prevention

- **Treat any externally-observable resource a service acquires (kernel P2P
  group, interface address, child daemon, bound port) as potentially leftover on
  the next acquire.** Make acquisition idempotent rather than assuming a matching
  release ran.
- **Never assume session cleanup runs.** The transport that triggers cleanup
  (here BLE) can vanish without notice. The start path is the only code
  guaranteed to run.
- **Factor the impure step into a pure builder and unit-test it** — `ip-command`
  (`BuildAddrReplaceArgv`) and `IsWpaUnsolicitedEvent` are pure and tested in the
  container; the fork/exec wrapper is verified on-device.
- **Known remaining gap (tracked):** a firmware *crash* (SIGKILL/OOM) resets the
  in-process `pid_` to -1, so a leftover dnsmasq from the crashed process is
  invisible to the next `Start()` and a second dnsmasq could be forked. A
  `pkill`/pidfile sweep before fork would close this; in-process repeats and
  graceful restarts are already covered.

## Related

- `docs/solutions/integration-issues/wifi-direct-reform-kills-argus-capture-needs-watchdog-2026-07-03.md`
  — the *second* blast radius of this same `StartP2pGroupOwner` radio reform: on
  this single-radio device the reform also disrupts CSI/VI and kills the Argus
  camera capture pipeline. Same trigger site, different subsystem.
- `docs/solutions/integration-issues/camera-undiscoverable-ble-after-connect-2026-07-09.md`
  — the *third* blast radius: a lingering idle P2P-GO starves BLE advertising
  (RTL8822CE coexistence) so the camera can't be rediscovered. Note: that fix
  now bounds the **idle** case with a 20s teardown grace, so the "group lingers
  for the whole session" assumption below no longer holds for idle paths — but
  the idempotent-start defense here is still required on a hard force-kill (the
  in-process grace timer never fires).
