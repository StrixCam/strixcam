// BLE connect/disconnect ordering: the generation-validated supervisor drops a
// stale disconnect when a newer connect superseded it (StopNotify + immediate
// resubscribe must never deliver OnDisconnect after OnConnect and re-arm the
// auto-stop the reconnect just cancelled). Pure — no D-Bus.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "adapters/control/ble/bluez/connection-supervisor.hpp"

namespace {

using sst::adapters::control::ConnectionSupervisor;

TEST(ConnectionSupervisorTest, FirstWriteDeliversConnectOnce) {
    ConnectionSupervisor supervisor;
    int connects = 0;
    EXPECT_TRUE(supervisor.OnWrite([&connects] { ++connects; }));
    EXPECT_FALSE(supervisor.OnWrite([&connects] { ++connects; }));  // same central
    EXPECT_EQ(connects, 1);
    EXPECT_TRUE(supervisor.CentralPresent());
}

TEST(ConnectionSupervisorTest, NormalDisconnectDelivers) {
    ConnectionSupervisor supervisor;
    supervisor.OnWrite(nullptr);

    const auto ticket = supervisor.OnCentralGone();
    ASSERT_TRUE(ticket.has_value());
    EXPECT_FALSE(supervisor.CentralPresent());

    int disconnects = 0;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_TRUE(supervisor.DeliverDisconnect(*ticket, [&disconnects] { ++disconnects; }));
    EXPECT_EQ(disconnects, 1);
}

TEST(ConnectionSupervisorTest, GoneWithoutCentralYieldsNoTicket) {
    ConnectionSupervisor supervisor;
    EXPECT_FALSE(supervisor.OnCentralGone().has_value());
}

// Abrupt central loss delivers TWO gone signals for one disconnect (GATT
// StopNotify and Device1 Connected=false, in either order). Only the first
// claims the ticket; the second is a no-op, so the session-level disconnect
// is delivered exactly once.
TEST(ConnectionSupervisorTest, DualGoneSignalsClaimOneTicket) {
    ConnectionSupervisor supervisor;
    supervisor.OnWrite(nullptr);

    const auto first = supervisor.OnCentralGone();   // e.g. StopNotify
    const auto second = supervisor.OnCentralGone();  // e.g. Connected=false
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(second.has_value());

    int disconnects = 0;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_TRUE(supervisor.DeliverDisconnect(*first, [&disconnects] { ++disconnects; }));
    EXPECT_EQ(disconnects, 1);
}

// The core race: StopNotify claims a ticket, the central resubscribes + writes
// (new connect) BEFORE the detached worker delivers — the stale disconnect must
// be dropped, never delivered after the newer connect.
TEST(ConnectionSupervisorTest, StaleDisconnectAfterReconnectIsDropped) {
    ConnectionSupervisor supervisor;
    std::vector<std::string> order;

    supervisor.OnWrite([&order] { order.emplace_back("connect-1"); });
    const auto ticket = supervisor.OnCentralGone();
    ASSERT_TRUE(ticket.has_value());

    // Reconnect lands before the disconnect worker runs.
    EXPECT_TRUE(supervisor.OnWrite([&order] { order.emplace_back("connect-2"); }));

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_FALSE(supervisor.DeliverDisconnect(
        *ticket, [&order] { order.emplace_back("disconnect-stale"); }));

    ASSERT_EQ(order.size(), 2U);
    EXPECT_EQ(order[0], "connect-1");
    EXPECT_EQ(order[1], "connect-2");
    EXPECT_TRUE(supervisor.CentralPresent());
}

// A full disconnect/reconnect/disconnect sequence: each disconnect ticket is
// valid exactly for its own generation.
TEST(ConnectionSupervisorTest, TicketsAreGenerationScoped) {
    ConnectionSupervisor supervisor;

    supervisor.OnWrite(nullptr);
    const auto first = supervisor.OnCentralGone();
    ASSERT_TRUE(first.has_value());
    supervisor.OnWrite(nullptr);
    const auto second = supervisor.OnCentralGone();
    ASSERT_TRUE(second.has_value());

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_FALSE(supervisor.DeliverDisconnect(*first, nullptr));  // superseded
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_TRUE(supervisor.DeliverDisconnect(*second, nullptr));  // current
}

}  // namespace
