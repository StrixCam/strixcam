#include <gtest/gtest.h>

#include <cstddef>

#include "app/control/services/handlers/camera-calibration.handler.hpp"
#include "bluetooth.pb.h"
#include "domain/processing/models/auto-color-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"

namespace {

using sst::control::CameraCalibrationHandler;
using sst::processing::AutoColorMode;
using sst::processing::AutoColorState;
using sst::processing::ColorCalibrationState;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) floor-ok: test helper
auto MakeCommand(float red, float green, float blue, bool enabled) -> sst_cam::Command {
    sst_cam::Command cmd;
    auto* payload = cmd.mutable_set_camera_calibration();
    payload->set_r_gain(red);
    payload->set_g_gain(green);
    payload->set_b_gain(blue);
    payload->set_enabled(enabled);
    return cmd;
}

TEST(CameraCalibrationHandlerTest, HandledCasesMatchesCommand) {
    ColorCalibrationState state({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = true});
    AutoColorState auto_color;
    CameraCalibrationHandler handler(state, auto_color);
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kSetCameraCalibration);
}

TEST(CameraCalibrationHandlerTest, WritesSharedStateAndEchoesGains) {
    ColorCalibrationState state({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = true});
    AutoColorState auto_color;
    CameraCalibrationHandler handler(state, auto_color);

    const auto resp = handler.Handle(MakeCommand(0.8F, 1.1F, 0.7F, true));

    // Response echoes the applied gains for slider confirmation.
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kCameraCalibration);
    EXPECT_FLOAT_EQ(resp.camera_calibration().r_gain(), 0.8F);
    EXPECT_FLOAT_EQ(resp.camera_calibration().g_gain(), 1.1F);
    EXPECT_FLOAT_EQ(resp.camera_calibration().b_gain(), 0.7F);
    EXPECT_TRUE(resp.camera_calibration().enabled());

    // Shared state updated for the postprocessor to sample next frame — EVERY
    // camera (one slider set applies rig-wide).
    for (std::size_t camera = 0; camera < ColorCalibrationState::kCameras; ++camera) {
        const auto gains = state.Get(camera);
        EXPECT_FLOAT_EQ(gains.r, 0.8F);
        EXPECT_FLOAT_EQ(gains.g, 1.1F);
        EXPECT_FLOAT_EQ(gains.b, 0.7F);
        EXPECT_TRUE(gains.enabled);
    }
    // A user calibration with enabled=true preempts the continuous auto-WB loop.
    EXPECT_EQ(auto_color.Mode(), AutoColorMode::kManual);
}

TEST(CameraCalibrationHandlerTest, DisableResumesAutoColorLoop) {
    ColorCalibrationState state({.r = 1.0F, .g = 1.0F, .b = 1.0F, .enabled = true});
    AutoColorState auto_color;
    CameraCalibrationHandler handler(state, auto_color);

    constexpr float kManualGain = 0.8F;
    handler.Handle(MakeCommand(kManualGain, 1.0F, kManualGain, true));
    ASSERT_EQ(auto_color.Mode(), AutoColorMode::kManual);

    // enabled=false hands color authority back to the auto loop.
    handler.Handle(MakeCommand(1.0F, 1.0F, 1.0F, false));
    EXPECT_EQ(auto_color.Mode(), AutoColorMode::kAuto);
}

TEST(CameraCalibrationHandlerTest, DisabledFlagPropagates) {
    // NOLINTNEXTLINE(readability-magic-numbers) — self-evident calibration gains
    ColorCalibrationState state({.r = 0.8F, .g = 1.0F, .b = 0.8F, .enabled = true});
    AutoColorState auto_color;
    CameraCalibrationHandler handler(state, auto_color);

    handler.Handle(MakeCommand(1.0F, 1.0F, 1.0F, false));

    EXPECT_FALSE(state.Get().enabled);
}

}  // namespace
