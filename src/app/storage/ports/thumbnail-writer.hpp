#pragma once

#include <filesystem>

#include "domain/capture/models/frame.hpp"

namespace sst::storage {

// Encodes a representative frame to a JPEG thumbnail on disk. Used at recording
// finalization (the on-demand BLE ThumbnailRequest path is separate).
class IThumbnailWriter {
   public:
    virtual ~IThumbnailWriter() = default;

    virtual auto Write(const sst::capture::Frame& frame,
                       const std::filesystem::path& output_path) -> bool = 0;
};

}  // namespace sst::storage
