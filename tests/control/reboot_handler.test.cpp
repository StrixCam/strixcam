// Unit tests for the reboot handler (U7): invokes the injected reboot action
// and reports OK once dispatched, ERROR on failure. The real reboot side effect
// (a bounded `systemctl reboot`) is replaced here by a fake, so these run in the
// cross-compile container without rebooting anything.
#include <gtest/gtest.h>

#include "app/control/services/handlers/reboot.handler.hpp"
#include "bluetooth.pb.h"

namespace {

auto RebootCmd() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.mutable_reboot();  // selects the (empty) RebootCommand oneof case
    return cmd;
}

}  // namespace

TEST(RebootHandlerTest, HandlesRebootCase) {
    sst::control::RebootHandler handler([] { return true; });
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kReboot);
}

TEST(RebootHandlerTest, DispatchesRebootAndReportsOk) {
    int calls = 0;
    sst::control::RebootHandler handler([&calls] {
        ++calls;
        return true;
    });

    const auto resp = handler.Handle(RebootCmd());

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
}

TEST(RebootHandlerTest, RebootFailureReportsError) {
    sst::control::RebootHandler handler([] { return false; });

    const auto resp = handler.Handle(RebootCmd());

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(resp.error_message().empty());
}
