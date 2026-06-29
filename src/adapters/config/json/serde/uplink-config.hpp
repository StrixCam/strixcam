#pragma once
#include <nlohmann/json.hpp>

#include "adapters/config/json/json-opt.hpp"
#include "domain/config/models/uplink-config.hpp"

namespace sst::config {

using nlohmann::json;

inline void from_json(const json& jsonObject, EthernetUplink& values) {
    json_get_opt(jsonObject, "enabled", values.enabled);
    json_get_opt(jsonObject, "dhcp", values.dhcp);
    json_get_opt(jsonObject, "address", values.address);
    json_get_opt(jsonObject, "gateway", values.gateway);
    json_get_opt(jsonObject, "dns", values.dns);
}

inline void from_json(const json& jsonObject, WifiStaUplink& values) {
    json_get_opt(jsonObject, "enabled", values.enabled);
    json_get_opt(jsonObject, "ssid", values.ssid);
    json_get_opt(jsonObject, "passphrase", values.passphrase);
    json_get_opt(jsonObject, "dhcp", values.dhcp);
    json_get_opt(jsonObject, "address", values.address);
    json_get_opt(jsonObject, "gateway", values.gateway);
    json_get_opt(jsonObject, "dns", values.dns);
}

inline void from_json(const json& jsonObject, UplinkData& values) {
    // Each sub-object is optional; a missing section leaves its fields nullopt.
    if (jsonObject.contains("ethernet")) {
        jsonObject.at("ethernet").get_to(values.ethernet);
    }
    if (jsonObject.contains("wifi")) {
        jsonObject.at("wifi").get_to(values.wifi);
    }
}

}  // namespace sst::config
