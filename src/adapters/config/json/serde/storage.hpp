#pragma once
#include <nlohmann/json.hpp>

#include "adapters/config/json/json-opt.hpp"
#include "domain/config/models/storage.hpp"

namespace sst::config {

using nlohmann::json;

inline void from_json(const json& jsonObject, StorageData& values) {
    json_get_opt(jsonObject, "log", values.log);
    json_get_opt(jsonObject, "video", values.video);
    json_get_opt(jsonObject, "snapshots", values.snapshots);
    json_get_opt(jsonObject, "thumbnails", values.thumbnails);
    json_get_opt(jsonObject, "min_free_bytes", values.min_free_bytes);
    json_get_opt(jsonObject, "proxy_max_total_bytes", values.proxy_max_total_bytes);
}

}  // namespace sst::config
