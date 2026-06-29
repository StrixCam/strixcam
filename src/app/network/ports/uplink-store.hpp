#pragma once

#include "domain/config/models/uplink-config.hpp"

namespace sst::network {

// Port for persisting the camera's uplink config (R8: survives restart). Lets
// the app-layer NetworkHandler save the app-pushed config without depending on
// the JSON serde / filesystem (those live in the adapter layer — app → ports/
// domain only). The concrete adapter writes uplink.json with owner-only perms
// so the WiFi passphrase is not world-readable.
class IUplinkStore {
   public:
    IUplinkStore() = default;
    virtual ~IUplinkStore() = default;

    IUplinkStore(const IUplinkStore&) = delete;
    auto operator=(const IUplinkStore&) -> IUplinkStore& = delete;
    IUplinkStore(IUplinkStore&&) = delete;
    auto operator=(IUplinkStore&&) -> IUplinkStore& = delete;

    // Persist `data` (best-effort). Returns true iff it was written durably with
    // the restricted file mode; false (and logs) on any IO/permission failure.
    virtual auto Persist(const sst::config::UplinkData& data) -> bool = 0;
};

}  // namespace sst::network
