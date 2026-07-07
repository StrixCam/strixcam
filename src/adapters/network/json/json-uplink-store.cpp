#include "adapters/network/json/json-uplink-store.hpp"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "adapters/common/fs/atomic-file-write.hpp"
#include "adapters/config/json/serde/uplink-config.hpp"  // to_json for persistence

namespace sst::adapters::network {

namespace {
// Owner rw only (0600) from the very first byte: the file holds the WiFi
// passphrase. Creating with the default umask and chmod-ing afterwards leaves
// a window where the secret sits world-readable; WriteFileAtomic applies this
// mode at open(O_CREAT) time instead.
constexpr mode_t kOwnerRwMode = 0600;
}  // namespace

auto JsonUplinkStore::Persist(const sst::config::UplinkData& data) -> bool {
    const nlohmann::json json_config = data;
    // Atomic replace (0600 tmp + fsync + rename): a crash mid-write can't
    // corrupt the previous good config, and a pre-existing wider-mode file is
    // tightened because the rename swaps in the 0600 inode wholesale.
    if (!sst::adapters::fs_common::WriteFileAtomic(path_, json_config.dump(2) + '\n',
                                                   kOwnerRwMode)) {
        spdlog::error("JsonUplinkStore: persist to {} failed", path_);
        return false;
    }
    spdlog::info("JsonUplinkStore: persisted uplink config to {} (0600)", path_);
    return true;
}

}  // namespace sst::adapters::network
