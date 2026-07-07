#include "bootstrap/pipeline/camera-chains.hpp"

#include <memory>

#include "adapters/capture/frame/gstreamer/gstreamer.hpp"
#include "adapters/processing/opencv/opencv-preprocessor.hpp"
#include "bootstrap/runtime/runtime-defaults.hpp"

namespace sst::bootstrap {

auto BuildCameraChains(const sst::capture::CameraConfig& camera_config,
                       const std::string& device_model) -> std::vector<sst::pipeline::CameraChain> {
    std::vector<sst::pipeline::CameraChain> chains;
    chains.push_back(sst::pipeline::CameraChain{
        .capture = std::make_unique<sst::capture::GStreamerAdapter>(
            camera_config, device_model, sst::runtime_defaults::kCamera0Index),
        .preprocessor = std::make_unique<sst::adapters::processing::OpenCvPreprocessor>()});
    chains.push_back(sst::pipeline::CameraChain{
        .capture = std::make_unique<sst::capture::GStreamerAdapter>(
            camera_config, device_model, sst::runtime_defaults::kCamera1Index),
        .preprocessor = std::make_unique<sst::adapters::processing::OpenCvPreprocessor>()});
    return chains;
}

}  // namespace sst::bootstrap
