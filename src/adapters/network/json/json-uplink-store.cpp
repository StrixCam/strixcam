#include "adapters/network/json/json-uplink-store.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#include "adapters/config/json/serde/uplink-config.hpp"  // to_json for persistence

namespace sst::adapters::network {

auto JsonUplinkStore::Persist(const sst::config::UplinkData& data) -> bool {
    const nlohmann::json json_config = data;
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::error("JsonUplinkStore: could not open {} for write", path_);
            return false;
        }
        out << json_config.dump(2) << '\n';
        if (!out) {
            spdlog::error("JsonUplinkStore: write to {} failed", path_);
            return false;
        }
    }  // close before chmod so the bytes are flushed

    // Restrict to owner rw (0600): the file holds the WiFi passphrase, so it must
    // not land world-readable with the default umask. Replace, don't OR, so a
    // pre-existing 0644 file is tightened.
    namespace fs = std::filesystem;
    std::error_code err;
    fs::permissions(path_, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, err);
    if (err) {
        spdlog::error("JsonUplinkStore: chmod 0600 on {} failed: {}", path_, err.message());
        return false;
    }
    spdlog::info("JsonUplinkStore: persisted uplink config to {} (0600)", path_);
    return true;
}

}  // namespace sst::adapters::network
