#include "app/network/services/uplink-manager/uplink-manager.hpp"

#include <spdlog/spdlog.h>

namespace sst::network {

auto UplinkManager::Apply(const sst::config::UplinkData& cfg) -> UplinkStatus {
    UplinkStatus status;

    status.ethernet_enabled = cfg.ethernet.enabled.value_or(false);
    if (status.ethernet_enabled) {
        status.ethernet = configurator_.ApplyEthernet(cfg.ethernet);
        spdlog::info("UplinkManager: ethernet uplink {} ({})", status.ethernet.ok ? "up" : "FAILED",
                     status.ethernet.detail);
    } else {
        spdlog::info("UplinkManager: ethernet uplink disabled — skipped");
    }

    status.wifi_enabled = cfg.wifi.enabled.value_or(false);
    if (status.wifi_enabled) {
        status.wifi = configurator_.ApplyWifiSta(cfg.wifi);
        spdlog::info("UplinkManager: wifi-STA uplink {} ({})",
                     status.wifi.ok ? "up" : "unavailable", status.wifi.detail);
    } else {
        spdlog::info("UplinkManager: wifi-STA uplink disabled — skipped");
    }

    return status;
}

}  // namespace sst::network
