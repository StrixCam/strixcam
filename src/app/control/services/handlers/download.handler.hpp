#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles ListRecordings + DownloadRequest (R24, F4). ListRecordings enumerates
// MP4s from disk; DownloadRequest mints a short-lived bearer token scoped to one
// recording and returns its HTTP URL on the WiFi Direct download port.
class DownloadHandler final : public ICommandHandler {
   public:
    DownloadHandler(sst::network::DownloadServer& downloads, std::string group_owner_ip,
                    std::uint32_t download_port, std::uint64_t token_ttl_seconds);

    [[nodiscard]] auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    auto HandleList() -> sst_cam::CommandResponse;
    auto HandleDownloadRequest(const sst_cam::DownloadRequestCommand& cmd)
        -> sst_cam::CommandResponse;

    sst::network::DownloadServer& downloads_;
    std::string group_owner_ip_;
    std::uint32_t download_port_;
    std::uint64_t token_ttl_seconds_;
};

}  // namespace sst::control
