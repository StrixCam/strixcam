#pragma once

#include <string>
#include <utility>

#include "app/network/ports/uplink-store.hpp"
#include "domain/config/models/uplink-config.hpp"

namespace sst::adapters::network {

// JSON-file implementation of the IUplinkStore port: writes the uplink config
// to a path (uplink.json) via the config serde, then restricts the file to
// owner read/write (0600) so the persisted WiFi passphrase is not world-
// readable. Lives in the adapter layer so the app-layer handler need not import
// the serde header or do filesystem IO.
class JsonUplinkStore final : public sst::network::IUplinkStore {
   public:
    explicit JsonUplinkStore(std::string path) : path_(std::move(path)) {}

    auto Persist(const sst::config::UplinkData& data) -> bool override;

   private:
    std::string path_;
};

}  // namespace sst::adapters::network
