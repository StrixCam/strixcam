#pragma once

#include <chrono>
#include <optional>

#include "domain/capture/models/frame.hpp"
#include "domain/config/models/config-data.hpp"

namespace sst::capture {

// using sst::capture::Frame;
using sst::config::ConfigData;

class ICaptureFrame {
   public:
    virtual ~ICaptureFrame() = default;

    virtual auto Start() -> void = 0;
    virtual auto Stop() -> void = 0;
    virtual auto Restart() -> void {
        Stop();
        Start();
    };
    [[nodiscard]] virtual auto IsRunning() const -> bool = 0;
    virtual auto Capture() -> std::optional<Frame> = 0;

    // Frame truth for the per-camera health derivation (state-health cycle U3):
    // when the source last delivered a sample, on the steady clock. nullopt =
    // no sample since Start() (or the adapter cannot report frame truth, in
    // which case the camera reads as stalled rather than fabricating health).
    // Thread-safe: read by the health path while Capture() runs on the producer.
    [[nodiscard]] virtual auto LastSampleAt() const
        -> std::optional<std::chrono::steady_clock::time_point> {
        return std::nullopt;
    }
};

}  // namespace sst::capture