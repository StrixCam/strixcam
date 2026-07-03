#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/capture/ports/frame-src.hpp"
#include "app/decision/ports/decision.hpp"
#include "app/overlay/ports/overlay-frame-source.hpp"
#include "app/pipeline/ports/frame-snapshot-source.hpp"
#include "app/processing/ports/frame-compositor.hpp"
#include "app/processing/ports/postprocessor.hpp"
#include "app/processing/ports/preprocessor.hpp"
#include "app/raw_capture/ports/raw-capture-sink.hpp"
#include "domain/buffer/services/latest-only-slot.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/processing/models/frame-bundle.hpp"
#include "domain/streaming/models/preview-layout.hpp"

namespace sst::pipeline {

struct PipelineConfig {
    // Polling interval used by a producer when Capture() returns nullopt
    // (no new frame ready). Tight enough to not miss frames at 30+ fps,
    // loose enough not to spin a core when the camera is idle.
    std::chrono::milliseconds capture_idle_sleep{kDefaultCaptureIdleSleepMs};
    // Bound on how long the consumer waits for a new bundle from the cadence
    // camera before re-evaluating. Keeps shutdown latency low.
    std::chrono::milliseconds consumer_pop_timeout{kDefaultConsumerPopTimeoutMs};
    // Backoff a producer waits after a FAILED capture restart before retrying.
    // A capture pipeline can die mid-run — Argus posts INVALID_SETTINGS when the
    // WiFi-Direct radio reforms its group, and HandleBusMessages()->Stop() then
    // leaves it torn down. The producer watchdog re-Starts the dead pipeline;
    // this backoff keeps a persistently-failing sensor (or a still-settling
    // radio) from hot-looping gst_parse_launch every few milliseconds.
    std::chrono::milliseconds capture_restart_backoff{kDefaultCaptureRestartBackoffMs};

   private:
    static constexpr int kDefaultCaptureIdleSleepMs = 5;
    static constexpr int kDefaultConsumerPopTimeoutMs = 100;
    static constexpr int kDefaultCaptureRestartBackoffMs = 500;
};

// One capture → preprocess chain for a single camera. The orchestrator owns the
// chain, runs a producer thread per chain, and materializes each frame before
// it enters that camera's slot.
struct CameraChain {
    std::unique_ptr<sst::capture::ICaptureFrame> capture;
    std::unique_ptr<sst::processing::IPreprocessor> preprocessor;
};

// Multi-camera pipeline orchestrator. Owns one producer thread per camera plus
// a single consumer thread:
//
//   producer[i]: ICaptureFrame::Capture() loop → IPreprocessor::Process →
//                buffer::MaterializeFrame(source) → push to slots_[i].
//
//   consumer:    block on the cadence camera's slot (camera 0), sample every
//                other camera non-blocking, call IDecision::Decide(per-camera
//                latest) → postprocess the chosen camera's bundle with the
//                returned crop → IFrameSink::Push.
//
// The IDecision port is the intelligence seam: StaticDecision selects camera 0
// full-frame today; the AI/physics/decision stack implements the same port
// later. Both cameras run regardless of which is chosen — unchosen bundles age
// out of their LatestOnlySlot, and the second camera being live is what makes
// raw dual capture (U6) possible.
//
// Cadence note: the consumer waits on camera 0's slot, since the static policy
// always presents camera 0. If camera 0 stalls, the consumer wakes every
// consumer_pop_timeout, the decision returns nullopt, and it retries — no
// deadlock, and other cameras' frames simply age out.
class PipelineOrchestrator final : public sst::pipeline::IFrameSnapshotSource {
   public:
    // Both raw_sink and overlay_source are optional (may be null). When raw_sink
    // is set, every camera's materialized bundle is forked to it before the move
    // into the slot, so raw dual capture taps the same frames the decision path
    // consumes. The CLEAN post-processed frame (no overlay) goes to record_sink;
    // the overlay (when overlay_source has one) is composited only onto the copy
    // sent to stream_sink. So a recording plays with or without overlays (overlay
    // is burned on demand, #6) while the live/broadcast stream carries the baked
    // overlay. Retires the prior "recording + RTSP + RTMP carry identical pixels"
    // invariant.
    PipelineOrchestrator(std::vector<CameraChain> cameras,
                         std::unique_ptr<sst::processing::IPostprocessor> postprocessor,
                         std::unique_ptr<sst::decision::IDecision> decision,
                         // NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: clean L1
                         // vs overlaid stream are distinct sinks
                         sst::buffer::IFrameSink& record_sink, sst::buffer::IFrameSink& stream_sink,
                         // NOLINTEND(bugprone-easily-swappable-parameters)
                         PipelineConfig config = PipelineConfig{},
                         sst::raw_capture::IRawCaptureSink* raw_sink = nullptr,
                         sst::overlay::IOverlayFrameSource* overlay_source = nullptr,
                         // #6 F6d dual preview: both optional. When set and the
                         // layout is SIDE_BY_SIDE, the consumer postprocesses the
                         // second camera and composites cam0 | cam1 (clean) into
                         // the stream instead of the single overlaid frame.
                         sst::processing::IFrameCompositor* compositor = nullptr,
                         sst::streaming::PreviewLayoutState* preview_layout = nullptr);

    ~PipelineOrchestrator() override;

    PipelineOrchestrator(const PipelineOrchestrator&) = delete;
    auto operator=(const PipelineOrchestrator&) -> PipelineOrchestrator& = delete;
    PipelineOrchestrator(PipelineOrchestrator&&) = delete;
    auto operator=(PipelineOrchestrator&&) -> PipelineOrchestrator& = delete;

    // Idempotent. Starts every camera's capture, then spawns the producer
    // threads and the consumer. Returns false (and stops anything started) if
    // any camera fails to start.
    auto Start() -> bool;
    // Idempotent. Signals all threads to exit, joins them, stops every capture.
    auto Stop() -> void;
    [[nodiscard]] auto IsRunning() const -> bool;

    // IFrameSnapshotSource: the most recent post-processed final frame, deep-
    // copied off any GstBuffer so it stays valid for the caller. std::nullopt
    // until the consumer has produced at least one frame.
    [[nodiscard]] auto GrabLatest() -> std::optional<sst::capture::Frame> override;

   private:
    auto ProducerLoop(std::size_t camera_index) -> void;
    auto ConsumerLoop() -> void;

    // Produce the frame pushed to the live/broadcast stream from the clean
    // chosen-camera frame. SIDE_BY_SIDE composites cam0 | cam1 clean; otherwise
    // bakes the current overlay onto a copy (SINGLE broadcast view). Returns an
    // owned frame (its pixels survive the call).
    auto BuildStreamFrame(const sst::capture::Frame& clean_chosen,
                          const std::vector<std::optional<sst::processing::FrameBundle>>& latest,
                          std::size_t chosen_index) -> sst::capture::Frame;

    std::vector<CameraChain> cameras_;
    std::unique_ptr<sst::processing::IPostprocessor> postprocessor_;
    std::unique_ptr<sst::decision::IDecision> decision_;
    sst::buffer::IFrameSink& record_sink_;  // CLEAN L1 (no overlay)
    sst::buffer::IFrameSink& stream_sink_;  // overlaid (live/broadcast: RTSP + RTMP)
    PipelineConfig config_;
    sst::raw_capture::IRawCaptureSink* raw_sink_;
    sst::overlay::IOverlayFrameSource* overlay_source_;
    sst::processing::IFrameCompositor* compositor_;
    sst::streaming::PreviewLayoutState* preview_layout_;

    // One slot per camera. unique_ptr because LatestOnlySlot is non-movable and
    // we size the vector at construction from the camera count.
    std::vector<std::unique_ptr<sst::buffer::LatestOnlySlot<sst::processing::FrameBundle>>> slots_;

    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> producer_threads_;
    std::thread consumer_thread_;

    // Latest final frame for on-demand snapshots (thumbnail). Guarded by its own
    // mutex so GrabLatest() (BLE thread) never contends with Start/Stop. The
    // stored Frame owns its pixels (postprocessor MakeOwnedFrame), so a value
    // copy under the lock hands the caller a fully independent frame.
    mutable std::mutex latest_frame_mtx_;
    std::optional<sst::capture::Frame> latest_frame_;
};

}  // namespace sst::pipeline
