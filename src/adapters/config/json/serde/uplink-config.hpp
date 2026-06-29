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

// Serialization for persistence (R8): the network handler writes the
// app-pushed uplink config back to uplink.json so it survives restart. Only
// engaged optionals are written, so a partial config stays partial on disk.
inline void put_opt(json& jsonObject, const char* key, const std::optional<bool>& value) {
    if (value.has_value()) {
        jsonObject[key] = *value;
    }
}
inline void put_opt(json& jsonObject, const char* key, const std::optional<std::string>& value) {
    if (value.has_value()) {
        jsonObject[key] = *value;
    }
}

inline void to_json(json& jsonObject, const EthernetUplink& values) {
    put_opt(jsonObject, "enabled", values.enabled);
    put_opt(jsonObject, "dhcp", values.dhcp);
    put_opt(jsonObject, "address", values.address);
    put_opt(jsonObject, "gateway", values.gateway);
    put_opt(jsonObject, "dns", values.dns);
}

inline void to_json(json& jsonObject, const WifiStaUplink& values) {
    put_opt(jsonObject, "enabled", values.enabled);
    put_opt(jsonObject, "ssid", values.ssid);
    put_opt(jsonObject, "passphrase", values.passphrase);
    put_opt(jsonObject, "dhcp", values.dhcp);
    put_opt(jsonObject, "address", values.address);
    put_opt(jsonObject, "gateway", values.gateway);
    put_opt(jsonObject, "dns", values.dns);
}

inline void to_json(json& jsonObject, const UplinkData& values) {
    jsonObject["ethernet"] = values.ethernet;
    jsonObject["wifi"] = values.wifi;
}

}  // namespace sst::config
