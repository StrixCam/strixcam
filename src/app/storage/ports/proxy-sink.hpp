#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "domain/capture/models/frame.hpp"

namespace sst::storage {

// Internal dual-camera proxy writer: records BOTH cameras to small per-camera
// files, independently of the final (chosen-camera) recording. The proxy is a
// firmware-automatic development artifact — angle/framing/stability/
// color-grading checks and model training — invisible to the app: ProxyLifecycle
// (record-or-stream ref-count) starts it whenever a match records or platform-
// streams and stops it on last-out; no wire command exists. Proxy footage is
// retrieved on-device (ssh, by match id), never over the app contract.
//
// Implementations MUST NOT do synchronous file I/O on the calling (capture)
// thread — PushCamera hands off to a per-camera writer behind a bounded queue
// that drops oldest under backpressure, so a slow disk or encode can never
// stall capture. Raw NV12 at 1080p30 is ~93 MB/s per camera (~186 MB/s for
// two), so the bounded-queue + drop-oldest discipline is load-bearing, not
// optional.
class IProxySink {
   public:
    virtual ~IProxySink() = default;

    // Open per-camera files for a new proxy session named by the session's
    // match_uuid, written INTO `output_dir` — the per-match directory
    // (SessionConfig.video_output_path), so the proxy pair sits beside the final
    // <match>.mp4 + <match>.timeline.json. An empty output_dir falls back to the
    // sink's construction-time root. Returns false if already capturing or a
    // file can't be opened. Independent of the final recording — both run
    // concurrently.
    virtual auto Start(const std::string& match_uuid,
                       const std::filesystem::path& output_dir) -> bool = 0;

    // Enqueue a materialized frame from `camera_index`. Non-blocking: no-ops when
    // not capturing or the index is out of range; drops the oldest queued frame
    // rather than blocking the caller when the writer can't keep up.
    virtual auto PushCamera(std::uint32_t camera_index,
                            const sst::capture::Frame& frame) -> void = 0;

    // Flush every per-camera queue, close the files, join the writer threads.
    // Returns false when no session was active. Idempotent enough for cleanup.
    virtual auto Stop() -> bool = 0;

    [[nodiscard]] virtual auto IsCapturing() const -> bool = 0;
};

}  // namespace sst::storage
