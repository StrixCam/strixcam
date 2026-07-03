#include <gtest/gtest.h>

#include "app/control/services/handlers/active-camera.handler.hpp"
#include "bluetooth.pb.h"
#include "domain/decision/models/manual-camera-state.hpp"

namespace {

using sst::control::ActiveCameraHandler;
using sst::decision::ManualCameraState;

TEST(ActiveCameraHandlerTest, WritesSelectionAndEchoes) {
    ManualCameraState state;
    ActiveCameraHandler handler(state);

    sst_cam::Command cmd;
    cmd.mutable_set_active_camera()->set_camera_index(1);
    const auto resp = handler.Handle(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kActiveCamera);
    EXPECT_EQ(resp.active_camera().camera_index(), 1U);
    EXPECT_EQ(state.Get(), 1U);
}

TEST(ActiveCameraHandlerTest, HandledCasesMatchesCommand) {
    ManualCameraState state;
    ActiveCameraHandler handler(state);
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kSetActiveCamera);
}

}  // namespace
