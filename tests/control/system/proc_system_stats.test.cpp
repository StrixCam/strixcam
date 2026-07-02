#include <gtest/gtest.h>

#include <filesystem>

#include "adapters/control/system/proc-system-stats.hpp"
#include "domain/control/models/system-stats.hpp"

// End-to-end coverage of the stateful /proc/stat CPU delta (the pure parse +
// percentage math live in cpu_usage.test.cpp). Reads real /proc/stat, so it runs
// in the dev container. storage_root points at a temp dir purely to satisfy the
// fs::space() call; the CPU path is what is under test.

namespace {

namespace fs = std::filesystem;

using sst::adapters::control::ProcSystemStats;

TEST(ProcSystemStats, FirstReadReportsZeroCpuThenABoundedPercent) {
    const fs::path root = fs::temp_directory_path();
    ProcSystemStats stats(root);

    // First read has no prior /proc/stat snapshot -> 0% by contract.
    EXPECT_FLOAT_EQ(stats.Read().cpu_used_pct, 0.0F);

    // Second read computes a delta against the first; whatever the load, it must
    // land in [0, 100] (never negative, never a >100% spike from the raw jiffies).
    const float second = stats.Read().cpu_used_pct;
    EXPECT_GE(second, 0.0F);
    EXPECT_LE(second, 100.0F);
}

}  // namespace
