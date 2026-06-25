#pragma once

#include <cstdint>
#include <string>

#include "domain/capture/models/frame.hpp"

namespace sst::storage {

// Records raw frames from BOTH cameras to per-camera files, independently of the
// final (cam-0) recording. Driven by RawCaptureControlCommand: Start opens the
// per-camera files for one app-minted capture_group_id, the pipeline producers
// push each camera's materialized frames in, Stop flushes and closes them.
//
// Implementations MUST NOT do synchronous file I/O on the calling (capture)
// thread — PushCamera hands off to a per-camera writer thread via a bounded
// queue that drops oldest under backpressure, so a slow disk can never stall
// capture. Raw NV12 at 1080p30 is ~93 MB/s per camera (~186 MB/s for two), so
// the bounded-queue + drop-oldest discipline is load-bearing, not optional.
class IRawCaptureSink {
   public:
    virtual ~IRawCaptureSink() = default;

    // Open per-camera files for a new raw session stamped with the app-minted
    // capture_group_id. Returns false if already capturing or a file can't be
    // opened. Independent of the final recording — both can run concurrently.
    virtual auto Start(const std::string& capture_group_id) -> bool = 0;

    // Enqueue a materialized frame from `camera_index`. Non-blocking: no-ops when
    // not capturing or the index is out of range; drops the oldest queued frame
    // rather than blocking the caller when the writer can't keep up.
    virtual auto PushCamera(std::uint32_t camera_index, const sst::capture::Frame& frame)
        -> void = 0;

    // Flush every per-camera queue, close the files, join the writer threads.
    // Returns false when no session was active. Idempotent enough for cleanup.
    virtual auto Stop() -> bool = 0;

    [[nodiscard]] virtual auto IsCapturing() const -> bool = 0;
};

}  // namespace sst::storage
