#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/capture/ports/frame-src.hpp"
#include "app/decision/ports/decision.hpp"
#include "app/overlay/ports/overlay-frame-source.hpp"
#include "app/pipeline/ports/camera-frame-tap.hpp"
#include "app/pipeline/ports/frame-snapshot-source.hpp"
#include "app/processing/ports/frame-compositor.hpp"
#include "app/processing/ports/postprocessor.hpp"
#include "app/processing/ports/preprocessor.hpp"
#include "app/storage/ports/raw-capture-sink.hpp"
#include "domain/buffer/services/latest-only-slot.hpp"
#include "domain/capture/models/frame.hpp"
#include "domain/health/models/camera-health.hpp"
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
    // Frame-truth health thresholds (U3). A camera whose last sample is older
    // than health_stall_threshold reads RECOVERING; DOWN triggers only after
    // health_down_after_failed_restarts COMPLETED-AND-FAILED watchdog restart
    // attempts for that camera (a count, never elapsed time — restarts
    // serialize under restart_mtx_, so an elapsed window would false-DOWN a
    // camera queued behind another's full Argus reacquisition). Both are
    // env-overridable (SST_HEALTH_STALL_MS / SST_HEALTH_DOWN_RESTARTS) for
    // metal calibration without a rebuild; env wins over these values.
    std::chrono::milliseconds health_stall_threshold{kDefaultHealthStallThresholdMs};
    int health_down_after_failed_restarts{kDefaultHealthDownFailedRestarts};
    // Both-mode hold cap (U4). While the layout is side-by-side, this is how
    // many consecutive other-pane misses the consumer bridges by repeating the
    // previous successful composite (both panes hold — invisible at 30fps)
    // before switching to per-tick re-composition, where the live chosen pane
    // keeps advancing and the stale pane holds its last-good frame. Bounded so
    // a sustained one-camera stall (RECOVERING can last a full serialized
    // restart window) never freezes the healthy camera's pane. Env-overridable
    // (SST_BOTHMODE_HOLD_TICKS) for metal tuning; env wins over this value.
    int bothmode_hold_ticks{kDefaultBothmodeHoldTicks};

   private:
    static constexpr int kDefaultCaptureIdleSleepMs = 5;
    static constexpr int kDefaultConsumerPopTimeoutMs = 100;
    static constexpr int kDefaultCaptureRestartBackoffMs = 500;
    static constexpr int kDefaultHealthStallThresholdMs = 2000;
    static constexpr int kDefaultHealthDownFailedRestarts = 3;
    static constexpr int kDefaultBothmodeHoldTicks = 5;
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
class PipelineOrchestrator final : public sst::pipeline::IFrameSnapshotSource,
                                   public sst::pipeline::ICameraFrameTap {
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
                         sst::storage::IRawCaptureSink* raw_sink = nullptr,
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
    // threads and the consumer. A camera that fails to prime (cold
    // nvargus-daemon, dead sensor) does NOT abort the start: its producer is
    // spawned anyway, the camera begins life RECOVERING, and the watchdog +
    // health derivation own recovery — an all-or-nothing rollback here would
    // turn a transient boot-time prime timeout into a permanently dead
    // pipeline with no watchdog. Returns false only with no cameras configured.
    auto Start() -> bool;
    // Idempotent. Signals all threads to exit, joins them, stops every capture.
    auto Stop() -> void;
    [[nodiscard]] auto IsRunning() const -> bool;

    // Frame-truth health for one camera (U3), derived on read — an atomic
    // failed-restart counter plus the adapter's last-sample age, so it is cheap
    // enough for the BLE dispatcher thread (no sampling thread needed):
    //   fresh sample            → kOk (frame truth dominates; resuming frames
    //                             clear DOWN immediately)
    //   stalled, < N failures   → kRecovering
    //   stalled, ≥ N completed-and-failed restarts → kDown
    // nullopt for an out-of-range camera index. Valid whether or not the
    // orchestrator is running (a stopped pipeline reads stalled — correct for
    // gating: nothing should start against it).
    [[nodiscard]] auto CameraHealthStatus(std::size_t camera_index) const
        -> std::optional<sst::health::CameraHealth>;

    // Invoked (from that camera's producer thread, outside restart_mtx_) when a
    // camera transitions into DOWN — i.e. exactly when its Nth consecutive
    // watchdog restart completes and fails. Fires once per DOWN episode (frames
    // resuming reset the counter and re-arm it). Wire before Start(); main.cpp
    // hooks the session manager's camera-failure finalize here.
    auto SetOnCameraDown(std::function<void(std::size_t)> on_camera_down) -> void;

    // IFrameSnapshotSource: the most recent post-processed final frame, deep-
    // copied off any GstBuffer so it stays valid for the caller. std::nullopt
    // until the consumer has produced at least one frame.
    [[nodiscard]] auto GrabLatest() -> std::optional<sst::capture::Frame> override;

    // ICameraFrameTap (U6 autofocus): block up to `timeout` for the next frame
    // camera_index's producer materializes, then return a descriptor copy that
    // shares pixel ownership (never a pixel copy). The producer pays a single
    // relaxed atomic load per frame while no request is pending, so the tap is
    // free on the hot path. nullopt on timeout / out-of-range / stopped.
    [[nodiscard]] auto GrabCameraFrame(std::size_t camera_index, std::chrono::milliseconds timeout)
        -> std::optional<sst::capture::Frame> override;

   private:
    auto ProducerLoop(std::size_t camera_index) -> void;
    auto ConsumerLoop() -> void;

    // Consumer-thread-local composition hold state (U4 both-mode flicker fix).
    // Lives on ConsumerLoop's stack and is touched only by the consumer thread,
    // so the hot path takes no new locks. Held Frames share pixel ownership via
    // their `owner` shared_ptr — copies are cheap descriptor copies, never
    // pixel copies.
    struct StreamHoldState {
        // Last successful side-by-side output; repeated verbatim to bridge
        // short per-tick misses of the other pane.
        std::optional<sst::capture::Frame> last_composite;
        // Last successfully post-processed other-camera pane; past the hold cap
        // it becomes the stale pane of the per-tick re-composition.
        std::optional<sst::capture::Frame> last_other_pane;
        // Which camera last_other_pane came from — a selection change flips
        // which camera is "other", invalidating a held pane.
        std::size_t other_pane_index{0};
        // Consecutive both-mode ticks the other pane has missed.
        int stale_ticks{0};

        auto Reset() -> void {
            last_composite.reset();
            last_other_pane.reset();
            stale_ticks = 0;
        }
    };

    // Produce the frame pushed to the live/broadcast stream from the clean
    // chosen-camera frame. SIDE_BY_SIDE composites cam0 | cam1 clean (with U4
    // hold semantics — see BuildSideBySideFrame); otherwise bakes the current
    // overlay onto a copy (SINGLE broadcast view) and drops any held both-mode
    // state so intentional layout switches stay instant. Returns an owned frame
    // (its pixels survive the call).
    auto BuildStreamFrame(const sst::capture::Frame& clean_chosen,
                          const std::vector<std::optional<sst::processing::FrameBundle>>& latest,
                          std::size_t chosen_index, StreamHoldState& hold) -> sst::capture::Frame;

    // U4: the side-by-side stream frame, never a bare single-camera frame. A
    // per-tick miss of the other pane (empty slot, postprocess nullopt,
    // composite nullopt, OpenCV throw) repeats the previous composite for up to
    // bothmode_hold_ticks consecutive ticks; past the cap it re-composites
    // every tick — the live chosen pane keeps advancing while the stale pane
    // holds its last-good frame (black until that camera has ever produced
    // one), so a sustained one-camera stall never freezes the healthy pane.
    auto BuildSideBySideFrame(
        const sst::capture::Frame& clean_chosen,
        const std::vector<std::optional<sst::processing::FrameBundle>>& latest,
        std::size_t chosen_index, StreamHoldState& hold) -> sst::capture::Frame;

    // Composite the two panes POSITIONALLY: camera 0 is always the left pane,
    // camera 1 the right, regardless of which one is the selected main feed.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: chosen vs
    // other pane — the function's whole job is ordering them positionally
    auto ComposePanes(const sst::capture::Frame& chosen, const sst::capture::Frame& other,
                      std::size_t chosen_index) -> std::optional<sst::capture::Frame>;
    // NOLINTEND(bugprone-easily-swappable-parameters)

    std::vector<CameraChain> cameras_;
    std::unique_ptr<sst::processing::IPostprocessor> postprocessor_;
    std::unique_ptr<sst::decision::IDecision> decision_;
    sst::buffer::IFrameSink& record_sink_;  // CLEAN L1 (no overlay)
    sst::buffer::IFrameSink& stream_sink_;  // overlaid (live/broadcast: RTSP + RTMP)
    PipelineConfig config_;
    sst::storage::IRawCaptureSink* raw_sink_;
    sst::overlay::IOverlayFrameSource* overlay_source_;
    sst::processing::IFrameCompositor* compositor_;
    sst::streaming::PreviewLayoutState* preview_layout_;

    // One slot per camera. unique_ptr because LatestOnlySlot is non-movable and
    // we size the vector at construction from the camera count.
    std::vector<std::unique_ptr<sst::buffer::LatestOnlySlot<sst::processing::FrameBundle>>> slots_;

    // Per-camera on-request producer tap (U6 autofocus). GrabCameraFrame arms
    // `requested`; the producer — right after materializing, so the shared
    // pixels are an owned vector, never a pinned GstBuffer — checks it with one
    // relaxed load per frame and, only when armed, publishes a descriptor copy
    // under the slot mutex and notifies. unique_ptr because the sync members
    // are non-movable and the vector is sized at construction.
    struct FrameTapSlot {
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> requested{false};
        std::optional<sst::capture::Frame> sample;
    };
    std::vector<std::unique_ptr<FrameTapSlot>> tap_slots_;

    // Producer-side half of the tap: publish `frame` iff a grab is waiting.
    // One relaxed load when idle — the hot path's entire cost.
    static auto ServeFrameTap(FrameTapSlot& tap, const sst::capture::Frame& frame) -> void;

    // Per-camera count of COMPLETED-AND-FAILED watchdog restart attempts since
    // frames last flowed. Written by that camera's producer (increment on a
    // failed restart, reset on a successful Capture()); read lock-free by
    // CameraHealthStatus() on any thread. unique_ptr because std::atomic is
    // non-movable and the vector is sized at construction.
    std::vector<std::unique_ptr<std::atomic<int>>> failed_restarts_;
    std::function<void(std::size_t)> on_camera_down_;

    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> producer_threads_;
    std::thread consumer_thread_;

    // Serializes capture-pipeline restarts across producer threads. The
    // ProducerLoop watchdog re-inits Argus (gst_parse_launch of nvarguscamerasrc)
    // on the producer thread; startup does the same init serially on the main
    // thread. A shared radio-reform event kills both cameras at once, so without
    // this lock both producers would re-acquire the process-wide nvargus session
    // concurrently — the one concurrency the serialized startup deliberately
    // avoids. Held only around Restart(); the rest of the loop stays lock-free.
    std::mutex restart_mtx_;

    // Latest final frame for on-demand snapshots (thumbnail). Guarded by its own
    // mutex so GrabLatest() (BLE thread) never contends with Start/Stop. The
    // stored Frame owns its pixels (postprocessor MakeOwnedFrame), so a value
    // copy under the lock hands the caller a fully independent frame.
    mutable std::mutex latest_frame_mtx_;
    std::optional<sst::capture::Frame> latest_frame_;
};

}  // namespace sst::pipeline
