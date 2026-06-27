#pragma once

#include <nlohmann/json.hpp>

#include "domain/overlay/models/overlay-enums.hpp"
#include "domain/overlay/models/overlay-layout.hpp"
#include "domain/overlay/models/render-scene.hpp"

// nlohmann ADL serde for a fully-resolved RenderScene — the unit persisted in
// the overlay timeline (#6 F6b). Every field is written and read back via
// `.at()` so a round-trip is total: the burn (F6c) replays exactly what was
// shown. Enums are stored as their integer value (stable wire-independent of the
// proto). Mirrors the inline-serde style of adapters/config/json/serde/*.

namespace sst::overlay {

using nlohmann::json;

inline void to_json(json& out, const OverlayRect& rect) {
    out = json{{"x1", rect.x1}, {"y1", rect.y1}, {"x2", rect.x2}, {"y2", rect.y2}, {"z", rect.z}};
}

inline void from_json(const json& node, OverlayRect& rect) {
    node.at("x1").get_to(rect.x1);
    node.at("y1").get_to(rect.y1);
    node.at("x2").get_to(rect.x2);
    node.at("y2").get_to(rect.y2);
    node.at("z").get_to(rect.z);
}

inline void to_json(json& out, const OverlayStyle& style) {
    out = json{{"fill", style.fill_color},
               {"text_color", style.text_color},
               {"opacity", style.opacity},
               {"corner_radius", style.corner_radius},
               {"font_family", style.font_family},
               {"font_size", style.font_size},
               {"text_align", static_cast<int>(style.text_align)},
               {"font_weight", static_cast<int>(style.font_weight)},
               {"static_text", style.static_text}};
}

inline void from_json(const json& node, OverlayStyle& style) {
    node.at("fill").get_to(style.fill_color);
    node.at("text_color").get_to(style.text_color);
    node.at("opacity").get_to(style.opacity);
    node.at("corner_radius").get_to(style.corner_radius);
    node.at("font_family").get_to(style.font_family);
    node.at("font_size").get_to(style.font_size);
    style.text_align = static_cast<TextAlign>(node.at("text_align").get<int>());
    style.font_weight = static_cast<FontWeight>(node.at("font_weight").get<int>());
    node.at("static_text").get_to(style.static_text);
}

inline void to_json(json& out, const RenderElement& element) {
    out = json{{"shape", static_cast<int>(element.shape)},
               {"bounds", element.bounds},
               {"style", element.style},
               {"text", element.text}};
}

inline void from_json(const json& node, RenderElement& element) {
    element.shape = static_cast<OverlayShape>(node.at("shape").get<int>());
    node.at("bounds").get_to(element.bounds);
    node.at("style").get_to(element.style);
    node.at("text").get_to(element.text);
}

inline void to_json(json& out, const RenderScene& scene) {
    out = json{{"w", scene.canvas_width}, {"h", scene.canvas_height}, {"elements", scene.elements}};
}

inline void from_json(const json& node, RenderScene& scene) {
    node.at("w").get_to(scene.canvas_width);
    node.at("h").get_to(scene.canvas_height);
    node.at("elements").get_to(scene.elements);
}

}  // namespace sst::overlay
