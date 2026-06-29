#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "app/control/services/handlers/network.handler.hpp"
#include "app/network/ports/uplink-configurator.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "bluetooth.pb.h"
#include "domain/config/models/uplink-config.hpp"

namespace {

namespace fs = std::filesystem;

class FakeConfigurator final : public sst::network::IUplinkConfigurator {
   public:
    auto ApplyEthernet(const sst::config::EthernetUplink& /*cfg*/)
        -> sst::network::UplinkResult override {
        ++ethernet_calls;
        return {.ok = true, .detail = "10.10.1.30/24"};
    }
    auto ApplyWifiSta(const sst::config::WifiStaUplink& /*cfg*/)
        -> sst::network::UplinkResult override {
        return {.ok = false, .detail = "unavailable"};
    }
    int ethernet_calls{0};
};

TEST(NetworkHandlerTest, HandlesSetAndGet) {
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);
    sst::control::NetworkHandler handler(manager, "/tmp/ignored.json", {}, {});

    const auto cases = handler.HandledCases();
    EXPECT_NE(std::find(cases.begin(), cases.end(), sst_cam::Command::kSetNetworkConfig),
              cases.end());
    EXPECT_NE(std::find(cases.begin(), cases.end(), sst_cam::Command::kGetNetworkConfig),
              cases.end());
}

TEST(NetworkHandlerTest, SetAppliesPersistsAndEchoesStatus) {
    const fs::path path = fs::path{::testing::TempDir()} / "uplink-handler-test.json";
    fs::remove(path);
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);
    sst::control::NetworkHandler handler(manager, path.string(), {}, {});

    sst_cam::Command cmd;
    auto* eth = cmd.mutable_set_network_config()->mutable_config()->mutable_ethernet();
    eth->set_enabled(true);
    eth->set_dhcp(false);
    eth->set_address("10.10.1.30/24");
    eth->set_gateway("10.10.1.1");

    const auto resp = handler.Handle(cmd);

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kNetworkConfig);
    // Config echoed back.
    const auto& echoed = resp.network_config().config().ethernet();
    EXPECT_TRUE(echoed.enabled());
    EXPECT_FALSE(echoed.dhcp());
    EXPECT_EQ(echoed.address(), "10.10.1.30/24");
    EXPECT_EQ(echoed.gateway(), "10.10.1.1");
    // Live status from the apply.
    EXPECT_TRUE(resp.network_config().ethernet_up());
    EXPECT_EQ(resp.network_config().ethernet_address(), "10.10.1.30/24");
    // Applied + persisted.
    EXPECT_EQ(configurator.ethernet_calls, 1);
    EXPECT_TRUE(fs::exists(path));

    fs::remove(path);
}

TEST(NetworkHandlerTest, GetReturnsInitialConfigWithoutApplying) {
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);

    sst::config::UplinkData initial;
    initial.ethernet.enabled = true;
    initial.ethernet.address = "192.168.0.5/24";
    sst::network::UplinkStatus initial_status;
    initial_status.ethernet = {.ok = true, .detail = "192.168.0.5/24"};

    sst::control::NetworkHandler handler(manager, "/tmp/ignored.json", initial, initial_status);

    sst_cam::Command cmd;
    cmd.mutable_get_network_config();
    const auto resp = handler.Handle(cmd);

    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kNetworkConfig);
    EXPECT_TRUE(resp.network_config().config().ethernet().enabled());
    EXPECT_EQ(resp.network_config().config().ethernet().address(), "192.168.0.5/24");
    EXPECT_TRUE(resp.network_config().ethernet_up());
    EXPECT_EQ(configurator.ethernet_calls, 0);  // Get does not re-apply
}

}  // namespace
