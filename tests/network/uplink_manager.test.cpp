#include <gtest/gtest.h>

#include "app/network/ports/uplink-configurator.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "domain/config/models/uplink-config.hpp"

namespace {

// Records which interfaces the manager drove + returns canned results, so the
// orchestration (enabled→apply, disabled→skip) is tested without NetworkManager.
class FakeConfigurator final : public sst::network::IUplinkConfigurator {
   public:
    auto ApplyEthernet(const sst::config::EthernetUplink& cfg)
        -> sst::network::UplinkResult override {
        ++ethernet_calls;
        last_eth_dhcp = cfg.dhcp.value_or(true);
        return {.ok = true, .detail = "10.10.1.30/24"};
    }
    auto ApplyWifiSta(const sst::config::WifiStaUplink& /*cfg*/)
        -> sst::network::UplinkResult override {
        ++wifi_calls;
        return {.ok = false, .detail = "unavailable"};
    }

    int ethernet_calls{0};
    int wifi_calls{0};
    bool last_eth_dhcp{true};
};

TEST(UplinkManagerTest, AppliesEnabledEthernetSkipsDisabledWifi) {
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);

    sst::config::UplinkData cfg;
    cfg.ethernet.enabled = true;
    cfg.ethernet.dhcp = false;
    cfg.wifi.enabled = false;

    const auto status = manager.Apply(cfg);

    EXPECT_EQ(configurator.ethernet_calls, 1);
    EXPECT_EQ(configurator.wifi_calls, 0);  // disabled → skipped
    EXPECT_FALSE(configurator.last_eth_dhcp);
    EXPECT_TRUE(status.ethernet_enabled);
    EXPECT_TRUE(status.ethernet.ok);
    EXPECT_EQ(status.ethernet.detail, "10.10.1.30/24");
    EXPECT_FALSE(status.wifi_enabled);
}

TEST(UplinkManagerTest, BothDisabledDrivesNothing) {
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);

    sst::config::UplinkData cfg;  // both default-disabled (nullopt → false)

    const auto status = manager.Apply(cfg);

    EXPECT_EQ(configurator.ethernet_calls, 0);
    EXPECT_EQ(configurator.wifi_calls, 0);
    EXPECT_FALSE(status.ethernet_enabled);
    EXPECT_FALSE(status.wifi_enabled);
}

// AE5 / R6: with both configured but wifi deactivated, only ethernet is driven.
TEST(UplinkManagerTest, WifiEnabledIsDrivenButReportsGatedUnavailable) {
    FakeConfigurator configurator;
    sst::network::UplinkManager manager(configurator);

    sst::config::UplinkData cfg;
    cfg.ethernet.enabled = true;
    cfg.wifi.enabled = true;

    const auto status = manager.Apply(cfg);

    EXPECT_EQ(configurator.ethernet_calls, 1);
    EXPECT_EQ(configurator.wifi_calls, 1);
    EXPECT_TRUE(status.wifi_enabled);
    EXPECT_FALSE(status.wifi.ok);  // gated on single-radio hardware
}

}  // namespace
