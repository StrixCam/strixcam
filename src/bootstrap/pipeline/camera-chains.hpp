#pragma once

#include <string>
#include <vector>

#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "domain/capture/models/camera-config.hpp"

namespace sst::bootstrap {

// The two production camera chains (sensor-id 0 and 1): GStreamer capture +
// OpenCV preprocessor each. Both run; the decision seam presents one camera,
// the other ages out unchosen but stays live so raw dual capture can tap it.
auto BuildCameraChains(const sst::capture::CameraConfig& camera_config,
                       const std::string& device_model) -> std::vector<sst::pipeline::CameraChain>;

}  // namespace sst::bootstrap
