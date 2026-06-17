#pragma once

#include <vector>

#include "bluetooth.pb.h"

namespace sst::control {

// One per controllable concern (device, session, recording, streaming, match,
// wifi-direct, download, ...). Replaces the old string-route IController.
//
// A handler declares which Command oneof payload cases it owns; the
// CommandDispatcher routes a Command to the handler registered for its
// payload_case(). Drop-in extension point: a new handler is registered with a
// single Register() line in main.cpp (U14).
//
// The handler returns a fully-formed CommandResponse (payload + status); the
// dispatcher overwrites correlation_id to guarantee the echo (R5) and converts
// a thrown exception into status=ERROR (R6). A handler that cannot process a
// recognized command sets status=ERROR + error_message itself.
class ICommandHandler {
   public:
    virtual ~ICommandHandler() = default;

    [[nodiscard]] virtual auto HandledCases() const
        -> std::vector<sst_cam::Command::PayloadCase> = 0;

    virtual auto Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse = 0;
};

}  // namespace sst::control
