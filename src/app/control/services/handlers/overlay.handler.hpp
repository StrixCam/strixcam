#pragma once

#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-manager.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles PushOverlayLayoutCommand: maps the proto layout into the overlay
// controller, advances the session to Ready, and renders the initial overlay.
// Can be re-sent mid-session to change the design.
class OverlayHandler final : public ICommandHandler {
   public:
    using Clock = std::function<std::uint64_t()>;

    OverlayHandler(sst::session::ISessionManager& session,
                   sst::overlay::OverlayController& controller, Clock now_ms);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    sst::session::ISessionManager& session_;
    sst::overlay::OverlayController& controller_;
    Clock now_ms_;
};

}  // namespace sst::control
