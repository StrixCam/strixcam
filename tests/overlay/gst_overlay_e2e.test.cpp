// GStreamer overlay compositor (U9, R20). Hardware-bound: needs working
// GStreamer plugins (appsrc/compositor) — expected to FAIL in the cross-compile
// container, passes on-device.

#include <gtest/gtest.h>

#include "adapters/overlay/gstreamer/gst-overlay-compositor.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace {

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;
constexpr std::uint32_t kRgbaBytesPerPixel = 4;
constexpr std::uint8_t kFillByte = 0x80;  // mid-gray test fill

TEST(GstOverlayE2E, AppSrcAcceptsRgbaPush) {
    sst::adapters::overlay::GstOverlayCompositor compositor(
        sst::common::OutputSize{kWidth, kHeight});
    ASSERT_NE(compositor.AppSrc(), nullptr);

    sst::overlay::RgbaImage frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.stride = kWidth * kRgbaBytesPerPixel;
    frame.pixels.assign(static_cast<std::size_t>(frame.stride) * kHeight, kFillByte);

    // On-device this pushes a buffer into the live appsrc without error.
    compositor.PushFrame(frame);
    SUCCEED();
}

}  // namespace
