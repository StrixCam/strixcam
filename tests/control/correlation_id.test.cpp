// CorrelationId wrapper + formatter (U5, R2).

#include <gtest/gtest.h>

#include <fmt/format.h>

#include "domain/control/models/correlation-id.hpp"
#include "domain/control/models/formatter/correlation-id-fmt.hpp"

namespace {

using sst::control::CorrelationId;

TEST(CorrelationIdTest, WrapsValue) {
    CorrelationId id{"req-42"};
    EXPECT_EQ(id.value, "req-42");
}

TEST(CorrelationIdTest, FormatterRendersValue) {
    EXPECT_EQ(fmt::format("{}", CorrelationId{"req-42"}), "CorrelationId{req-42}");
}

}  // namespace
