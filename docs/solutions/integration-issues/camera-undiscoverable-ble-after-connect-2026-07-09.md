---
title: Camera undiscoverable over BLE after first connect — idle WiFi-Direct group starves advertising
date: 2026-07-09
category: integration-issues
module: session-manager
problem_type: integration_issue
component: service_object
symptoms:
  - "Camera does not appear in the app's BLE discovery after the first connect + app force-kill"
  - "Fresh boot (no WiFi group) is discoverable; any camera that has served one connection goes dark on the radio"
  - "On-device btmon shows the advert HCI-enabled, yet Android's raw ScanController logcat never receives it"
  - "BlueZ reports ActiveInstances=1 (believes it is advertising) while the radio is effectively dark"
  - "The idle preview WiFi-Direct P2P-GO stays up for the full 30-min auto-stop window while session=Idle"
root_cause: logic_error
resolution_type: code_fix
severity: high
tags: [ble-advertising, wifi-direct, rtl8822ce, bt-coex, session-manager, p2p-go, radio-coexistence, ble-discovery, jetson]
related_components: [wifi-direct-p2p-group, ble-advertising, argus-preview-pipeline]
---

# Camera undiscoverable over BLE after first connect — idle WiFi-Direct group starves advertising

## Problem

After an Android app connected to the camera and was then force-killed, the camera vanished from BLE discovery and could never be rediscovered — a fresh boot advertised fine, but any camera that had served one app connection went dark on the radio even though its BLE advertiser was still registered and "enabled." Root cause: the RTL8822CE WiFi+BT combo chip's WiFi-Direct P2P group (kept up after the app left) starves BLE advertising via radio coexistence.

## Symptoms

- Camera absent from the app's BLE discovery **after the first connect + force-kill**; a fresh boot works, but the camera goes dark post-connect.
- On-device `btmon` shows the advertisement **enabled** (HCI `LE Set Extended Advertising Enable` succeeded), yet Android's raw `ScanController` logcat **never receives the advert** (its address never appears in scan results, even pre-filter).
- BlueZ reports `ActiveInstances=1` — it *believes* it is advertising — while the radio is effectively dark. BlueZ state and RF reality disagree.
- On the Jetson: `iw dev wlP1p1s0 info` shows `type P2P-GO`, SSID `DIRECT-8x-sst-cam-7967`, on a 2.4 GHz channel; the firmware log reads `SessionManager: app disconnected — session=Idle group=up kept`.

## What Didn't Work

Three dead ends, each of which *looked* like the cause because it correlated with the symptom:

**(a) The BLE advertising watchdog re-register churn.** The advertiser's periodic re-register was hitting an HCI `0x12` "Invalid HCI Command Parameters" wedge, so it was the obvious suspect — a self-inflicted advertiser wedge fully explains "advert claims enabled but nothing goes out." An **edge-only re-advertise** change (re-register only on a real disconnect edge, not on the blind 30 s timer) **did help** and was worth keeping, but it was **not the root cause**: the camera still went dark after a clean connect/disconnect with no watchdog churn at all. A real but secondary bug that masked the primary one by producing the same visible symptom.

**(b) The warm `reboot` that triggered a GPU ACR crash-loop.** Rebooting to "reset BLE" led the nvgpu secure-boot ACR (Access-Controlled-Region) ucode bootstrap into a crash-loop (`Bootstrap HS ACR failed` → `Cuda failure: status=100` → `EGL failed to initialize! Exiting...` → SEGV) that killed the firmware process entirely. With the firmware dead, BLE was of course absent — which looked like "the BLE fix didn't take," when in fact it was a **separate platform bug** (warm-reset ACR bootstrap failure) that only a **cold power-cycle** cleared. It burned time by masking whatever state the BLE fix had actually left the radio in.

**(c) PC-side scans as ground truth.** `bluetoothctl`/`btmgmt` scans from a laptop were flaky and intermittently missed adverts that the phone *could* see (and vice versa), so they gave contradictory readings and sent the investigation chasing phantom failures. The **phone's own scanner** is the only ground truth that matters — it is the actual client, on the actual chipset, under the actual coexistence conditions.

## Solution

The real fix is in `SessionManager::OnDisconnect`: split the idle-with-group-up case out from the active-session case, and arm a **short teardown grace** on the orphaned idle preview group instead of leaving it up until the 30-min auto-stop.

Before — an idle preview group that outlived the app was simply left standing (only an active session got the auto-stop net; the idle+group case fell into the same "keep" branch and lingered):

```cpp
auto SessionManager::OnDisconnect() -> void {
    const std::lock_guard lock(mtx_);
    state_.app_connected = false;
    if (state_.phase == SessionPhase::kFinalizing) { /* ... */ return; }
    if (state_.phase != SessionPhase::kIdle || state_.wifi_group_up) {
        ArmAutoStopLocked();   // 30-min net — but an idle group has no session to bound
        // "app disconnected — session=Idle group=up kept"  <-- the bug
        return;
    }
    // idle, no group: nothing to do
}
```

After — an active session still arms the 30-min auto-stop, but **idle with the group still up** arms a dedicated short grace (`session-manager.cpp`):

```cpp
    if (state_.phase != SessionPhase::kIdle) {
        // Active session (Configured/Ready/Recording) must survive the disconnect.
        ArmAutoStopLocked();
        return;
    }
    if (state_.wifi_group_up) {
        // Idle, but the WiFi-Direct group is still up (preview only). It serves
        // only the now-gone app AND starves BLE advertising on the shared 2.4GHz
        // combo radio (RTL8822CE coexistence), so the camera cannot be
        // rediscovered while it lingers. Arm a SHORT grace, not the 30-min
        // auto-stop: a quick reconnect (OnConnect cancels it) keeps the group so
        // preview resumes without a disruptive P2P reform; a real disconnect lets
        // it tear down within seconds and advertising recovers.
        ArmIdleGroupGraceLocked();
        return;
    }
```

The timer arming was refactored so the auto-stop net and the new grace share one backing (`ArmTimerLocked`); the fire handler branches on live state, not on which duration armed it:

```cpp
auto SessionManager::ArmTimerLocked(std::chrono::milliseconds timeout) -> void {
    auto_stop_deadline_ = SteadyClock::now() + timeout;
    timer_cv_.notify_all();
}

auto SessionManager::ArmAutoStopLocked() -> void {
    const auto timeout = (state_.config && state_.config->auto_stop_minutes > 0)
                             ? state_.config->auto_stop_minutes * timing_.config_minute
                             : timing_.default_auto_stop;
    ArmTimerLocked(timeout);
}

auto SessionManager::ArmIdleGroupGraceLocked() -> void {
    ArmTimerLocked(timing_.idle_group_grace);
}
```

The grace duration is a new `SessionTiming` knob (`session-manager.hpp`):

```cpp
std::chrono::milliseconds idle_group_grace{std::chrono::seconds{20}};
```

When the grace elapses, `HandleAutoStopLocked` recognizes the idle-group case and tears the group down — **no session ended, so no summary is written**:

```cpp
    if (state_.phase == SessionPhase::kIdle && state_.wifi_group_up) {
        // Preview-only group with no app: tear the group down (frees BLE
        // advertising for rediscovery); no session ended so no summary is written.
        lock.unlock();
        SafeStep("TeardownWifiDirect", [this] { cleanup_.TeardownWifiDirect(); });
        lock.lock();
        state_.wifi_group_up = false;
    }
```

Note the teardown machinery already existed — `cleanup_.TeardownWifiDirect()` was already the active-session finalize path, and the idle branch reuses the same seam. **Only the armed timeout changed for the idle-group case.** `OnConnect` already cancels any pending deadline via `CancelAutoStopLocked()`, so blip-reconnects keep the group with no code change there.

Metal log confirming the fix (2026-07-09):

```
WiFi group up                                                          <- connect forms preview group
central link down                                                     <- force-kill
app disconnected (idle, group up) — group-teardown grace armed (20s)  <- FIX arms
idle group-teardown grace elapsed — tearing down WiFi group           <- +20s, group gone -> BLE recovers
app connected -> WiFi group up                                        <- rediscovered + reconnected
```

## Why This Works

The camera is an **RTL8822CE** — a single-die WiFi+BT combo chip sharing one 2.4 GHz front end (`rtk_btcoex`). When it brings up a WiFi-Direct **P2P group owner (P2P-GO)** for app preview, that 2.4 GHz P2P traffic **starves BLE advertising**: the advert stays registered and HCI-enabled (hence `btmon` "enabled" and BlueZ `ActiveInstances=1`), but coexistence arbitration gives it so few TX windows that the phone's scanner never lands on one. After a force-kill the app is gone, but the preview group it was created for **lingers with no consumer** — pinning the radio and keeping the already-registered advert off the air. Tearing the orphaned group down **frees the 2.4 GHz front end**, and the advert that was registered all along finally broadcasts, so the phone rediscovers the camera. (Proven manually: `wpa_cli -i wlP1p1s0 p2p_group_remove wlP1p1s0` made the camera reappear immediately, with no BLE re-advertise.)

The teardown is a **grace, not immediate**, because an immediate tear-down would fight quick reconnects: a brief disconnect blip would drop the P2P group, and the reconnect would force a **P2P group reform**, which restarts Argus and surfaces as `INVALID_SETTINGS` (see the Argus-watchdog doc under Related). The 20 s grace lets a real disconnect free the radio within seconds while a blip keeps the group intact — and `OnConnect` cancelling the grace is what makes the blip case keep preview alive with no reform.

## Prevention

- **Ground-truth tooling — trust the right layer.**
  - On-device **`btmon`** is HCI truth: it tells you the controller *accepted* `LE Set Extended Advertising Enable`. Necessary but **not sufficient** — "enabled" at HCI does not mean "on the air" under coexistence.
  - The **Android raw `ScanController` logcat** is the only client-side ground truth (the actual scanner on the actual chip). If `btmon` says enabled but `ScanController` never sees the advert, suspect **RF / coexistence**, not the BLE stack.
  - **BlueZ `ActiveInstances` LIES** in this failure mode — it reported `1` while the radio was dark. Do not treat BlueZ D-Bus state as RF reality.
  - **PC-side `bluetoothctl`/`btmgmt` scans are not reliable ground truth** vs the phone.
- **Combo-chip coexistence awareness.** On a shared-antenna WiFi+BT die (RTL8822CE / `rtk_btcoex`), a 2.4 GHz P2P-GO can silently starve BLE. Any long-lived WiFi-Direct group must have an owner and be torn down when its consumer leaves — a "harmless" idle preview group is a radio hog.
- **Cost of the shorter idle lifetime.** Bounding the idle group at 20 s instead of 30 min means the P2P group forms/tears down more often; every P2P reform collaterally restarts Argus (`INVALID_SETTINGS`). The producer watchdog covers it, but it is not free — the grace + `OnConnect`-cancel is what keeps blip reconnects from paying that cost.
- **Reconnect-within-grace regression test** (`tests/session/session_manager.test.cpp`) locks in that a blip keeps the group and never triggers a teardown/reform:

```cpp
TEST(SessionManagerTest, IdleGroupKeptWhenAppReconnectsWithinGrace) {
    FakeCleanup cleanup;
    SessionManager manager(cleanup, nullptr, TestTiming());

    ASSERT_TRUE(manager.OnConnect());
    ASSERT_TRUE(manager.OnWifiReady());
    manager.OnDisconnect();  // arms the idle-group teardown grace

    // Reconnect before the grace elapses: the pending teardown is cancelled.
    std::this_thread::sleep_for(kBeforeTimeout);
    EXPECT_TRUE(manager.OnConnect());

    // Past the original grace deadline: the group is still up, never torn down.
    std::this_thread::sleep_for(kSettleWait);
    const auto after = manager.Snapshot();
    EXPECT_TRUE(after.wifi_group_up);
    EXPECT_EQ(cleanup.teardown_wifi.load(), 0);
}
```

  Its sibling `IdleGroupTornDownAfterTimeoutWithoutSummary` asserts the real-disconnect path tears the group down (`teardown_wifi == 1`) and writes **no** session-end summary. Both use `TestTiming()`, which shrinks `idle_group_grace` to the same millisecond scale as the auto-stop knobs so timer scenarios run fast.
- **Verify in the devcontainer only** — the host has no cross-toolchain/sysroot, so IDE/clangd diagnostics on the host are meaningless. Gate with `devcontainer exec --workspace-folder . bash -lc 'cmake --preset test && cmake --build --preset test && ctest --preset test'`.

## Related Issues

- `docs/solutions/integration-issues/wifi-direct-data-plane-idempotency-2026-06-26.md` — same abrupt-BLE-drop / cleanup-skipped premise. That doc assumed the leftover P2P group lingers for the whole session; this fix bounds the **idle** case to a 20 s teardown. On a hard force-kill (SIGKILL/OOM) the in-process grace timer never fires either, so that doc's idempotent-start defense is still required — the two are complementary, not contradictory.
- `docs/solutions/integration-issues/wifi-direct-reform-kills-argus-capture-needs-watchdog-2026-07-03.md` — the "third blast radius" sibling of the same single-radio P2P-GO trigger site. Relevant because the shorter idle-group lifetime here increases P2P-reform frequency, and every reform collaterally kills Argus capture (covered by that doc's producer watchdog).
- GitHub issues: none (repo tracks no issues).
