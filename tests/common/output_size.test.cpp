// OutputSize value type + formatter (U1, R1/R2).

#include <gtest/gtest.h>

#include <fmt/format.h>

#include "domain/common/models/formatter/output-size-fmt.hpp"
#include "domain/common/models/output-size.hpp"

namespace {

using sst::common::OutputSize;

TEST(OutputSizeTest, AggregateInitReadsBack) {
    OutputSize size{1920, 1080};
    EXPECT_EQ(size.width, 1920U);
    EXPECT_EQ(size.height, 1080U);
}

TEST(OutputSizeTest, FormatterRendersFields) {
    OutputSize size{1920, 1080};
    EXPECT_EQ(fmt::format("{}", size), "OutputSize{width=1920, height=1080}");
}

TEST(OutputSizeTest, NativeSizeSentinelRoundTrips) {
    OutputSize native{0, 0};
    EXPECT_EQ(native.width, 0U);
    EXPECT_EQ(native.height, 0U);
    EXPECT_EQ(fmt::format("{}", native), "OutputSize{width=0, height=0}");
}

}  // namespace
