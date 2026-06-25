#pragma once

#include <filesystem>

#include "app/storage/ports/thumbnail-writer.hpp"
#include "domain/capture/models/frame.hpp"

namespace sst::adapters::storage {

// JPEG thumbnail writer backed by OpenCV. Wraps the frame's first plane in a
// cv::Mat (converting color order as needed) and imwrite()s a JPEG. Runs in the
// dev container (no GPU) — a real module-boundary writer, not hardware-bound.
class OpenCvThumbnailWriter final : public sst::storage::IThumbnailWriter {
   public:
    auto Write(const sst::capture::Frame& frame,
               const std::filesystem::path& output_path) -> bool override;
};

}  // namespace sst::adapters::storage
