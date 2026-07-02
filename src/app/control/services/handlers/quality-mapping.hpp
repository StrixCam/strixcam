#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>

#include "bluetooth.pb.h"
#include "domain/common/models/formatter/_fmt.hpp"  // IWYU pragma: keep
#include "domain/common/models/video-quality.hpp"

namespace sst::control {

// Maps an optional wire VideoQuality onto the domain value, validated against
// the advertised supported modes. `has_quality == false` → unset (the firmware
// then uses its per-branch default). A set-but-unsupported mode is rejected →
// unset + a warning, so a misbehaving app can never drive the encode pipeline to
// an unadvertised resolution/fps (the app only ever offers
// DeviceInfoResponse.supported_modes). Shared by the record and stream handlers
// so both validate against the single kSupportedVideoModes source of truth.
inline auto ResolveQuality(bool has_quality,
                           const sst_cam::VideoQuality& wire) -> sst::common::VideoQuality {
    if (!has_quality) {
        return {};
    }
    const sst::common::VideoQuality quality{static_cast<std::int32_t>(wire.width()),
                                            static_cast<std::int32_t>(wire.height()),
                                            static_cast<std::int32_t>(wire.fps())};
    if (!sst::common::IsSupportedMode(quality)) {
        spdlog::warn("Rejecting unsupported quality {} — using firmware default", quality);
        return {};
    }
    return quality;
}

}  // namespace sst::control
