#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/output-size.hpp"

namespace sst::storage {

// Encodes a frame to JPEG bytes in memory (no disk). Used by the on-demand BLE
// ThumbnailRequest path, where ThumbnailResponse.jpeg_bytes needs the encoded
// bytes directly. `size` requests an output size (`OutputSize{0, 0}` = keep the
// frame's own size); `quality` is the JPEG quality 1..100 (0 = encoder default).
// Returns std::nullopt when the frame has no usable pixel data or encoding
// fails.
class IJpegEncoder {
   public:
    virtual ~IJpegEncoder() = default;

    [[nodiscard]] virtual auto Encode(const sst::capture::Frame& frame, common::OutputSize size,
                                      std::uint32_t quality)
        -> std::optional<std::vector<std::uint8_t>> = 0;
};

}  // namespace sst::storage
