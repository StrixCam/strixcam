// PreviewPort/DownloadPort wrappers + formatters (U4, R2).

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <cstdint>

#include "domain/control/models/formatter/service-ports-fmt.hpp"
#include "domain/control/models/service-ports.hpp"

namespace {

using sst::control::DownloadPort;
using sst::control::PreviewPort;

constexpr std::uint32_t kPreviewPortValue = 8554;
constexpr std::uint32_t kDownloadPortValue = 8080;

TEST(ServicePortsTest, WrappersCarryValue) {
    PreviewPort preview{kPreviewPortValue};
    DownloadPort download{kDownloadPortValue};
    EXPECT_EQ(preview.value, kPreviewPortValue);
    EXPECT_EQ(download.value, kDownloadPortValue);
}

TEST(ServicePortsTest, FormattersRenderValue) {
    EXPECT_EQ(fmt::format("{}", PreviewPort{kPreviewPortValue}), "PreviewPort{8554}");
    EXPECT_EQ(fmt::format("{}", DownloadPort{kDownloadPortValue}), "DownloadPort{8080}");
}

}  // namespace
