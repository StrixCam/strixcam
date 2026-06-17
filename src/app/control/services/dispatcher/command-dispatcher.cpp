#include "app/control/services/dispatcher/command-dispatcher.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace sst::control {

auto CommandDispatcher::Register(std::shared_ptr<ICommandHandler> handler) -> void {
    if (!handler) {
        spdlog::warn("CommandDispatcher::Register: null handler ignored");
        return;
    }
    for (const auto payload_case : handler->HandledCases()) {
        const int key = static_cast<int>(payload_case);
        auto [it, inserted] = handlers_.try_emplace(key, handler);
        if (!inserted) {
            spdlog::warn(
                "CommandDispatcher::Register: payload case {} already registered — "
                "ignoring duplicate",
                key);
        }
    }
}

auto CommandDispatcher::Dispatch(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    const auto payload_case = cmd.payload_case();

    if (payload_case == sst_cam::Command::PAYLOAD_NOT_SET) {
        sst_cam::CommandResponse resp;
        resp.set_correlation_id(cmd.correlation_id());
        resp.set_status(sst_cam::ResponseStatus::UNSUPPORTED);
        resp.set_error_message("empty command: no payload set");
        spdlog::warn("CommandDispatcher: empty command (corr={})", cmd.correlation_id());
        return resp;
    }

    const auto entry = handlers_.find(static_cast<int>(payload_case));
    if (entry == handlers_.end()) {
        sst_cam::CommandResponse resp;
        resp.set_correlation_id(cmd.correlation_id());
        resp.set_status(sst_cam::ResponseStatus::UNSUPPORTED);
        resp.set_error_message("command not supported on this firmware");
        spdlog::info("CommandDispatcher: UNSUPPORTED payload case {} (corr={})",
                     static_cast<int>(payload_case), cmd.correlation_id());
        return resp;
    }

    try {
        sst_cam::CommandResponse resp = entry->second->Handle(cmd);
        // Enforce the echo regardless of what the handler set (R5).
        resp.set_correlation_id(cmd.correlation_id());
        return resp;
    } catch (const std::exception& ex) {
        sst_cam::CommandResponse resp;
        resp.set_correlation_id(cmd.correlation_id());
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message(ex.what());
        spdlog::error("CommandDispatcher: handler threw for payload case {} (corr={}): {}",
                      static_cast<int>(payload_case), cmd.correlation_id(), ex.what());
        return resp;
    }
}

}  // namespace sst::control
