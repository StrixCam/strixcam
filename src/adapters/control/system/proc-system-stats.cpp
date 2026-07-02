#include "adapters/control/system/proc-system-stats.hpp"

#include <spdlog/spdlog.h>
#include <sys/sysinfo.h>

#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace sst::adapters::control {

namespace fs = std::filesystem;

namespace {

constexpr float kPercentMax = 100.0F;
constexpr double kMilliDegreePerDegree = 1000.0;

auto ReadThermalCelsius() -> float {
    // Jetson exposes SoC temperature in millidegrees here.
    std::ifstream stream("/sys/class/thermal/thermal_zone0/temp");
    if (!stream) {
        return 0.0F;
    }
    std::int64_t milli = 0;
    stream >> milli;
    if (!stream) {
        return 0.0F;
    }
    return static_cast<float>(static_cast<double>(milli) / kMilliDegreePerDegree);
}

auto ReadProcStat() -> std::string {
    std::ifstream stream("/proc/stat");
    if (!stream) {
        return {};
    }
    std::string line;
    std::getline(stream, line);  // the aggregate "cpu ..." line is the first
    return line;
}

}  // namespace

ProcSystemStats::ProcSystemStats(fs::path storage_root) : storage_root_(std::move(storage_root)) {}

auto ProcSystemStats::Read() const -> sst::control::SystemStats {
    sst::control::SystemStats stats;

    // Storage — from the configured video root (its filesystem).
    std::error_code space_ec;
    const auto space = fs::space(storage_root_, space_ec);
    if (!space_ec) {
        stats.storage_free_bytes = space.available;
        stats.storage_total_bytes = space.capacity;
    } else {
        spdlog::warn("ProcSystemStats: fs::space({}) failed: {}", storage_root_.string(),
                     space_ec.message());
    }

    // RAM + uptime — from sysinfo(2).
    struct sysinfo info {};
    if (sysinfo(&info) == 0) {
        stats.uptime_seconds = static_cast<std::uint64_t>(info.uptime);
        const std::uint64_t total = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
        const std::uint64_t freer = static_cast<std::uint64_t>(info.freeram) * info.mem_unit;
        if (total > 0) {
            const double used = static_cast<double>(total - freer) / static_cast<double>(total);
            stats.ram_used_pct = static_cast<float>(used * kPercentMax);
        }
    }

    // CPU — real busy-jiffy utilisation from a /proc/stat delta vs the previous
    // read (loadavg counts the run queue, incl. I/O waiters, so it falsely pegged
    // 100% under non-CPU load). First read has no prior sample → 0%.
    if (const std::optional<CpuTimes> cur = ParseProcStatCpu(ReadProcStat())) {
        const std::lock_guard lock(cpu_mtx_);
        if (prev_cpu_) {
            stats.cpu_used_pct = CpuBusyPercent(*prev_cpu_, *cur);
        }
        prev_cpu_ = cur;
    }

    // SoC temperature (device-specific; 0 off-device).
    stats.temp_celsius = ReadThermalCelsius();

    // No battery sensor wired yet.
    stats.battery_level_pct = 0;

    return stats;
}

}  // namespace sst::adapters::control
