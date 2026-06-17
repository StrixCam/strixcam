// OutputSize value type + formatter (U1, R1/R2).

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "domain/common/models/formatter/output-size-fmt.hpp"
#include "domain/common/models/output-size.hpp"

namespace {

using sst::common::OutputSize;

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;

TEST(OutputSizeTest, AggregateInitReadsBack) {
    OutputSize size{kWidth, kHeight};
    EXPECT_EQ(size.width, kWidth);
    EXPECT_EQ(size.height, kHeight);
}

TEST(OutputSizeTest, FormatterRendersFields) {
    OutputSize size{kWidth, kHeight};
    EXPECT_EQ(fmt::format("{}", size), "OutputSize{width=1920, height=1080}");
}

TEST(OutputSizeTest, NativeSizeSentinelRoundTrips) {
    OutputSize native{0, 0};
    EXPECT_EQ(native.width, 0U);
    EXPECT_EQ(native.height, 0U);
    EXPECT_EQ(fmt::format("{}", native), "OutputSize{width=0, height=0}");
}

}  // namespace
