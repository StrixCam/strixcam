#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "app/storage/ports/jpeg-encoder.hpp"
#include "domain/capture/models/frame.hpp"

namespace sst::adapters::storage {

// In-memory JPEG encoder backed by OpenCV cv::imencode. Wraps the frame's first
// plane in a cv::Mat (converting color order to BGR as needed), optionally
// resizes to the requested output size, and encodes to JPEG bytes. Runs in the
// dev container (CPU OpenCV) — a real module-boundary encoder, not
// hardware-bound. Sibling to OpenCvThumbnailWriter, which writes to disk.
class OpenCvJpegEncoder final : public sst::storage::IJpegEncoder {
   public:
    [[nodiscard]] auto Encode(const sst::capture::Frame& frame, sst::common::OutputSize size,
                              std::uint32_t quality)
        -> std::optional<std::vector<std::uint8_t>> override;
};

}  // namespace sst::adapters::storage
