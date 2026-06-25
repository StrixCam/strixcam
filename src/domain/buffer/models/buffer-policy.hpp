#pragma once

namespace sst::buffer {

enum class BufferPolicy {
    LatestOnly,
    DropOldest,
};

}  // namespace sst::buffer
