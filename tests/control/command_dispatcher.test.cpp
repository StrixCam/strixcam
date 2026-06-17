// CommandDispatcher behaviour (U3, R4/R5/R6/R8a, AE1).
//
// Pure — no transport, no hardware. Exercises the always-respond contract:
// every command yields exactly one CommandResponse with a matching
// correlation_id; unknown commands return UNSUPPORTED; throwing handlers
// return ERROR.

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "bluetooth.pb.h"

namespace {

using sst::control::CommandDispatcher;
using sst::control::ICommandHandler;

// Handles get_device_info → OK with a device_info payload.
class DeviceInfoHandler final : public ICommandHandler {
   public:
    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override {
        return {sst_cam::Command::kGetDeviceInfo};
    }
    auto Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse override {
        sst_cam::CommandResponse resp;
        resp.set_status(sst_cam::ResponseStatus::OK);
        resp.mutable_device_info()->set_device_id("dev-1");
        return resp;
    }
};

// Handles recording_control by throwing — simulates an internal failure.
class ThrowingHandler final : public ICommandHandler {
   public:
    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override {
        return {sst_cam::Command::kRecordingControl};
    }
    auto Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse override {
        throw std::runtime_error("recorder exploded");
    }
};

// Handles score_update by reporting an invalid argument as ERROR — the handler
// boundary where a domain status maps onto ResponseStatus (R5).
class InvalidArgHandler final : public ICommandHandler {
   public:
    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override {
        return {sst_cam::Command::kScoreUpdate};
    }
    auto Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse override {
        sst_cam::CommandResponse resp;
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("unknown team_id");
        return resp;
    }
};

auto MakeDispatcher() -> CommandDispatcher {
    CommandDispatcher dispatcher;
    dispatcher.Register(std::make_shared<DeviceInfoHandler>());
    dispatcher.Register(std::make_shared<ThrowingHandler>());
    dispatcher.Register(std::make_shared<InvalidArgHandler>());
    return dispatcher;
}

// AE1 / R6: a command whose capability isn't wired returns UNSUPPORTED with the
// same correlation_id and no payload — never silence.
TEST(CommandDispatcherTest, UnregisteredCommandReturnsUnsupported) {
    auto dispatcher = MakeDispatcher();
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-unsupported");
    cmd.mutable_list_recordings();  // no handler registered for this case

    auto resp = dispatcher.Dispatch(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "corr-unsupported");
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::PAYLOAD_NOT_SET);
}

// An empty command (no oneof set) is also UNSUPPORTED, not a crash.
TEST(CommandDispatcherTest, EmptyCommandReturnsUnsupported) {
    auto dispatcher = MakeDispatcher();
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-empty");

    auto resp = dispatcher.Dispatch(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "corr-empty");
}

// R4: a recognized command returns exactly one OK response with matching id.
TEST(CommandDispatcherTest, RegisteredCommandReturnsOkWithEcho) {
    auto dispatcher = MakeDispatcher();
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-ok");
    cmd.mutable_get_device_info();

    auto resp = dispatcher.Dispatch(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.correlation_id(), "corr-ok");
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kDeviceInfo);
    EXPECT_EQ(resp.device_info().device_id(), "dev-1");
}

// R6: a handler that throws yields ERROR with a non-empty error_message.
TEST(CommandDispatcherTest, ThrowingHandlerReturnsError) {
    auto dispatcher = MakeDispatcher();
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-throw");
    cmd.mutable_recording_control()->set_action(sst_cam::RecordingAction::RECORDING_START);

    auto resp = dispatcher.Dispatch(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(resp.correlation_id(), "corr-throw");
    EXPECT_FALSE(resp.error_message().empty());
}

// R5: a handler reporting invalid-argument surfaces as ERROR + message, with
// the correlation_id still enforced by the dispatcher.
TEST(CommandDispatcherTest, HandlerInvalidArgumentMapsToError) {
    auto dispatcher = MakeDispatcher();
    sst_cam::Command cmd;
    cmd.set_correlation_id("corr-badarg");
    cmd.mutable_score_update()->set_team_id("nope");

    auto resp = dispatcher.Dispatch(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_EQ(resp.correlation_id(), "corr-badarg");
    EXPECT_EQ(resp.error_message(), "unknown team_id");
}

// Two commands with distinct correlation_ids get responses with the respective
// ids — no cross-talk.
TEST(CommandDispatcherTest, DistinctCorrelationIdsDoNotCrossTalk) {
    auto dispatcher = MakeDispatcher();

    sst_cam::Command cmd_a;
    cmd_a.set_correlation_id("aaa");
    cmd_a.mutable_get_device_info();
    sst_cam::Command cmd_b;
    cmd_b.set_correlation_id("bbb");
    cmd_b.mutable_list_recordings();

    auto resp_a = dispatcher.Dispatch(cmd_a);
    auto resp_b = dispatcher.Dispatch(cmd_b);

    EXPECT_EQ(resp_a.correlation_id(), "aaa");
    EXPECT_EQ(resp_a.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp_b.correlation_id(), "bbb");
    EXPECT_EQ(resp_b.status(), sst_cam::ResponseStatus::UNSUPPORTED);
}

}  // namespace
