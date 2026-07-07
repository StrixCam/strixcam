#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "app/control/services/handlers/health-gate.hpp"
#include "app/control/services/handlers/raw-capture.handler.hpp"
#include "app/storage/ports/raw-capture-sink.hpp"
#include "bluetooth.pb.h"
#include "domain/capture/models/frame.hpp"

namespace {

using sst::control::RawCaptureHandler;

// Records the handler's calls into the sink and lets a test force Start/Stop
// failure.
class FakeRawSink final : public sst::storage::IRawCaptureSink {
   public:
    auto Start(const std::string& capture_group_id,
               const std::filesystem::path& output_dir) -> bool override {
        ++starts;
        last_group = capture_group_id;
        last_dir = output_dir;
        capturing_ = start_ok;
        return start_ok;
    }
    auto PushCamera(std::uint32_t /*camera_index*/,
                    const sst::capture::Frame& /*frame*/) -> void override {
        ++pushes;
    }
    auto Stop() -> bool override {
        ++stops;
        if (!capturing_) {
            return false;
        }
        capturing_ = false;
        return true;
    }
    [[nodiscard]] auto IsCapturing() const -> bool override { return capturing_; }

    bool start_ok{true};
    int starts{0};
    int stops{0};
    int pushes{0};
    std::string last_group;
    std::filesystem::path last_dir;

   private:
    bool capturing_{false};
};

auto RawCmd(sst_cam::RecordingAction action, const std::string& group = "") -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("r");
    auto* raw = cmd.mutable_raw_capture();
    raw->set_action(action);
    if (!group.empty()) {
        raw->set_capture_group_id(group);
    }
    return cmd;
}

TEST(RawCaptureHandlerTest, StartWithGroupIdStartsSink) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_START, "grp-7"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(sink.starts, 1);
    EXPECT_EQ(sink.last_group, "grp-7");
}

TEST(RawCaptureHandlerTest, StartWithoutGroupIdErrors) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_START));  // no group id
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(sink.starts, 0);  // sink never touched
}

TEST(RawCaptureHandlerTest, StartFailureReportsError) {
    FakeRawSink sink;
    sink.start_ok = false;
    RawCaptureHandler handler(sink);
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_START, "g"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(sink.starts, 1);
}

TEST(RawCaptureHandlerTest, StopStopsSink) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    handler.Handle(RawCmd(sst_cam::RECORDING_START, "g"));
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_STOP));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(sink.stops, 1);
}

TEST(RawCaptureHandlerTest, StopWithNoActiveCaptureErrors) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_STOP));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
}

TEST(RawCaptureHandlerTest, PauseAndResumeAreUnsupported) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    EXPECT_EQ(handler.Handle(RawCmd(sst_cam::RECORDING_PAUSE)).status(),
              sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(handler.Handle(RawCmd(sst_cam::RECORDING_RESUME)).status(),
              sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(sink.starts, 0);
    EXPECT_EQ(sink.stops, 0);
}

TEST(RawCaptureHandlerTest, UnsetActionIsErrorNeverStart) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    // mutable_raw_capture() with no set_action(): has_action() is false, which
    // the proto3 default would otherwise read as RECORDING_ACTION_UNKNOWN.
    sst_cam::Command cmd;
    cmd.set_correlation_id("r");
    cmd.mutable_raw_capture()->set_capture_group_id("g");  // group present, action absent
    auto resp = handler.Handle(cmd);
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(sink.starts, 0);  // absent action MUST NOT start
}

TEST(RawCaptureHandlerTest, HandledCasesIsRawCapture) {
    FakeRawSink sink;
    RawCaptureHandler handler(sink);
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kRawCapture);
}

// U3 firmware-side gating: raw-capture START while any camera is not OK is
// refused with DEVICE_INOPERABLE before the sink is touched (raw dual capture
// needs BOTH cameras delivering).
TEST(RawCaptureHandlerTest, StartWhileCameraRecoveringRejectedDeviceInoperable) {
    FakeRawSink sink;
    const sst::control::StartHealthGate gate{
        []() -> std::optional<sst_cam::CameraHealth> { return sst_cam::CAMERA_HEALTH_OK; },
        []() -> std::optional<sst_cam::CameraHealth> { return sst_cam::CAMERA_HEALTH_RECOVERING; }};
    RawCaptureHandler handler(sink, gate);

    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_START, "grp-9"));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::DEVICE_INOPERABLE);
    EXPECT_FALSE(resp.error_message().empty());
    EXPECT_EQ(sink.starts, 0);  // sink never touched
}

// U3: STOP is never gated — footage retrieval must survive a DOWN camera.
TEST(RawCaptureHandlerTest, StopWhileCameraDownAccepted) {
    FakeRawSink sink;
    std::atomic<sst_cam::CameraHealth> health{sst_cam::CAMERA_HEALTH_OK};
    const sst::control::StartHealthGate gate{
        [&health]() -> std::optional<sst_cam::CameraHealth> { return health.load(); },
        []() -> std::optional<sst_cam::CameraHealth> { return sst_cam::CAMERA_HEALTH_OK; }};
    RawCaptureHandler handler(sink, gate);
    ASSERT_EQ(handler.Handle(RawCmd(sst_cam::RECORDING_START, "grp-10")).status(),
              sst_cam::ResponseStatus::OK);

    health = sst_cam::CAMERA_HEALTH_DOWN;  // camera dies mid-capture
    auto resp = handler.Handle(RawCmd(sst_cam::RECORDING_STOP));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(sink.stops, 1);
}

}  // namespace
