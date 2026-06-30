#pragma once

#include <functional>
#include <vector>

#include "app/control/ports/handler.hpp"
#include "bluetooth.pb.h"

namespace sst::control {

// Handles RebootCommand (U7): invokes the injected reboot action and replies OK
// once the reboot has been dispatched — the device then goes down. The action
// is a std::function so the privileged side effect (a bounded `systemctl reboot`
// in production) stays out of the handler and is replaced by a fake in tests.
// RebootCommand is parameterless by design, so no BLE-derived data reaches the
// exec path.
class RebootHandler final : public ICommandHandler {
   public:
    using RebootFn = std::function<bool()>;
    explicit RebootHandler(RebootFn reboot);

    [[nodiscard]] auto HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> override;
    auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse override;

   private:
    RebootFn reboot_;
};

}  // namespace sst::control
