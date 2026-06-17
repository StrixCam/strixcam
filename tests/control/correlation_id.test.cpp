// CorrelationId wrapper + formatter (U5, R2).

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "domain/control/models/correlation-id.hpp"
#include "domain/control/models/formatter/correlation-id-fmt.hpp"

namespace {

using sst::control::CorrelationId;

TEST(CorrelationIdTest, WrapsValue) {
    CorrelationId cid{"req-42"};
    EXPECT_EQ(cid.value, "req-42");
}

TEST(CorrelationIdTest, FormatterRendersValue) {
    EXPECT_EQ(fmt::format("{}", CorrelationId{"req-42"}), "CorrelationId{req-42}");
}

}  // namespace
