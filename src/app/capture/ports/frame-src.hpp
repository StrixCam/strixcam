#pragma once

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
};

}  // namespace sst::capture