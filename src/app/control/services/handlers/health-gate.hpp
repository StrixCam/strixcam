#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "bluetooth.pb.h"
#include "domain/health/models/camera-health.hpp"

namespace sst::control {

// Domain → wire mapping for the frame-truth per-camera health (U3). Shared by
// the telemetry / snapshot providers in main.cpp so the two surfaces can never
// map the same domain state to different wire values.
inline auto ToWireHealth(sst::health::CameraHealth health) -> sst_cam::CameraHealth {
    switch (health) {
        case sst::health::CameraHealth::kOk:
            return sst_cam::CAMERA_HEALTH_OK;
        case sst::health::CameraHealth::kRecovering:
            return sst_cam::CAMERA_HEALTH_RECOVERING;
        case sst::health::CameraHealth::kDown:
            return sst_cam::CAMERA_HEALTH_DOWN;
    }
    return sst_cam::CAMERA_HEALTH_UNKNOWN;
}

// Firmware-side health gating for START-class commands (U3, origin R8): start
// recording / streaming / raw capture are refused with DEVICE_INOPERABLE while
// any camera is not OK — app-side gating alone leaves a poll-latency hole.
// Stop/finalize, downloads, wifi-direct, reboot, diagnostics reads, snapshot
// and telemetry are NEVER gated: the operator must always be able to end a
// session and retrieve footage. A default-constructed gate (or a nullopt
// health reading — "unreported") gates nothing, so handlers built without a
// health source keep their pre-U3 behavior.
class StartHealthGate {
   public:
    // Same provider shape as the telemetry/snapshot seams: nullopt = health
    // unreported (never a fabricated OK — and never a fabricated failure, so
    // unreported does not gate).
    using Provider = std::function<std::optional<sst_cam::CameraHealth>()>;

    StartHealthGate() = default;
    StartHealthGate(Provider camera0, Provider camera1)
        : camera0_(std::move(camera0)), camera1_(std::move(camera1)) {}

    // nullopt when start-class commands may proceed; otherwise the
    // human-readable rejection the handler returns with DEVICE_INOPERABLE.
    [[nodiscard]] auto RejectReason() const -> std::optional<std::string> {
        if (auto reason = CameraReason(camera0_, 0)) {
            return reason;
        }
        return CameraReason(camera1_, 1);
    }

   private:
    static auto CameraReason(const Provider& provider,
                             std::size_t camera_index) -> std::optional<std::string> {
        if (!provider) {
            return std::nullopt;
        }
        const auto health = provider();
        // Only a positive unhealthy reading gates; nullopt/UNKNOWN mean
        // "unreported" and must neither fabricate OK nor fabricate failure.
        if (!health || (*health != sst_cam::CAMERA_HEALTH_RECOVERING &&
                        *health != sst_cam::CAMERA_HEALTH_DOWN)) {
            return std::nullopt;
        }
        const char* state = *health == sst_cam::CAMERA_HEALTH_RECOVERING ? "recovering" : "down";
        return "camera " + std::to_string(camera_index) + " is " + state +
               " — cannot start until it is healthy";
    }

    Provider camera0_;
    Provider camera1_;
};

}  // namespace sst::control
