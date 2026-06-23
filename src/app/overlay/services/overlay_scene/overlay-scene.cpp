#include "app/overlay/services/overlay_scene/overlay-scene.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace sst::overlay {

namespace {

constexpr std::uint64_t kMsPerSecond = 1000;
constexpr std::uint32_t kSecondsPerMinute = 60;

auto SubstituteParams(std::string text,
                      const std::map<std::string, std::string>& params) -> std::string {
    for (const auto& [key, value] : params) {
        const std::string token = "{{" + key + "}}";
        std::string::size_type pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), value);
            pos += value.size();
        }
    }
    return text;
}

}  // namespace

auto OverlayScene::SetLayout(OverlayLayout layout) -> void {
    layout_ = std::move(layout);
    has_layout_ = true;
    // A new layout invalidates any in-flight banners (they referenced the old
    // template set).
    banners_.clear();
}

auto OverlayScene::SetBindingData(const BindingData& data) -> void { data_ = data; }

// Public contract declared in overlay-scene.hpp and called from
// overlay-controller.cpp and tests; reordering would change that cross-file
// signature. The params carry distinct, self-documenting names (a duration
// override vs. a wall-clock timestamp).
auto OverlayScene::ActivateBanner(
    const std::string& template_id, const std::map<std::string, std::string>& params,
    std::uint32_t duration_s_override,  // NOLINT(bugprone-easily-swappable-parameters) // floor-ok:
                                        // distinct widths (u32 duration vs u64 timestamp) +
                                        // self-documenting names; no natural reorder
    std::uint64_t now_ms) -> bool {
    if (!has_layout_) {
        return false;
    }
    const auto match =
        std::find_if(layout_.templates.begin(), layout_.templates.end(),
                     [&](const OverlayTemplate& tmpl) { return tmpl.event_type == template_id; });
    if (match == layout_.templates.end()) {
        return false;
    }

    // R19: command duration_s wins; otherwise fall back to the template's
    // duration_ms.
    const std::uint64_t duration_ms =
        duration_s_override > 0 ? static_cast<std::uint64_t>(duration_s_override) * kMsPerSecond
                                : match->duration_ms;

    ActiveBanner banner;
    banner.expires_at_ms = now_ms + duration_ms;
    banner.elements = match->elements;
    for (auto& element : banner.elements) {
        element.visible = true;
        element.style.static_text = SubstituteParams(element.style.static_text, params);
    }
    banners_.push_back(std::move(banner));
    return true;
}

auto OverlayScene::ExpireBanners(std::uint64_t now_ms) -> void {
    banners_.erase(
        std::remove_if(banners_.begin(), banners_.end(),
                       [&](const ActiveBanner& banner) { return banner.expires_at_ms <= now_ms; }),
        banners_.end());
}

auto OverlayScene::ResolveText(const OverlayElement& element) const -> std::string {
    if (element.shape != OverlayShape::kText) {
        return {};
    }
    switch (element.binding) {
        case OverlayBinding::kStatic:
            return element.style.static_text;
        case OverlayBinding::kScoreA:
            return fmt::format("{}", data_.score_a);
        case OverlayBinding::kScoreB:
            return fmt::format("{}", data_.score_b);
        case OverlayBinding::kScoreVs:
            return fmt::format("{} – {}", data_.score_a, data_.score_b);  // en dash
        case OverlayBinding::kTeamAName:
            return data_.team_a_name;
        case OverlayBinding::kTeamBName:
            return data_.team_b_name;
        case OverlayBinding::kMatchClock:
            return fmt::format("{:02}:{:02}", data_.clock_seconds / kSecondsPerMinute,
                               data_.clock_seconds % kSecondsPerMinute);
        case OverlayBinding::kPeriodLabel:
            switch (data_.period_state) {
                case PeriodLabelState::kHalfTime:
                    return "HT";
                case PeriodLabelState::kFullTime:
                    return "FT";
                case PeriodLabelState::kInPlay:
                    break;
            }
            return fmt::format("P{}", data_.period);
    }
    return {};
}

auto OverlayScene::Build(std::uint64_t now_ms) -> RenderScene {
    ExpireBanners(now_ms);

    RenderScene scene;
    scene.canvas_width = layout_.canvas_width;
    scene.canvas_height = layout_.canvas_height;

    const auto add_element = [&](const OverlayElement& element) {
        if (!element.visible) {
            return;
        }
        scene.elements.push_back(RenderElement{
            .shape = element.shape,
            .bounds = element.bounds,
            .style = element.style,
            .text = ResolveText(element),
        });
    };

    for (const auto& element : layout_.elements) {
        add_element(element);
    }
    for (const auto& banner : banners_) {
        for (const auto& element : banner.elements) {
            add_element(element);
        }
    }

    std::stable_sort(scene.elements.begin(), scene.elements.end(),
                     [](const RenderElement& lhs, const RenderElement& rhs) {
                         return lhs.bounds.z < rhs.bounds.z;
                     });
    return scene;
}

}  // namespace sst::overlay
