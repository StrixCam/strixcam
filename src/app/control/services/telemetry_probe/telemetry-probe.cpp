#include "app/control/services/telemetry_probe/telemetry-probe.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace sst::control {

TelemetryProbe::TelemetryProbe(sst::streaming::IUplinkProbe& uplink, IWifiSignalProbe& wifi_signal,
                               std::chrono::milliseconds interval)
    : uplink_(uplink), wifi_signal_(wifi_signal), interval_(interval) {}

TelemetryProbe::~TelemetryProbe() { Stop(); }

auto TelemetryProbe::WifiSignalDbm() const -> std::optional<int> {
    const std::lock_guard lock(signal_mtx_);
    return wifi_signal_dbm_;
}

auto TelemetryProbe::SampleOnce() -> void {
    internet_reachable_.store(uplink_.HasInternetUplink());
    std::optional<int> signal = wifi_signal_.SampleSignalDbm();
    const std::lock_guard lock(signal_mtx_);
    wifi_signal_dbm_ = signal;
}

auto TelemetryProbe::Start() -> void {
    {
        const std::lock_guard lock(run_mtx_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    SampleOnce();  // warm the cache before the first telemetry request
    thread_ = std::thread([this] { Loop(); });
}

auto TelemetryProbe::Stop() -> void {
    {
        const std::lock_guard lock(run_mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    run_cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

auto TelemetryProbe::Loop() -> void {
    std::unique_lock lock(run_mtx_);
    while (running_) {
        // Wait out the interval, but wake immediately on Stop() so shutdown is
        // prompt rather than blocked on a full interval.
        if (run_cv_.wait_for(lock, interval_, [this] { return !running_; })) {
            break;
        }
        lock.unlock();
        SampleOnce();
        lock.lock();
    }
}

}  // namespace sst::control
