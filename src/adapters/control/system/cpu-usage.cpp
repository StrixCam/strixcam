#include "adapters/control/system/cpu-usage.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

namespace sst::adapters::control {

namespace {
constexpr float kPercentMax = 100.0F;
// user nice system idle iowait irq softirq steal — the fields we sum. guest /
// guest_nice are already counted inside user/nice by the kernel, so we stop here.
constexpr std::size_t kCpuFieldCount = 8;
constexpr std::size_t kIdleIndex = 3;    // idle
constexpr std::size_t kIowaitIndex = 4;  // iowait
}  // namespace

auto ParseProcStatCpu(const std::string& proc_stat_contents) -> std::optional<CpuTimes> {
    std::istringstream stream(proc_stat_contents);
    std::string label;
    // The aggregate line is the first token "cpu" (no trailing digit). Per-core
    // lines are "cpu0", "cpu1", … — skip anything that isn't exactly "cpu".
    if (!(stream >> label) || label != "cpu") {
        return std::nullopt;
    }

    std::array<std::uint64_t, kCpuFieldCount> fields{};
    for (std::uint64_t& field : fields) {
        if (!(stream >> field)) {
            return std::nullopt;
        }
    }

    CpuTimes times;
    for (const std::uint64_t field : fields) {
        times.total += field;
    }
    times.busy = times.total - fields.at(kIdleIndex) - fields.at(kIowaitIndex);
    return times;
}

auto CpuBusyPercent(const CpuTimes& prev, const CpuTimes& cur) -> float {
    if (cur.total <= prev.total || cur.busy < prev.busy) {
        return 0.0F;
    }
    const std::uint64_t total_delta = cur.total - prev.total;
    const std::uint64_t busy_delta = cur.busy - prev.busy;
    const float pct =
        (static_cast<float>(busy_delta) / static_cast<float>(total_delta)) * kPercentMax;
    return pct > kPercentMax ? kPercentMax : pct;
}

}  // namespace sst::adapters::control
