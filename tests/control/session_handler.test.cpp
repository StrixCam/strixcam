// PushSessionConfig handler: wire config -> SessionConfig snapshot. Focused on
// the state-health cycle addition (auto_stop_minutes, U2); the broader
// config-push ordering flow is covered by control_plane_integration.test.cpp.

#include <gtest/gtest.h>

#include <cstdint>

#include "app/control/services/handlers/session.handler.hpp"
#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "bluetooth.pb.h"

namespace {

using sst::control::SessionHandler;
using sst::overlay::IOverlayRenderer;
using sst::overlay::IOverlaySink;
using sst::overlay::OverlayController;
using sst::overlay::RenderScene;
using sst::overlay::RgbaImage;

// Overlay canvas size for the test controller; any small square works.
constexpr std::uint32_t kOverlayDim = 64;
// Operator-chosen auto-stop window the mapping test pushes (minutes).
constexpr std::uint32_t kAutoStopMinutes = 5;

class FakeRenderer final : public IOverlayRenderer {
   public:
    // Param order fixed by the IOverlayRenderer::Render interface contract.
    auto Render(const RenderScene& /*scene*/,
                std::uint32_t out_width,  // NOLINT(bugprone-easily-swappable-parameters)
                                          // floor-ok: test double; order fixed by
                                          // IOverlayRenderer::Render, cannot reorder in override
                std::uint32_t out_height) -> RgbaImage override {
        RgbaImage img;
        img.width = out_width;
        img.height = out_height;
        img.stride = out_width * 4;
        img.pixels.assign(static_cast<std::size_t>(img.stride) * out_height, 0);
        return img;
    }
};

class FakeSink final : public IOverlaySink {
   public:
    auto PushFrame(const RgbaImage& /*frame*/) -> void override {}
    auto Clear() -> void override {}
};

class FakeCleanup final : public sst::session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> bool override { return false; }
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
    auto ResetSelections() -> void override {}
};

struct Fixture {
    FakeCleanup cleanup;
    FakeRenderer renderer;
    FakeSink sink;
    sst::session::SessionManager manager{cleanup};
    OverlayController controller{renderer, sink, sst::common::OutputSize{kOverlayDim, kOverlayDim}};
    SessionHandler handler{manager, controller};

    Fixture() {
        manager.OnConnect();
        manager.OnWifiReady();
    }
};

auto ConfigCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("cfg-1");
    auto* config = cmd.mutable_push_session_config();
    config->set_match_uuid("match-42");
    config->set_video_output_path("");  // skip dir creation
    config->set_thumbnail_output_path("");
    return cmd;
}

// The app-chosen auto-stop window flows into the session config.
TEST(SessionHandlerTest, AutoStopMinutesMappedWhenPresent) {
    Fixture fixture;
    auto cmd = ConfigCmd();
    cmd.mutable_push_session_config()->set_auto_stop_minutes(kAutoStopMinutes);

    EXPECT_EQ(fixture.handler.Handle(cmd).status(), sst_cam::ResponseStatus::OK);

    const auto snap = fixture.manager.Snapshot();
    ASSERT_TRUE(snap.config.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(snap.config.value().auto_stop_minutes, static_cast<std::int32_t>(kAutoStopMinutes));
}

// Absent on the wire -> 0 in the config, which means "firmware default (30
// min)" inside the SessionManager — never a zero-minute timer.
TEST(SessionHandlerTest, AutoStopMinutesAbsentMapsToDefaultSentinel) {
    Fixture fixture;

    EXPECT_EQ(fixture.handler.Handle(ConfigCmd()).status(), sst_cam::ResponseStatus::OK);

    const auto snap = fixture.manager.Snapshot();
    ASSERT_TRUE(snap.config.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(snap.config.value().auto_stop_minutes, 0);
}

}  // namespace
