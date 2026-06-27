#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/ports/overlay-timeline-recorder.hpp"
#include "app/overlay/services/overlay_scene/overlay-scene.hpp"
#include "domain/common/models/output-size.hpp"
#include "domain/overlay/models/overlay-layout.hpp"

namespace sst::overlay {

// Glues the scene to the renderer + compositor sink with push-on-change
// semantics (R18/R20): a new RGBA frame is rendered and pushed only when the
// resolved scene actually differs from the last one pushed. Between changes the
// compositor keeps showing the last buffer, so identical updates don't thrash
// the pipeline.
class OverlayController {
   public:
    // `timeline` (optional, may be null) receives every pushed scene so it can
    // be persisted during a recording (#6 F6b). Null disables timeline capture.
    OverlayController(IOverlayRenderer& renderer, IOverlaySink& sink, common::OutputSize out_size,
                      IOverlayTimelineRecorder* timeline = nullptr);

    auto SetLayout(OverlayLayout layout) -> void;
    auto SetBindingData(const BindingData& data) -> void;
    auto ActivateBanner(const std::string& template_id,
                        const std::map<std::string, std::string>& params,
                        std::uint32_t duration_s_override, std::uint64_t now_ms) -> bool;

    // Rebuild the scene at now_ms and push a new frame only if it changed.
    // Returns true if a frame was pushed.
    auto Refresh(std::uint64_t now_ms) -> bool;

    [[nodiscard]] auto PushCount() const -> std::uint64_t {
        std::lock_guard lock(mtx_);
        return push_count_;
    }

   private:
    [[nodiscard]] static auto Signature(const RenderScene& scene) -> std::string;

    // Guards all scene/signature state: mutators run from the BLE event-loop
    // thread (command handlers) while the 1 Hz match-clock thread also calls
    // Refresh() concurrently.
    mutable std::mutex mtx_;
    IOverlayRenderer& renderer_;
    IOverlaySink& sink_;
    common::OutputSize out_size_;
    IOverlayTimelineRecorder* timeline_;
    OverlayScene scene_;
    std::string last_signature_;
    bool pushed_once_{false};
    std::uint64_t push_count_{0};
};

}  // namespace sst::overlay
