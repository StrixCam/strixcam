#include "app/control/services/handlers/raw-capture.handler.hpp"

#include <utility>

namespace sst::control {

RawCaptureHandler::RawCaptureHandler(sst::storage::IRawCaptureSink& sink,
                                     StartHealthGate health_gate)
    : sink_(sink), health_gate_(std::move(health_gate)) {}

auto RawCaptureHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kRawCapture};
}

auto RawCaptureHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;
    const auto& raw = cmd.raw_capture();

    // An unset action decodes as the proto3 zero value RECORDING_ACTION_UNKNOWN.
    // The contract forbids treating absent/UNKNOWN as START, so reject it.
    if (!raw.has_action()) {
        resp.set_status(sst_cam::ResponseStatus::ERROR);
        resp.set_error_message("raw capture: action not set");
        return resp;
    }

    switch (raw.action()) {
        case sst_cam::RECORDING_START: {
            // Health gate first (U3): raw dual capture needs BOTH cameras
            // delivering. STOP below is never gated.
            if (const auto reason = health_gate_.RejectReason()) {
                resp.set_status(sst_cam::ResponseStatus::DEVICE_INOPERABLE);
                resp.set_error_message("raw capture start rejected: " + *reason);
                return resp;
            }
            // capture_group_id is app-minted and mandatory on START — firmware
            // stamps both files with it and never mints its own.
            if (!raw.has_capture_group_id() || raw.capture_group_id().empty()) {
                resp.set_status(sst_cam::ResponseStatus::ERROR);
                resp.set_error_message("raw capture start: missing capture_group_id");
                return resp;
            }
            // Standalone (retired) trigger: no per-match dir, fall back to the
            // sink's construction-time root.
            if (!sink_.Start(raw.capture_group_id(), {})) {
                resp.set_status(sst_cam::ResponseStatus::ERROR);
                resp.set_error_message("raw capture failed to start (already capturing or I/O)");
                return resp;
            }
            resp.set_status(sst_cam::ResponseStatus::OK);
            return resp;
        }
        case sst_cam::RECORDING_STOP: {
            if (!sink_.Stop()) {
                resp.set_status(sst_cam::ResponseStatus::ERROR);
                resp.set_error_message("raw capture stop: no active raw capture");
                return resp;
            }
            resp.set_status(sst_cam::ResponseStatus::OK);
            return resp;
        }
        case sst_cam::RECORDING_PAUSE:
        case sst_cam::RECORDING_RESUME:
            resp.set_status(sst_cam::ResponseStatus::UNSUPPORTED);
            resp.set_error_message("raw capture does not support pause/resume");
            return resp;
        default:
            // RECORDING_ACTION_UNKNOWN or any unrecognized value — never START.
            resp.set_status(sst_cam::ResponseStatus::ERROR);
            resp.set_error_message("raw capture: unsupported action");
            return resp;
    }
}

}  // namespace sst::control
