#include <gtest/gtest.h>

#include "app/control/services/handlers/auto-white-balance.handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"

namespace {

using sst::control::AutoWhiteBalanceHandler;
using sst::processing::ColorCalibrationState;
using sst::processing::FrameColorStats;

auto AutoWbCommand() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.mutable_auto_white_balance();
    return cmd;
}

TEST(AutoWhiteBalanceHandlerTest, ComputesGreyWorldGainsFromMagentaFrame) {
    FrameColorStats stats;
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident test channel means (B, G, R, spread)
    stats.Set(100.0F, 40.0F, 100.0F, 50.0F);  // B, G, R, spread — magenta (R=B=2.5×G)
    ColorCalibrationState calib({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = false});
    AutoWhiteBalanceHandler handler(stats, calib);

    const auto resp = handler.Handle(AutoWbCommand());

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kCameraCalibration);
    // gR = G/R = 0.4, gG = 1.0 (green never boosted), gB = G/B = 0.4.
    EXPECT_NEAR(resp.camera_calibration().r_gain(), 0.4F, 0.01F);
    EXPECT_FLOAT_EQ(resp.camera_calibration().g_gain(), 1.0F);
    EXPECT_NEAR(resp.camera_calibration().b_gain(), 0.4F, 0.01F);
    EXPECT_TRUE(resp.camera_calibration().enabled());
    // Auto also sets tone: fixed saturation boost, and contrast/brightness in range.
    EXPECT_FLOAT_EQ(resp.camera_calibration().saturation(), 1.25F);
    EXPECT_GE(resp.camera_calibration().contrast(), 0.9F);
    EXPECT_LE(resp.camera_calibration().contrast(), 1.5F);

    // Applied live to the shared state.
    const auto gains = calib.Get();
    EXPECT_NEAR(gains.r, 0.4F, 0.01F);
    EXPECT_TRUE(gains.enabled);
    EXPECT_FLOAT_EQ(gains.saturation, 1.25F);
}

TEST(AutoWhiteBalanceHandlerTest, ClampsExtremeGainToSliderRange) {
    FrameColorStats stats;
    // Self-evident test channel means (R hugely dominant).
    // NOLINTNEXTLINE(readability-magic-numbers)
    stats.Set(100.0F, 40.0F, 400.0F, 50.0F);  // R hugely dominant → gR = 0.1 → clamp 0.3
    ColorCalibrationState calib({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = false});
    AutoWhiteBalanceHandler handler(stats, calib);

    const auto resp = handler.Handle(AutoWbCommand());
    EXPECT_FLOAT_EQ(resp.camera_calibration().r_gain(), 0.3F);
}

TEST(AutoWhiteBalanceHandlerTest, DarkOrNoSampleFrameKeepsCurrentGains) {
    FrameColorStats stats;  // never Set → invalid
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident preset current gains (r, g, b)
    ColorCalibrationState calib({.r = 0.7F, .g = 1.0F, .b = 0.8F, .enabled = true});
    AutoWhiteBalanceHandler handler(stats, calib);

    const auto resp = handler.Handle(AutoWbCommand());
    // No usable sample → current gains preserved, not overwritten with garbage.
    EXPECT_FLOAT_EQ(resp.camera_calibration().r_gain(), 0.7F);
    EXPECT_FLOAT_EQ(calib.Get().r, 0.7F);
}

TEST(AutoWhiteBalanceHandlerTest, HandledCasesMatchesCommand) {
    FrameColorStats stats;
    ColorCalibrationState calib({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = true});
    AutoWhiteBalanceHandler handler(stats, calib);
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kAutoWhiteBalance);
}

}  // namespace
