// PreviewPort/DownloadPort wrappers + formatters (U4, R2).

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "domain/control/models/formatter/service-ports-fmt.hpp"
#include "domain/control/models/service-ports.hpp"

namespace {

using sst::control::DownloadPort;
using sst::control::PreviewPort;

TEST(ServicePortsTest, WrappersCarryValue) {
    PreviewPort preview{8554};
    DownloadPort download{8080};
    EXPECT_EQ(preview.value, 8554U);
    EXPECT_EQ(download.value, 8080U);
}

TEST(ServicePortsTest, FormattersRenderValue) {
    EXPECT_EQ(fmt::format("{}", PreviewPort{8554}), "PreviewPort{8554}");
    EXPECT_EQ(fmt::format("{}", DownloadPort{8080}), "DownloadPort{8080}");
}

}  // namespace
