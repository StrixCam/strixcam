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
    // Assign thread_ while still holding run_mtx_ so a concurrent Stop() can never
    // observe running_==true with an unassigned thread_ (which would leave the
    // Loop thread joinable-but-never-joined → std::terminate at destruction). The
    // first sample runs inside Loop() rather than synchronously here, so Start()
    // never blocks the caller on a slow `ip`/`iw` fork at boot.
    const std::lock_guard lock(run_mtx_);
    if (running_) {
        return;
    }
    running_ = true;
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
        lock.unlock();
        SampleOnce();  // sample immediately on the first pass, then every interval
        lock.lock();
        // Wait out the interval, but wake immediately on Stop() so shutdown is
        // prompt (bounded only by an already-in-flight SampleOnce()).
        if (run_cv_.wait_for(lock, interval_, [this] { return !running_; })) {
            break;
        }
    }
}

}  // namespace sst::control
