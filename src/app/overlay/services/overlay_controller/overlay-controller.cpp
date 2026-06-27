#include "app/overlay/services/overlay_controller/overlay-controller.hpp"

#include <fmt/format.h>

#include <utility>

namespace sst::overlay {

OverlayController::OverlayController(IOverlayRenderer& renderer, IOverlaySink& sink,
                                     common::OutputSize out_size,
                                     IOverlayTimelineRecorder* timeline)
    : renderer_(renderer), sink_(sink), out_size_(out_size), timeline_(timeline) {}

auto OverlayController::SetLayout(OverlayLayout layout) -> void {
    std::lock_guard lock(mtx_);
    scene_.SetLayout(std::move(layout));
}

auto OverlayController::SetBindingData(const BindingData& data) -> void {
    std::lock_guard lock(mtx_);
    scene_.SetBindingData(data);
}

auto OverlayController::ActivateBanner(const std::string& template_id,
                                       const std::map<std::string, std::string>& params,
                                       std::uint32_t duration_s_override,
                                       std::uint64_t now_ms) -> bool {
    std::lock_guard lock(mtx_);
    return scene_.ActivateBanner(template_id, params, duration_s_override, now_ms);
}

auto OverlayController::Signature(const RenderScene& scene) -> std::string {
    std::string sig = fmt::format("{}x{}|", scene.canvas_width, scene.canvas_height);
    for (const auto& element : scene.elements) {
        sig +=
            fmt::format("{};{},{},{},{},{};", static_cast<int>(element.shape), element.bounds.x1,
                        element.bounds.y1, element.bounds.x2, element.bounds.y2, element.bounds.z);
        sig += element.text;
        sig += ';';
        sig += element.style.fill_color;
        sig += ';';
        sig += element.style.text_color;
        sig += fmt::format(";{};{}|", element.style.opacity,
                           static_cast<int>(element.style.corner_radius));
    }
    return sig;
}

auto OverlayController::Refresh(std::uint64_t now_ms) -> bool {
    std::lock_guard lock(mtx_);
    const RenderScene scene = scene_.Build(now_ms);
    const std::string sig = Signature(scene);
    if (pushed_once_ && sig == last_signature_) {
        return false;  // nothing visibly changed — don't re-render or push
    }
    last_signature_ = sig;
    pushed_once_ = true;

    // Persist the resolved scene to the recording's overlay timeline (#6 F6b)
    // before rasterizing. The recorder no-ops outside a recording. Capturing the
    // scene (not the RGBA) keeps the timeline compact and lets F6c re-rasterize.
    if (timeline_ != nullptr) {
        timeline_->OnScene(now_ms, scene);
    }

    RgbaImage frame = renderer_.Render(scene, out_size_.width, out_size_.height);
    sink_.PushFrame(frame);
    ++push_count_;
    return true;
}

}  // namespace sst::overlay
