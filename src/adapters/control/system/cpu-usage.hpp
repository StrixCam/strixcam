#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sst::adapters::control {

// Aggregate CPU jiffy counters from the `/proc/stat` "cpu" line. `busy` excludes
// idle + iowait; `total` is every counter summed. Utilisation is the ratio of
// the DELTAS of these across two samples — a single snapshot carries no percent.
struct CpuTimes {
    std::uint64_t busy{0};
    std::uint64_t total{0};
};

// Parses the aggregate "cpu " line of /proc/stat contents into CpuTimes.
// Fields: user nice system idle iowait irq softirq steal [guest guest_nice].
// busy = total - idle - iowait. Returns nullopt if the line is missing/malformed.
auto ParseProcStatCpu(const std::string& proc_stat_contents) -> std::optional<CpuTimes>;

// Busy-CPU percentage [0,100] between two samples (prev → cur). Returns 0 when
// the total delta is non-positive (no elapsed jiffies, or counters went
// backwards) so a bad/first sample reads as 0% rather than a spike.
auto CpuBusyPercent(const CpuTimes& prev, const CpuTimes& cur) -> float;

}  // namespace sst::adapters::control
