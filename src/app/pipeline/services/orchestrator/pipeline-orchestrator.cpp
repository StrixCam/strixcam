#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <exception>
#include <utility>

#include "domain/buffer/services/materialize-frame.hpp"
#include "domain/decision/models/camera-choice.hpp"
#include "domain/overlay/services/frame-compositor.hpp"

namespace sst::pipeline {

namespace {
// The consumer blocks on this camera's slot to pace the loop at capture rate.
constexpr std::size_t kCadenceCamera = 0;
}  // namespace

PipelineOrchestrator::PipelineOrchestrator(
    std::vector<CameraChain> cameras,
    std::unique_ptr<sst::processing::IPostprocessor> postprocessor,
    std::unique_ptr<sst::decision::IDecision> decision,
    // NOLINTBEGIN(bugprone-easily-swappable-parameters) // floor-ok: clean L1 vs overlaid stream
    // are distinct sinks
    sst::buffer::IFrameSink& record_sink, sst::buffer::IFrameSink& stream_sink,
    // NOLINTEND(bugprone-easily-swappable-parameters)
    PipelineConfig config, sst::raw_capture::IRawCaptureSink* raw_sink,
    sst::overlay::IOverlayFrameSource* overlay_source,
    sst::processing::IFrameCompositor* compositor,
    sst::streaming::PreviewLayoutState* preview_layout)
    : cameras_(std::move(cameras)),
      postprocessor_(std::move(postprocessor)),
      decision_(std::move(decision)),
      record_sink_(record_sink),
      stream_sink_(stream_sink),
      config_(config),
      raw_sink_(raw_sink),
      overlay_source_(overlay_source),
      compositor_(compositor),
      preview_layout_(preview_layout) {
    slots_.reserve(cameras_.size());
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        slots_.push_back(
            std::make_unique<sst::buffer::LatestOnlySlot<sst::processing::FrameBundle>>());
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
            spdlog::error("PipelineOrchestrator::Start: camera {} failed to start", i);
            // Roll back any captures already started so we never leave half the
            // cameras running on a failed Start().
            for (std::size_t j = 0; j < i; ++j) {
                cameras_[j].capture->Stop();
            }
            return false;
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

auto PipelineOrchestrator::ProducerLoop(std::size_t camera_index) -> void {
    auto& capture = *cameras_[camera_index].capture;
    auto& preprocessor = *cameras_[camera_index].preprocessor;
    auto& slot = *slots_[camera_index];

    while (running_) {
        auto raw = capture.Capture();
        if (!raw) {
            std::this_thread::sleep_for(config_.capture_idle_sleep);
            continue;
        }
        auto bundle = preprocessor.Process(*raw);
        if (!bundle) {
            // Preprocess failure is logged inside Process(); drop and continue.
            continue;
        }
        // Materialize the source plane bytes off the GstBuffer so the appsink
        // slot is freed before the bundle crosses to the consumer thread.
        bundle->source_frame = sst::buffer::MaterializeFrame(bundle->source_frame);
        // Fork the materialized frame to raw capture BEFORE the std::move below
        // (tapping after the move would read a moved-from bundle). The sink
        // copies what it needs and no-ops cheaply when not capturing.
        if (raw_sink_ != nullptr) {
            raw_sink_->PushCamera(static_cast<std::uint32_t>(camera_index), bundle->source_frame);
        }
        slot.Push(std::move(*bundle));
    }
}

auto PipelineOrchestrator::ConsumerLoop() -> void {
    while (running_) {
        // Block on the cadence camera so the loop runs at capture rate; sample every
        // other camera non-blocking. Each tick consumes the latest bundle from each
        // slot — unchosen ones are dropped (aged out) here.
        std::vector<std::optional<sst::processing::FrameBundle>> latest(cameras_.size());
        latest[kCadenceCamera] = slots_[kCadenceCamera]->Pop(config_.consumer_pop_timeout);
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (i != kCadenceCamera) {
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
        auto final_frame = postprocessor_->Process(chosen.source_frame, choice->crop);
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
        auto stream_frame = BuildStreamFrame(*final_frame, latest, choice->camera_index);
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
    std::size_t chosen_index) -> sst::capture::Frame {
    const bool want_side_by_side =
        preview_layout_ != nullptr && compositor_ != nullptr && cameras_.size() >= 2 &&
        preview_layout_->Get() == sst::streaming::PreviewLayout::kSideBySide;

    if (want_side_by_side) {
        // Composite the chosen camera (left) with the other camera (right), both
        // CLEAN — no overlay in the monitoring view. Postprocess the other
        // camera full-frame; if it has no frame this tick or the composite
        // fails, fall back to the clean chosen frame (still no overlay).
        const std::size_t other_index = chosen_index == 0 ? 1 : 0;
        const auto& other_slot = latest[other_index];
        if (other_slot.has_value()) {
            // Postprocess + composite call into OpenCV, which throws on a
            // zero-area or mismatched Mat. Catch here so one bad frame degrades
            // to the clean single view instead of escaping the consumer thread
            // and tearing down record + preview with it.
            try {
                const sst::processing::CropRect full_frame{
                    .x = 0,
                    .y = 0,
                    .width = other_slot->source_frame.geometry.width,
                    .height = other_slot->source_frame.geometry.height};
                if (auto right = postprocessor_->Process(other_slot->source_frame, full_frame)) {
                    if (auto composite = compositor_->CompositeSideBySide(clean_chosen, *right)) {
                        return std::move(*composite);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn(
                    "PipelineOrchestrator: side-by-side composite failed ({}); "
                    "falling back to single frame",
                    e.what());
            }
        }
        return clean_chosen;
    }

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

auto PipelineOrchestrator::GrabLatest() -> std::optional<sst::capture::Frame> {
    std::lock_guard lock(latest_frame_mtx_);
    return latest_frame_;
}

}  // namespace sst::pipeline
