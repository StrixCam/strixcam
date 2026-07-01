#pragma once

#include <filesystem>
#include <mutex>
#include <optional>

#include "adapters/control/system/cpu-usage.hpp"
#include "app/control/ports/system-stats.hpp"
#include "domain/control/models/system-stats.hpp"

namespace sst::adapters::control {

// Reads device health from the Linux OS: storage from the configured video
// root (statvfs via std::filesystem), RAM + uptime from sysinfo(2), CPU
// utilisation from a /proc/stat busy-jiffy delta between consecutive reads, and
// SoC temperature from the thermal sysfs zone. Battery is reported as 0 until a
// battery sensor is wired. The storage/RAM/uptime/CPU reads work in the dev
// container; temp is device-specific and best-effort.
//
// CPU% needs two samples, so the previous /proc/stat snapshot is held across
// calls (guarded — Read() may be invoked from the telemetry poll thread). The
// first Read() (no prior sample) reports 0% CPU.
class ProcSystemStats final : public sst::control::ISystemStats {
   public:
    explicit ProcSystemStats(std::filesystem::path storage_root);

    [[nodiscard]] auto Read() const -> sst::control::SystemStats override;

   private:
    std::filesystem::path storage_root_;
    mutable std::mutex cpu_mtx_;
    mutable std::optional<CpuTimes> prev_cpu_;
};

}  // namespace sst::adapters::control
