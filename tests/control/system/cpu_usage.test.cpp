#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "adapters/control/system/cpu-usage.hpp"

// Pure unit tests for the /proc/stat CPU parse + busy-delta percentage.

namespace {

using sst::adapters::control::CpuBusyPercent;
using sst::adapters::control::CpuTimes;
using sst::adapters::control::ParseProcStatCpu;

TEST(ParseProcStatCpu, SumsFieldsAndExcludesIdleAndIowaitFromBusy) {
    constexpr std::uint64_t kIdle = 1000;
    constexpr std::uint64_t kIowait = 30;
    // cpu  user nice system idle iowait irq softirq steal ...
    const auto times = ParseProcStatCpu("cpu  100 0 50 1000 30 5 5 0 0 0\ncpu0 ...\n");
    ASSERT_TRUE(times.has_value());
    const CpuTimes value = times.value_or(CpuTimes{});
    EXPECT_EQ(value.total, 100U + 0 + 50 + kIdle + kIowait + 5 + 5 + 0);  // 1190
    EXPECT_EQ(value.busy, value.total - kIdle - kIowait);                 // total - idle - iowait
}

TEST(ParseProcStatCpu, RejectsPerCoreAndMalformedLines) {
    EXPECT_FALSE(ParseProcStatCpu("cpu0 1 2 3 4 5 6 7 8").has_value());  // per-core, not aggregate
    EXPECT_FALSE(ParseProcStatCpu("cpu 1 2 3").has_value());             // too few fields
    EXPECT_FALSE(ParseProcStatCpu("").has_value());
    EXPECT_FALSE(ParseProcStatCpu("intr 1 2 3 4 5 6 7 8").has_value());
}

TEST(CpuBusyPercent, ComputesBusyShareOfTheDelta) {
    // Between samples: total advances 200, busy advances 50 → 25%.
    const CpuTimes prev{.busy = 100, .total = 1000};
    const CpuTimes cur{.busy = 150, .total = 1200};
    EXPECT_FLOAT_EQ(CpuBusyPercent(prev, cur), 25.0F);
}

TEST(CpuBusyPercent, ZeroWhenNoElapsedJiffiesOrCountersRegress) {
    const CpuTimes same{.busy = 100, .total = 1000};
    EXPECT_FLOAT_EQ(CpuBusyPercent(same, same), 0.0F);  // no elapsed time
    const CpuTimes regressed{.busy = 50, .total = 900};
    EXPECT_FLOAT_EQ(CpuBusyPercent(same, regressed), 0.0F);  // counters went backwards
}

TEST(CpuBusyPercent, ZeroWhenIdleAcrossTheWindow) {
    // The common real-world reading: time elapses (total advances) but no busy
    // jiffies accrue — an idle Jetson must read 0%, distinct from "no elapsed time".
    const CpuTimes prev{.busy = 100, .total = 1000};
    const CpuTimes cur{.busy = 100, .total = 1200};
    EXPECT_FLOAT_EQ(CpuBusyPercent(prev, cur), 0.0F);
}

TEST(CpuBusyPercent, FullyLoadedReadsExactlyFullScale) {
    // Non-degenerate 100%: every elapsed jiffy is busy (busy delta == total delta),
    // so the upper bound is pinned by real math, not only the clamp.
    const CpuTimes prev{.busy = 500, .total = 1000};
    const CpuTimes cur{.busy = 700, .total = 1200};
    EXPECT_FLOAT_EQ(CpuBusyPercent(prev, cur), 100.0F);
}

TEST(ParseProcStatCpu, HandlesPreStealSevenFieldKernels) {
    // Older /proc/stat lines omit steal+guest; the parser needs 8 fields, so a
    // 7-field line degrades to nullopt (0% CPU) rather than mis-summing.
    EXPECT_FALSE(ParseProcStatCpu("cpu 10 0 5 100 2 1 1\n").has_value());
}

TEST(CpuBusyPercent, ClampsToFullScale) {
    const CpuTimes prev{.busy = 0, .total = 0};
    const CpuTimes cur{.busy = 500, .total = 100};  // busy delta > total delta (degenerate)
    EXPECT_FLOAT_EQ(CpuBusyPercent(prev, cur), 100.0F);
}

}  // namespace
