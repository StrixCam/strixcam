#include "app/control/services/handlers/reboot.handler.hpp"

#include <utility>

namespace sst::control {

RebootHandler::RebootHandler(RebootFn reboot) : reboot_(std::move(reboot)) {}

auto RebootHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kReboot};
}

auto RebootHandler::Handle(const sst_cam::Command& /*cmd*/) -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    const bool dispatched = reboot_ && reboot_();
    if (dispatched) {
        // OK signals the reboot was dispatched; the device goes down right after,
        // so the app may not see this echo — that's expected.
        resp.set_status(sst_cam::ResponseStatus::OK);
    } else {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("reboot failed");
    }
    return resp;
}

}  // namespace sst::control
