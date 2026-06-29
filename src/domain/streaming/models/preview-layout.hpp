#pragma once

#include <atomic>
#include <cstdint>

namespace sst::streaming {

// Live preview composition (#6 F6d). kSingle = the active camera with overlay
// baked in (the broadcast view; default). kSideBySide = cam0 | cam1 composited
// into the single preview stream, clean (no overlay) — a "see both cameras"
// monitoring view. The RTSP geometry is unchanged: both cameras are letterboxed
// into the existing fixed output canvas, so the encoder caps never renegotiate
// and connected viewers are never dropped.
enum class PreviewLayout : std::uint8_t { kSingle = 0, kSideBySide = 1 };

// Thread-safe holder shared between the SetPreviewLayout BLE handler (which
// writes it) and the pipeline consumer thread (which reads it every tick to
// decide whether to composite). A lock-free atomic — the read is on the hot
// per-frame path.
class PreviewLayoutState {
   public:
    auto Set(PreviewLayout layout) -> void { layout_.store(layout, std::memory_order_relaxed); }

    [[nodiscard]] auto Get() const -> PreviewLayout {
        return layout_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<PreviewLayout> layout_{PreviewLayout::kSingle};
};

}  // namespace sst::streaming
