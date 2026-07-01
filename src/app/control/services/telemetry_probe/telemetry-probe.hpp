#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "app/control/ports/wifi-signal-probe.hpp"
#include "app/streaming/ports/uplink-probe.hpp"

namespace sst::control {

// Background sampler for the two telemetry signals that need a subprocess to
// read (internet uplink via `ip route`, WiFi peer RSSI via `iw`). Polling them
// inline on the BLE dispatcher thread — once per telemetry request — forked a
// child per poll and risked stalling every BLE command on a hung tool. This
// owns a single poll thread that refreshes both on an interval into a cache
// (internet_reachable as an atomic, wifi_signal_dbm under a mutex); the
// telemetry handler then reads them instantly, never forking on its thread.
//
// Start() before use; Stop()/destruction joins the thread. Getters are safe from
// any thread. Start()/Stop() are idempotent but must be called from a single
// controlling thread (they are not mutually reentrant across threads).
class TelemetryProbe {
   public:
    static constexpr std::chrono::seconds kDefaultInterval{5};

    // interval accepts milliseconds (seconds convert implicitly) so tests can
    // drive a fast poll cadence; production uses the 5s default.
    TelemetryProbe(sst::streaming::IUplinkProbe& uplink, IWifiSignalProbe& wifi_signal,
                   std::chrono::milliseconds interval = kDefaultInterval);
    ~TelemetryProbe();

    TelemetryProbe(const TelemetryProbe&) = delete;
    auto operator=(const TelemetryProbe&) -> TelemetryProbe& = delete;
    TelemetryProbe(TelemetryProbe&&) = delete;
    auto operator=(TelemetryProbe&&) -> TelemetryProbe& = delete;

    // Starts the poll thread; its first pass samples immediately, so the cache
    // warms shortly after (not synchronously — Start() never blocks the caller on
    // a slow tool). Getters return safe defaults until then. Idempotent.
    auto Start() -> void;
    // Signals and joins the poll thread. Idempotent; also called by the dtor.
    auto Stop() -> void;

    [[nodiscard]] auto InternetReachable() const -> bool { return internet_reachable_.load(); }
    [[nodiscard]] auto WifiSignalDbm() const -> std::optional<int>;

   private:
    auto SampleOnce() -> void;
    auto Loop() -> void;

    sst::streaming::IUplinkProbe& uplink_;
    IWifiSignalProbe& wifi_signal_;
    std::chrono::milliseconds interval_;

    std::atomic<bool> internet_reachable_{false};
    mutable std::mutex signal_mtx_;
    std::optional<int> wifi_signal_dbm_;

    std::thread thread_;
    std::mutex run_mtx_;
    std::condition_variable run_cv_;
    bool running_{false};
};

}  // namespace sst::control
