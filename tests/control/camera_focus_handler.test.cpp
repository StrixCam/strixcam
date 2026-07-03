#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "app/control/services/handlers/camera-focus.handler.hpp"
#include "app/focus/ports/focuser.hpp"
#include "bluetooth.pb.h"
#include "domain/focus/models/focus-state.hpp"

namespace {

using sst::control::CameraFocusHandler;
using sst::focus::FocusState;

// Records focus writes + reports a configurable availability.
class FakeFocuser final : public sst::focus::IFocuser {
   public:
    explicit FakeFocuser(bool available) : available_(available) {}
    auto SetFocus(std::uint32_t camera_index, std::uint32_t position) -> bool override {
        writes.emplace_back(camera_index, position);
        return available_;
    }
    [[nodiscard]] auto IsAvailable() const -> bool override { return available_; }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;

   private:
    bool available_;
};

auto ManualFocus(std::uint32_t camera, std::uint32_t position) -> sst_cam::Command {
    sst_cam::Command cmd;
    auto* payload = cmd.mutable_camera_focus();
    payload->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_MANUAL);
    payload->set_focus_position(position);
    payload->set_camera_index(camera);
    return cmd;
}

TEST(CameraFocusHandlerTest, ManualDrivesVcmAndRecordsState) {
    FakeFocuser focuser(true);
    FocusState state;
    CameraFocusHandler handler(focuser, state);

    const auto resp = handler.Handle(ManualFocus(1, 640));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kCameraFocus);
    EXPECT_TRUE(resp.camera_focus().autofocus_available());
    EXPECT_EQ(resp.camera_focus().focus_position(), 640U);
    // VCM driven for camera 1 only; state records manual + position.
    ASSERT_EQ(focuser.writes.size(), 1U);
    EXPECT_EQ(focuser.writes[0].first, 1U);
    EXPECT_EQ(focuser.writes[0].second, 640U);
    EXPECT_FALSE(state.IsAuto(1));
    EXPECT_EQ(state.Position(1), 640U);
}

TEST(CameraFocusHandlerTest, NoCameraIndexAppliesToBoth) {
    FakeFocuser focuser(true);
    FocusState state;
    CameraFocusHandler handler(focuser, state);

    sst_cam::Command cmd;
    auto* payload = cmd.mutable_camera_focus();
    payload->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_MANUAL);
    payload->set_focus_position(300);
    handler.Handle(cmd);  // no camera_index → both

    EXPECT_EQ(focuser.writes.size(), 2U);  // cam0 + cam1
    EXPECT_EQ(state.Position(0), 300U);
    EXPECT_EQ(state.Position(1), 300U);
}

TEST(CameraFocusHandlerTest, AutoFlipsStateAndDoesNotDriveVcm) {
    FakeFocuser focuser(true);
    FocusState state;
    CameraFocusHandler handler(focuser, state);

    sst_cam::Command cmd;
    cmd.mutable_camera_focus()->set_mode(sst_cam::CameraFocusMode::FOCUS_MODE_AUTO);
    cmd.mutable_camera_focus()->set_camera_index(0);
    handler.Handle(cmd);

    EXPECT_TRUE(state.IsAuto(0));
    EXPECT_TRUE(focuser.writes.empty());  // AUTO hands off to the AF loop
}

TEST(CameraFocusHandlerTest, UnavailableFocuserStillSucceeds) {
    FakeFocuser focuser(false);  // fixed lens
    FocusState state;
    CameraFocusHandler handler(focuser, state);

    const auto resp = handler.Handle(ManualFocus(0, 500));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);  // never fails
    EXPECT_FALSE(resp.camera_focus().autofocus_available());
}

}  // namespace
