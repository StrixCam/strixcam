#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <utility>

#include "domain/buffer/services/materialize-frame.hpp"
#include "domain/decision/models/camera-choice.hpp"
#include "domain/overlay/services/frame-compositor.hpp"

namespace sst::pipeline {

namespace {

// Positive-integer env override; the config value holds when the variable is
// unset or unparsable. Lets the health thresholds be recalibrated on metal
// (systemd drop-in) without a rebuild, same discipline as SST_PIPELINE_FPS.
auto EnvPositiveInt(const char* name, int fallback) -> int {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    const int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

}  // namespace

PipelineOrchestrator::PipelineOrchestrator(
    std::vector<CameraChain> cameras,
    std::unique_ptr<sst::processing::IPostprocessor> postprocessor,
    std::unique_ptr<sst::decision::IDecision> decision,
    // NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: clean L1 vs overlaid stream
    // are distinct sinks
    sst::buffer::IFrameSink& record_sink, sst::buffer::IFrameSink& stream_sink,
    // NOLINTEND(bugprone-easily-swappable-parameters)
    PipelineConfig config, sst::storage::IProxySink* proxy_sink,
    sst::overlay::IOverlayFrameSource* overlay_source,
    sst::processing::IFrameCompositor* compositor,
    sst::streaming::PreviewLayoutState* preview_layout)
    : cameras_(std::move(cameras)),
      postprocessor_(std::move(postprocessor)),
      decision_(std::move(decision)),
      record_sink_(record_sink),
      stream_sink_(stream_sink),
      config_(config),
      proxy_sink_(proxy_sink),
      overlay_source_(overlay_source),
      compositor_(compositor),
      preview_layout_(preview_layout) {
    // Env-tunable health thresholds (metal calibration; env wins over config).
    config_.health_stall_threshold = std::chrono::milliseconds{EnvPositiveInt(
        "SST_HEALTH_STALL_MS", static_cast<int>(config_.health_stall_threshold.count()))};
    config_.health_down_after_failed_restarts =
        EnvPositiveInt("SST_HEALTH_DOWN_RESTARTS", config_.health_down_after_failed_restarts);
    // U4 both-mode hold cap — tuned on metal without a rebuild.
    config_.bothmode_hold_ticks =
        EnvPositiveInt("SST_BOTHMODE_HOLD_TICKS", config_.bothmode_hold_ticks);

    slots_.reserve(cameras_.size());
    failed_restarts_.reserve(cameras_.size());
    tap_slots_.reserve(cameras_.size());
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        slots_.push_back(
            std::make_unique<sst::buffer::LatestOnlySlot<sst::processing::FrameBundle>>());
        failed_restarts_.push_back(std::make_unique<std::atomic<int>>(0));
        tap_slots_.push_back(std::make_unique<FrameTapSlot>());
    }
}

PipelineOrchestrator::~PipelineOrchestrator() {
    if (running_) {
        Stop();
    }
}

auto PipelineOrchestrator::Start() -> bool {
    std::lock_guard lock(mtx_);
    if (running_) {
        return true;
    }
    if (cameras_.empty()) {
        spdlog::error("PipelineOrchestrator::Start: no cameras configured");
        return false;
    }

    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        cameras_[i].capture->Start();
        if (!cameras_[i].capture->IsRunning()) {
            // Tolerated, NOT rolled back: Start() now reports primed truth, so
            // a cold nvargus-daemon prime timeout lands here at boot. The
            // producer watchdog below owns recovery; this camera begins life
            // RECOVERING (no frames yet) and heals when frames flow.
            spdlog::warn(
                "PipelineOrchestrator::Start: camera {} failed to prime — starting it RECOVERING, "
                "watchdog owns recovery",
                i);
        }
    }

    running_ = true;
    producer_threads_.reserve(cameras_.size());
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        producer_threads_.emplace_back(&PipelineOrchestrator::ProducerLoop, this, i);
    }
    consumer_thread_ = std::thread(&PipelineOrchestrator::ConsumerLoop, this);
    spdlog::info("PipelineOrchestrator: started with {} camera(s)", cameras_.size());
    return true;
}

auto PipelineOrchestrator::Stop() -> void {
    std::vector<std::thread> producers_to_join;
    std::thread consumer_to_join;
    {
        std::lock_guard lock(mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
        for (auto& slot : slots_) {
            slot->Close();
        }
        // Wake any autofocus GrabCameraFrame() waiter promptly — the producers
        // are about to exit, so no sample would ever arrive.
        for (auto& tap : tap_slots_) {
            tap->cv.notify_all();
        }
        producers_to_join = std::move(producer_threads_);
        producer_threads_.clear();
        consumer_to_join = std::move(consumer_thread_);
    }
    for (auto& producer : producers_to_join) {
        if (producer.joinable()) {
            producer.join();
        }
    }
    if (consumer_to_join.joinable()) {
        consumer_to_join.join();
    }
    for (auto& camera : cameras_) {
        camera.capture->Stop();
    }
    spdlog::info("PipelineOrchestrator: stopped");
}

auto PipelineOrchestrator::IsRunning() const -> bool { return running_; }

auto PipelineOrchestrator::SetOnCameraDown(std::function<void(std::size_t)> on_camera_down)
    -> void {
    on_camera_down_ = std::move(on_camera_down);
}

auto PipelineOrchestrator::CameraHealthStatus(std::size_t camera_index) const
    -> std::optional<sst::health::CameraHealth> {
    if (camera_index >= cameras_.size()) {
        return std::nullopt;
    }
    // Frame truth dominates: a fresh sample is OK regardless of the restart
    // history (frames resuming clear DOWN immediately; the producer resets the
    // counter on its next Capture()).
    const auto last_sample = cameras_[camera_index].capture->LastSampleAt();
    if (last_sample.has_value() &&
        std::chrono::steady_clock::now() - *last_sample <= config_.health_stall_threshold) {
        return sst::health::CameraHealth::kOk;
    }
    // Stalled. DOWN only after N completed-and-failed restart attempts for
    // THIS camera; anything less is the watchdog's RECOVERING window.
    if (*failed_restarts_[camera_index] >= config_.health_down_after_failed_restarts) {
        return sst::health::CameraHealth::kDown;
    }
    return sst::health::CameraHealth::kRecovering;
}

auto PipelineOrchestrator::ProducerLoop(std::size_t camera_index) -> void {
    auto& capture = *cameras_[camera_index].capture;
    auto& preprocessor = *cameras_[camera_index].preprocessor;
    auto& slot = *slots_[camera_index];
    auto& failed_restarts = *failed_restarts_[camera_index];

    while (running_) {
        // Watchdog: a capture pipeline can die mid-run and stay dead. Argus posts
        // INVALID_SETTINGS when the WiFi-Direct radio reforms its P2P group on
        // phone connect; HandleBusMessages()->Stop() tears the pipeline down, and
        // nothing else ever restarts it — the producer would otherwise spin
        // Capture() (nullopt) on a NULL pipeline forever, so preview goes blank
        // and recording produces 0-byte files. Detect the stopped pipeline and
        // re-init it, backing off between failed attempts (the radio may still be
        // settling) instead of hot-looping gst_parse_launch.
        if (!capture.IsRunning()) {
            {
                // Serialize re-init: a radio reform kills both cameras at once, and
                // two concurrent nvarguscamerasrc re-acquisitions race the shared
                // nvargus session. Restart() is Stop()+Start() (idempotent).
                std::lock_guard restart_lock(restart_mtx_);
                capture.Restart();
            }
            if (!running_) {
                break;  // shutdown raced the restart — exit now, don't Capture/backoff
            }
            if (!capture.IsRunning()) {
                // A COMPLETED-AND-FAILED restart attempt — the health signal
                // that (after N of them) declares this camera DOWN. A count,
                // never elapsed time: dual-camera radio events queue camera B
                // behind camera A's full Argus reacquisition on restart_mtx_,
                // so a time window would false-DOWN the queued camera.
                const int failures = failed_restarts.fetch_add(1) + 1;
                if (failures == config_.health_down_after_failed_restarts && on_camera_down_) {
                    spdlog::error(
                        "PipelineOrchestrator: camera {} DOWN after {} failed restart attempts",
                        camera_index, failures);
                    on_camera_down_(camera_index);
                }
                spdlog::warn(
                    "PipelineOrchestrator: camera {} capture down; restart failed ({} attempts), "
                    "retrying in {}ms",
                    camera_index, failures, config_.capture_restart_backoff.count());
                // Interruptible backoff: sleep in short slices so Stop() (running_
                // -> false) aborts the wait promptly instead of holding the join
                // for the full period.
                constexpr auto kBackoffSlice = std::chrono::milliseconds(50);
                for (std::chrono::milliseconds waited{0};
                     waited < config_.capture_restart_backoff && running_;
                     waited += kBackoffSlice) {
                    std::this_thread::sleep_for(kBackoffSlice);
                }
                continue;
            }
            spdlog::info("PipelineOrchestrator: camera {} capture restarted after failure",
                         camera_index);
        }

        auto raw = capture.Capture();
        if (!raw) {
            std::this_thread::sleep_for(config_.capture_idle_sleep);
            continue;
        }
        // Frames flow again: re-arm the DOWN counter (and the one-shot DOWN
        // notification) — recovery is proven by frames, not by restart returns.
        failed_restarts = 0;
        auto bundle = preprocessor.Process(*raw);
        if (!bundle) {
            // Preprocess failure is logged inside Process(); drop and continue.
            continue;
        }
        // Materialize the source plane bytes off the GstBuffer so the appsink
        // slot is freed before the bundle crosses to the consumer thread.
        bundle->source_frame = sst::buffer::MaterializeFrame(bundle->source_frame);
        // Autofocus frame tap (U6): only when a GrabCameraFrame() is actually
        // waiting does the producer publish a descriptor copy (shares the just-
        // materialized owned pixels — no pixel copy, no GstBuffer pinned).
        ServeFrameTap(*tap_slots_[camera_index], bundle->source_frame);
        // Fork the materialized frame to the internal proxy BEFORE the std::move
        // below (tapping after the move would read a moved-from bundle). The sink
        // copies what it needs and no-ops cheaply when not capturing.
        if (proxy_sink_ != nullptr) {
            proxy_sink_->PushCamera(static_cast<std::uint32_t>(camera_index), bundle->source_frame);
        }
        slot.Push(std::move(*bundle));
    }
}

auto PipelineOrchestrator::ConsumerLoop() -> void {
    // U4: composition hold state for the side-by-side view. Stack-local to this
    // thread — only BuildStreamFrame (called below, same thread) touches it, so
    // the hot path stays lock-free.
    StreamHoldState hold;
    while (running_) {
        // The cadence camera — the one we BLOCK on to pace the loop at capture
        // rate — follows the decision's preference. ManualDecision returns the
        // user-selected camera, so the blocking pop moves with the selection and
        // the chosen camera is never phase-empty (a non-cadence selected camera is
        // only TryPop'd, so its slot is intermittently empty on a phase miss —
        // that miss is what made a non-cadence selection flicker). Every other
        // camera is sampled non-blocking; each tick consumes the latest bundle
        // from each slot — unchosen ones are dropped (aged out) here.
        std::size_t cadence = decision_->PreferredCadenceCamera();
        if (cadence >= cameras_.size()) {
            cadence = 0;
        }
        std::vector<std::optional<sst::processing::FrameBundle>> latest(cameras_.size());
        latest[cadence] = slots_[cadence]->Pop(config_.consumer_pop_timeout);
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (i != cadence) {
                latest[i] = slots_[i]->TryPop();
            }
        }

        auto choice = decision_->Decide(latest);
        if (!choice) {
            continue;
        }
        if (choice->camera_index >= latest.size()) {
            spdlog::warn("PipelineOrchestrator: decision chose camera {} out of range; skipping",
                         choice->camera_index);
            continue;
        }
        // Bind the slot once so the has_value() check and the dereference below
        // operate on the same optional (a repeated latest[idx] subscript defeats
        // the static optional-access analysis).
        const auto& chosen_slot = latest[choice->camera_index];
        if (!chosen_slot.has_value()) {
            // Decision named a camera with no frame this tick — skip rather than
            // dereference nothing. (StaticDecision never does this.)
            spdlog::warn("PipelineOrchestrator: decision chose camera {} with no frame; skipping",
                         choice->camera_index);
            continue;
        }

        const auto& chosen = *chosen_slot;
        auto final_frame =
            postprocessor_->Process(chosen.source_frame, choice->crop, choice->camera_index);
        if (!final_frame) {
            continue;
        }
        // The recorder gets the CLEAN post-processed frame (no overlay): a clip
        // can be played with or without overlays, and the overlaid version is
        // burned on demand from the stored overlay timeline (#6). Push it BEFORE
        // compositing — IFrameSink::Push copies synchronously, so the recorder
        // captures these clean pixels before the composite below mutates the frame.
        record_sink_.Push(*final_frame);

        // Build the live/broadcast stream frame. SIDE_BY_SIDE (#6 F6d): composite
        // cam0 | cam1 CLEAN (no overlay) — a "see both cameras" monitoring view.
        // Otherwise the SINGLE broadcast view with the overlay baked on.
        auto stream_frame = BuildStreamFrame(*final_frame, latest, choice->camera_index, hold);
        {
            // Retain the latest stream frame for on-demand snapshots so a snapshot
            // matches what the live stream shows. It owns its pixels, so storing by
            // value keeps them alive for a later GrabLatest().
            std::lock_guard lock(latest_frame_mtx_);
            latest_frame_ = stream_frame;
        }
        stream_sink_.Push(stream_frame);
    }
}

auto PipelineOrchestrator::BuildStreamFrame(
    const sst::capture::Frame& clean_chosen,
    const std::vector<std::optional<sst::processing::FrameBundle>>& latest,
    std::size_t chosen_index, StreamHoldState& hold) -> sst::capture::Frame {
    const bool want_side_by_side =
        preview_layout_ != nullptr && compositor_ != nullptr && cameras_.size() >= 2 &&
        preview_layout_->Get() == sst::streaming::PreviewLayout::kSideBySide;

    if (want_side_by_side) {
        return BuildSideBySideFrame(clean_chosen, latest, chosen_index, hold);
    }

    // Not side-by-side this tick: drop any held both-mode state so an
    // intentional layout switch is instant (single frames flow immediately) and
    // a later return to both-mode starts fresh instead of resurrecting a stale
    // composite (U4).
    hold.Reset();

    // SINGLE: composite the current overlay (when present) onto a copy for the
    // live/broadcast stream. A fully-transparent overlay blends to a clean frame;
    // nullopt (no overlay yet, or unsupported format) leaves the frame untouched.
    if (overlay_source_ != nullptr) {
        if (auto overlay = overlay_source_->LatestOverlay()) {
            if (auto composited = sst::overlay::CompositeOverlay(clean_chosen, *overlay)) {
                return std::move(*composited);
            }
        }
    }
    return clean_chosen;
}

// floor-ok: linear miss/hold/re-composition laddering — each rung is one fallback.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PipelineOrchestrator::BuildSideBySideFrame(
    const sst::capture::Frame& clean_chosen,
    const std::vector<std::optional<sst::processing::FrameBundle>>& latest,
    std::size_t chosen_index, StreamHoldState& hold) -> sst::capture::Frame {
    // Composite both cameras CLEAN (no overlay) — a "see both cameras"
    // monitoring view. U4 contract: NEVER emit a bare single-camera frame while
    // the layout is side-by-side — that occasional full single-cam frame is the
    // user-visible both-mode flicker.
    const std::size_t other_index = chosen_index == 0 ? 1 : 0;
    // A selection change flips which camera is "other"; a pane held from the
    // now-chosen camera would show it twice — drop it.
    if (hold.last_other_pane.has_value() && hold.other_pane_index != other_index) {
        hold.last_other_pane.reset();
    }

    // Fresh path: the other camera has a frame this tick. Postprocess +
    // composite call into OpenCV, which throws on a zero-area or mismatched
    // Mat; catch so one bad frame follows the hold path below instead of
    // escaping the consumer thread and tearing down record + preview with it.
    const auto& other_slot = latest[other_index];
    if (other_slot.has_value()) {
        try {
            const sst::processing::CropRect full_frame{
                .x = 0,
                .y = 0,
                .width = other_slot->source_frame.geometry.width,
                .height = other_slot->source_frame.geometry.height};
            if (auto other =
                    postprocessor_->Process(other_slot->source_frame, full_frame, other_index)) {
                hold.last_other_pane = std::move(*other);
                hold.other_pane_index = other_index;
                if (auto composite =
                        ComposePanes(clean_chosen, *hold.last_other_pane, chosen_index)) {
                    hold.stale_ticks = 0;
                    hold.last_composite = *composite;
                    return std::move(*composite);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn(
                "PipelineOrchestrator: side-by-side postprocess/composite failed ({}); "
                "holding previous composite",
                e.what());
        }
    }

    // Miss: empty other slot, postprocess nullopt, composite nullopt, or an
    // OpenCV throw. Short misses repeat the previous composite verbatim — both
    // panes hold, invisible at 30fps. The counter saturates just past the cap
    // (only "over the cap or not" matters; a days-long stall must not overflow).
    if (hold.stale_ticks <= config_.bothmode_hold_ticks) {
        ++hold.stale_ticks;
    }
    if (hold.stale_ticks <= config_.bothmode_hold_ticks && hold.last_composite.has_value()) {
        return *hold.last_composite;
    }

    // Past the hold cap (or nothing held yet): per-tick re-composition. The
    // live chosen camera keeps advancing in its pane while the stale pane holds
    // its last-good frame — a sustained one-camera stall (RECOVERING can last a
    // full serialized restart window) must not freeze the healthy pane. With no
    // last-good pane at all, an empty Frame letterboxes to a black pane.
    try {
        const sst::capture::Frame empty_pane{};
        const sst::capture::Frame& stale_pane =
            hold.last_other_pane.has_value() ? *hold.last_other_pane : empty_pane;
        if (auto composite = ComposePanes(clean_chosen, stale_pane, chosen_index)) {
            hold.last_composite = *composite;
            return std::move(*composite);
        }
    } catch (const std::exception& e) {
        spdlog::warn(
            "PipelineOrchestrator: side-by-side re-composition failed ({}); "
            "holding previous composite",
            e.what());
    }

    // Re-composition itself failed. The held composite is still the best
    // both-mode frame available — repeat it past the cap rather than degrade.
    if (hold.last_composite.has_value()) {
        return *hold.last_composite;
    }
    // No composite has ever succeeded (compositor failing from the first
    // both-mode tick) — the bare chosen frame is the only frame left to emit.
    spdlog::warn(
        "PipelineOrchestrator: no composite ever produced in side-by-side mode; "
        "emitting bare chosen frame");
    return clean_chosen;
}

auto PipelineOrchestrator::ComposePanes(const sst::capture::Frame& chosen,
                                        const sst::capture::Frame& other, std::size_t chosen_index)
    -> std::optional<sst::capture::Frame> {
    // Pane order is POSITIONAL: camera 0 is always the left pane and camera 1
    // the right, regardless of which one is the selected main feed. Changing
    // the selection must not swap the panes.
    const sst::capture::Frame& left = chosen_index == 0 ? chosen : other;
    const sst::capture::Frame& right = chosen_index == 0 ? other : chosen;
    return compositor_->CompositeSideBySide(left, right);
}

auto PipelineOrchestrator::GrabLatest() -> std::optional<sst::capture::Frame> {
    std::lock_guard lock(latest_frame_mtx_);
    return latest_frame_;
}

auto PipelineOrchestrator::ServeFrameTap(FrameTapSlot& tap,
                                         const sst::capture::Frame& frame) -> void {
    if (tap.waiters.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    {
        const std::lock_guard tap_lock(tap.mtx);
        tap.sample = frame;
        ++tap.generation;
    }
    tap.cv.notify_all();
}

auto PipelineOrchestrator::GrabCameraFrame(std::size_t camera_index,
                                           std::chrono::milliseconds timeout)
    -> std::optional<sst::capture::Frame> {
    if (camera_index >= tap_slots_.size() || !running_) {
        return std::nullopt;
    }
    auto& tap = *tap_slots_[camera_index];
    std::unique_lock lock(tap.mtx);
    // Broadcast hand-off: wait for the generation to tick past what we joined
    // at, then take our own descriptor copy. Every concurrent waiter sees the
    // same publication, so multiple samplers (AF + auto-color) never race for
    // a single slot value.
    const std::uint64_t joined_at = tap.generation;
    tap.waiters.fetch_add(1, std::memory_order_relaxed);
    const bool served = tap.cv.wait_for(lock, timeout, [this, &tap, joined_at] {
        return tap.generation != joined_at || !running_;
    });
    const bool last_waiter = tap.waiters.fetch_sub(1, std::memory_order_relaxed) == 1;
    std::optional<sst::capture::Frame> sample;
    if (served && tap.generation != joined_at) {
        sample = tap.sample;
    }
    if (last_waiter) {
        // Don't pin the published frame's pixels past the last interested
        // grab — the next publication would otherwise be the only release.
        tap.sample.reset();
    }
    return sample;  // descriptor copy — pixels shared via the owner ptr
}

}  // namespace sst::pipeline
