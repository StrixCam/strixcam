---
title: "fix: WiFi-Direct data-plane bring-up (GO IP + caps + dnsmasq)"
type: fix
status: active
date: 2026-06-26
---

# fix: WiFi-Direct data-plane bring-up (GO IP + caps + dnsmasq)

## Summary

Complete the deferred WiFi-Direct data plane so the app's preview actually works:
assign the group-owner IP `192.168.49.1/24` to the P2P group interface after the
group forms (via `ip` fork/exec, mirroring the existing dnsmasq adapter), grant
the service the network capabilities it needs (`CAP_NET_ADMIN`,
`CAP_NET_BIND_SERVICE`), and point dnsmasq at a writable (read-only) lease store.
The control plane (group formation) already works on hardware; this is the last
mile that makes RTSP bind and DHCP serve.

---

## Problem Frame

On the real Jetson (`sst@10.10.1.30`, JetPack 7.2) the P2P **control** plane works
after a prior fix (netdev group + interface auto-detect): the firmware forms the
GO group (`formed group DIRECT-… role=GO` → `SessionManager -> WifiReady`). But the
**data** plane was explicitly deferred in code as "deploy-time provisioning
(KTD4)" and never built, so preview fails:

- `GstRtspAppStreamServer: gst_rtsp_server_attach failed` → `RTSP preview failed
  to start on 192.168.49.1:8554` — because `192.168.49.1` is on no interface
  (`ip -br addr` confirms it is absent).
- `dnsmasq: cannot open or create lease file /var/lib/misc/dnsmasq.leases:
  Permission denied` — DHCP never serves, so a joined phone gets no IP.
- The service process has `CapEff: 0` — it could not assign an IP or bind DHCP's
  privileged port even if it tried.

Net effect: the app shows "wifi failed" / preview never goes live, even though the
camera reports the group credentials over BLE.

---

## Requirements

- R1. After the GO group forms, `192.168.49.1/24` is assigned to the group
  interface and the link is up — before DHCP and RTSP start.
- R2. The systemd service runs with `CAP_NET_ADMIN` (assign IP) and
  `CAP_NET_BIND_SERVICE` (dnsmasq DHCP port 67), and nothing broader.
- R3. dnsmasq starts successfully without write access to `/var/lib/misc`
  (read-only lease handling).
- R4. The IP is torn down with the group (no stale address across sessions).
- R5. End-to-end on the Jetson: requesting preview yields `192.168.49.1` on the
  group iface, dnsmasq serving, RTSP attached on `192.168.49.1:8554`, the app
  preview going LINKING→LIVE, and telemetry `wifi_state` connected.
- R6. The command-construction logic is covered by pure unit tests (the fork/exec
  itself is hardware/root-bound and excluded from the container suite).

---

## Scope Boundaries

- No netlink/in-process IP assignment — use `ip` fork/exec (decided).
- No change to the P2P group-formation (control plane) — it already works.
- No app-side changes (the app already joins the group + plays RTSP on the
  reported `group_owner_ip`).

### Deferred to Follow-Up Work

- **Source-based policy routing for cellular coexistence** (the other half of
  KTD4): keep cellular default route while serving the P2P subnet. Not required
  for preview-on-LAN; revisit if the phone's mobile data drops during preview.
- **App-side discovery race** (flutter_blue_plus clears results on scan): separate
  app-repo plan.
- **BLE OS-level visibility / bonding** (camera not shown in the phone's system
  Bluetooth menu): separate concern, not data-plane.

---

## Context & Research

### Relevant Code and Patterns

- **Bring-up orchestration:** `src/app/control/services/handlers/wifi-direct.handler.cpp`
  — `HandleStart` does `wifi_.StartP2pGroupOwner()` → `dhcp_.Start(group_interface,
  group_owner_ip)` → `streaming_.StartAppStream(...)`; `HandleStop` does
  `dhcp_.Stop(); wifi_.Stop()`. The IP-assign step slots between group formation
  and `dhcp_.Start`, and teardown between `dhcp_.Stop` and `wifi_.Stop`.
- **Adapter + port pattern to mirror:** `IDhcpServer`
  (`src/app/control/ports/dhcp-server.hpp`) + `DnsmasqDhcpServer`
  (`src/adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.cpp`) — a port
  injected into the handler, with a fork/execvp adapter. The new IP configurator
  follows the same shape (port injected into `WifiDirectHandler`, iproute2 adapter).
- **fork/execvp reference:** `DnsmasqDhcpServer::Start` (fork → `execvp` with a
  fixed argv array → `_exit(127)` on exec failure → parent `waitpid(WNOHANG)` to
  catch immediate death). The IP adapter runs `ip` to completion and checks exit
  status (synchronous, unlike the long-lived dnsmasq child).
- **Pure-helper-for-testability precedent:** `ResolveWifiInterface`
  (`src/adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.cpp`) — a free
  function unit-tested with temp dirs while the socket I/O around it is not.
- **GO IP constant:** `kGoIpAddress = "192.168.49.1"`
  (`src/adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.cpp`); subnet
  `192.168.49.0/24` (dnsmasq range `.10–.50`).
- **systemd unit:** embedded heredoc in `embedded_unit()` in `deploy/install.sh`
  (`User=sst-cam`, no capabilities today). `deploy/install-test.sh` guards unit
  contract (e.g. the `video render netdev` grant) with static greps.
- **main wiring:** `src/main.cpp` constructs `DnsmasqDhcpServer` and
  `WifiDirectHandler(session, wifi, dhcp, streaming, …)`; the new configurator is
  constructed here and injected alongside `dhcp`.

### Institutional Learnings

- No `docs/solutions/` entry covers wifi-direct provisioning yet — a strong
  candidate for a learning doc after this lands (the deferred-data-plane pitfall +
  the caps requirement).

### External References

- None needed: iproute2 (`ip`) and dnsmasq behavior are well-established and
  present on the device; the approach is decided.

---

## Key Technical Decisions

- **`ip` fork/exec over netlink** (decided with user): minimal code, mirrors the
  dnsmasq adapter, relies on iproute2 (present on the Jetson). Trade-off: an
  external-process dependency, accepted.
- **New port + adapter (`INetworkConfigurator` / `IpNetworkConfigurator`)** rather
  than adding IP logic to `WpaWifiManager` or `DnsmasqDhcpServer`: keeps single
  responsibility, mirrors `IDhcpServer`/`DnsmasqDhcpServer`, and lets
  `WifiDirectHandler` own the ordering (assign → dhcp → rtsp). Injected into the
  handler the same way `IDhcpServer` is.
- **Synchronous exec with exit-status check** (not a long-lived child like
  dnsmasq): `ip addr add` / `ip link set up` run to completion; treat non-zero as
  failure and surface it so `HandleStart` can fail cleanly and tear down.
- **`--leasefile-ro` for dnsmasq** over a writable lease path: a camera's P2P
  leases are ephemeral; read-only leasing removes the filesystem-permission
  failure mode entirely with no functional loss. (A writable
  `/var/lib/sst/cam/dnsmasq.leases` is the fallback if lease persistence is ever
  needed — noted, not chosen.)
- **Capabilities, not root:** keep `User=sst-cam`; grant only `CAP_NET_ADMIN` +
  `CAP_NET_BIND_SERVICE` via `AmbientCapabilities` (so child `ip`/`dnsmasq`
  inherit them) plus a matching `CapabilityBoundingSet`. Least privilege over
  running the service as root.

---

## Open Questions

### Resolved During Planning

- IP-assign mechanism? → `ip` fork/exec (decided).
- Where does IP assignment live? → new `INetworkConfigurator` adapter injected
  into `WifiDirectHandler`, ordered before `dhcp_.Start`.
- dnsmasq lease fix? → `--leasefile-ro`.
- Run as root? → no; ambient capabilities only.

### Deferred to Implementation

- Exact port/adapter/method names and file placement under
  `src/app/control/ports/` and `src/adapters/control/wifi/`.
- Whether `ip link set <iface> up` is needed in addition to `ip addr add` (the
  wpa-created group iface is often already up) — implement defensively (idempotent;
  ignore "already up"), confirm on device.
- Whether teardown should explicitly `ip addr flush`/`link down` or rely on the
  group iface disappearing at `P2P_GROUP_REMOVE` — decide when verifying on device.

---

## High-Level Technical Design

> *Illustrates the intended approach; directional guidance for review, not
> implementation specification.*

Bring-up sequence after this change (new step ★):

```
WifiDirectHandler::HandleStart
  group = wifi_.StartP2pGroupOwner()          # control plane (already works)
  ★ netcfg_.AssignGroupOwnerAddress(group.group_interface, "192.168.49.1/24")
        -> ip addr add 192.168.49.1/24 dev <iface>;  ip link set <iface> up
        -> on failure: wifi_.Stop(); return error
  dhcp_.Start(group.group_interface, group.group_owner_ip)   # now has a subnet
  streaming_.StartAppStream(...)               # now binds 192.168.49.1:8554

WifiDirectHandler::HandleStop
  dhcp_.Stop()
  ★ netcfg_.Clear(group_interface)             # optional; iface vanishes anyway
  wifi_.Stop()                                  # P2P_GROUP_REMOVE
```

Testable seam (pure, like `ResolveWifiInterface`):

```
BuildAddrAddArgv("192.168.49.1/24", "p2p-wlP1p1s0-0")
   -> ["ip","addr","add","192.168.49.1/24","dev","p2p-wlP1p1s0-0"]
BuildLinkUpArgv("p2p-wlP1p1s0-0")
   -> ["ip","link","set","p2p-wlP1p1s0-0","up"]
```

---

## Implementation Units

### U1. `ip`-argv builder (pure, tested)

**Goal:** Pure functions that build the `ip addr add` / `ip link set up` argument
vectors, so the command construction is unit-tested independent of fork/exec.

**Requirements:** R1, R6

**Dependencies:** None

**Files:**
- Create: `src/adapters/control/wifi/wpa_supplicant/ip-command.hpp`
- Create: `src/adapters/control/wifi/wpa_supplicant/ip-command.cpp`
- Test: `tests/control/ip_command.test.cpp`

**Approach:**
- Free functions returning the argv token lists for assign-address and link-up
  given an iface + CIDR. No I/O. Validate inputs are non-empty; reject obviously
  malformed iface/CIDR (return empty / signal error) so the adapter can fail fast.

**Patterns to follow:**
- `ResolveWifiInterface` free-function + its temp-dir unit test
  (`tests/control/wpa_wifi_manager.test.cpp`).

**Test scenarios:**
- Happy path: `(192.168.49.1/24, p2p-wlP1p1s0-0)` → exact `ip addr add …` token list.
- Happy path: link-up builder → exact `ip link set <iface> up` token list.
- Edge case: empty iface or empty CIDR → empty/err result (no partial command).

**Verification:**
- `just test --gtest_filter=IpCommand*` passes; builders produce the exact tokens.

---

### U2. `INetworkConfigurator` port + `IpNetworkConfigurator` adapter

**Goal:** A port + iproute2 adapter that assigns/clears the GO address on the group
interface by fork/exec of `ip`, checking exit status.

**Requirements:** R1, R4

**Dependencies:** U1

**Files:**
- Create: `src/app/control/ports/network-configurator.hpp`
- Create: `src/adapters/control/wifi/wpa_supplicant/ip-network-configurator.hpp`
- Create: `src/adapters/control/wifi/wpa_supplicant/ip-network-configurator.cpp`
- Test: `tests/control/ip_network_configurator.test.cpp`

**Approach:**
- Port: `AssignGroupOwnerAddress(iface, cidr) -> bool` and `Clear(iface) -> void`
  (names final at implementation). Adapter forks, `execvp("ip", …)` using U1's
  argv, `waitpid` for completion, returns false on non-zero exit. `Clear` is
  best-effort (`addr flush` / `link down`), errors swallowed. Idempotent: treat
  "address exists" / "already up" as success.
- Mirror `DnsmasqDhcpServer`'s fork/execvp structure but synchronous (wait for the
  short-lived `ip` to finish rather than `WNOHANG`).

**Patterns to follow:**
- `DnsmasqDhcpServer` (`dnsmasq-dhcp-server.cpp`) fork/execvp + `_exit(127)`;
  `IDhcpServer` port shape (`dhcp-server.hpp`).

**Test scenarios:**
- Happy path: adapter invokes the U1 argv (assert via the builder seam / a fake
  exec hook if one is introduced; otherwise keep the adapter thin and rely on U1
  for argv coverage + an on-device check).
- Error path: a non-zero `ip` exit makes `AssignGroupOwnerAddress` return false.
- Note: the real fork/exec against a live iface is hardware/root-bound — that
  assertion is an on-device verification (R5), not a container test.

**Verification:**
- Builds clean under the tidy gate; `Clear` never throws; assign returns
  false on a bogus iface (verified on device).

---

### U3. Wire IP assignment into the GO bring-up + teardown

**Goal:** `WifiDirectHandler` assigns the GO address after the group forms and
before DHCP/RTSP, and clears it on stop; `main` constructs + injects the
configurator.

**Requirements:** R1, R4, R5

**Dependencies:** U2

**Files:**
- Modify: `src/app/control/services/handlers/wifi-direct.handler.hpp`
- Modify: `src/app/control/services/handlers/wifi-direct.handler.cpp`
- Modify: `src/main.cpp`
- Test: `tests/control/wifi_direct_handler.test.cpp`

**Approach:**
- Inject `INetworkConfigurator&` into `WifiDirectHandler` (mirror the `IDhcpServer&`
  member). In `HandleStart`, after `StartP2pGroupOwner` succeeds, call
  `AssignGroupOwnerAddress(group.group_interface, "192.168.49.1/24")`; on failure,
  `wifi_.Stop()` and return an error response (same shape as the existing
  dhcp-failure branch). In `HandleStop`, `Clear(group_interface)` between
  `dhcp_.Stop()` and `wifi_.Stop()`.
- `main`: construct `IpNetworkConfigurator` and pass it to `WifiDirectHandler`.

**Patterns to follow:**
- Existing failure-handling branch in `wifi-direct.handler.cpp` (the `dhcp_.Start`
  failure path that calls `wifi_.Stop()`); existing handler test's fakes.

**Test scenarios:**
- Happy path: with a fake configurator returning true, `HandleStart` calls assign
  with the group iface + GO CIDR before `dhcp_.Start` (assert call order via fakes).
- Error path: configurator returns false → `HandleStart` returns an error response
  and calls `wifi_.Stop()`, does NOT call `dhcp_.Start`/`StartAppStream`.
- Integration: `HandleStop` calls `Clear` then `wifi_.Stop()`.

**Verification:**
- `just test --gtest_filter=WifiDirectHandler*` passes; ordering + failure paths
  asserted with fakes.

---

### U4. dnsmasq read-only leases

**Goal:** dnsmasq no longer fails on the root-only lease directory.

**Requirements:** R3

**Dependencies:** None

**Files:**
- Modify: `src/adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.cpp`
- Test: `tests/control/` (extend an existing dnsmasq/argv test if present;
  otherwise factor the argv into a pure builder + test it)

**Approach:**
- Add `--leasefile-ro` to the dnsmasq argv (ephemeral leases; no writable lease
  file needed). Keep the existing flags. If the argv is currently an inline array,
  optionally factor it into a pure builder (like U1) so the flag set is testable.

**Patterns to follow:**
- The existing dnsmasq argv array in `dnsmasq-dhcp-server.cpp`.

**Test scenarios:**
- Happy path: the built dnsmasq argv contains `--leasefile-ro` alongside the
  existing `--interface=…`, `--dhcp-range=…`, `--dhcp-option=3,…`.
- (On device, R5) dnsmasq starts without the "lease file Permission denied" error.

**Verification:**
- No "cannot open or create lease file" in the journal on device; dnsmasq stays up.

---

### U5. Grant network capabilities in the systemd unit

**Goal:** The service can assign IPs and bind DHCP's privileged port without
running as root.

**Requirements:** R2

**Dependencies:** None

**Files:**
- Modify: `deploy/install.sh` (the `embedded_unit` heredoc)
- Modify: `deploy/install-test.sh`

**Approach:**
- Add `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_BIND_SERVICE` (so forked
  `ip`/`dnsmasq` inherit them) and a matching
  `CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_BIND_SERVICE` to the `[Service]`
  section. Keep `User=sst-cam`. Re-running install.sh must update the unit
  (existing `embedded_unit | cmp` re-install path handles this + daemon-reload).
- install-test: static-grep guards that the unit declares both capabilities.

**Execution note:** Config-only on the unit; the behavioral proof is on-device (R5).

**Test scenarios:**
- `deploy/install-test.sh`: assert `embedded_unit` contains `AmbientCapabilities`
  with `CAP_NET_ADMIN` and `CAP_NET_BIND_SERVICE`.
- shellcheck clean.

**Verification:**
- After deploy + restart, `/proc/<pid>/status` `CapEff` includes net_admin +
  net_bind_service (mask), and the WiFi-Direct flow no longer hits EACCES on
  assign/DHCP.

---

## System-Wide Impact

- **Interaction graph:** Only the WiFi-Direct bring-up path
  (`WifiDirectHandler` → wifi/netcfg/dhcp/streaming) and the systemd unit change.
  BLE control plane, telemetry, recording, downloads untouched.
- **Error propagation:** IP-assign failure must fail `HandleStart` cleanly (stop
  the group, return an error) — not leave a half-up group. Mirrors the existing
  dhcp-failure branch.
- **State lifecycle risks:** A stale `192.168.49.1` across sessions if teardown
  doesn't clear it and the iface name is reused — mitigated by `Clear` on stop and
  the group iface disappearing at `P2P_GROUP_REMOVE`. Idempotent assign tolerates a
  leftover address.
- **Privilege surface:** the service gains two network capabilities. Bounded by
  `CapabilityBoundingSet`; still non-root. Document in the unit comment.
- **Unchanged invariants:** `IWifiManager`/`IDhcpServer` ports and the P2P
  group-formation behavior are unchanged; the new configurator is additive.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `ip` not present / different path on device | iproute2 ships on the Jetson (verified); `execvp` uses PATH; assign-failure fails the flow with a clear log |
| Group iface name differs from assumption (`p2p-…`) | Use the `group_interface` reported by `StartP2pGroupOwner` (parsed from P2P-GROUP-STARTED), never a hardcoded name |
| AmbientCapabilities not honored (older systemd) | JetPack 7.2 systemd supports it; install-test guards presence; on-device CapEff check in R5 |
| `--leasefile-ro` insufficient if persistence ever needed | Documented fallback: writable `/var/lib/sst/cam/dnsmasq.leases` (data dir already chowned) |
| fork/exec hard to unit-test | Argv logic factored into pure builders (U1/U4) that ARE tested; exec path verified on device |

---

## Documentation / Operational Notes

- Update `deploy/README.md` if the capability grant or P2P data-plane behavior is
  worth calling out for operators.
- Strong `docs/solutions/` learning candidate after this lands: "WiFi-Direct GO
  needs the firmware to assign the static IP + CAP_NET_ADMIN/CAP_NET_BIND_SERVICE;
  the control plane forming is not enough."
- Verification loop: `just deploy-jetson sst@10.10.1.30`, then trigger preview from
  the app and check the journal for IP-assigned / dnsmasq-serving / RTSP-attached,
  and `ip -br addr` for `192.168.49.1`.

---

## Sources & References

- Bring-up: `src/app/control/services/handlers/wifi-direct.handler.cpp`
- Port/adapter pattern: `src/app/control/ports/dhcp-server.hpp`,
  `src/adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.cpp`
- Pure-helper precedent: `ResolveWifiInterface` in
  `src/adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.cpp`
- systemd unit: `embedded_unit` in `deploy/install.sh`
- Diagnosis: this debug session (Jetson `sst@10.10.1.30`, journal: `gst_rtsp_server_attach failed`, `dnsmasq … lease file Permission denied`, `CapEff: 0`).
