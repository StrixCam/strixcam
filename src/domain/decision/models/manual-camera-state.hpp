#pragma once

#include <atomic>
#include <cstdint>

namespace sst::decision {

// The manually-selected output camera index. Written by the SetActiveCamera BLE
// handler, read by ManualDecision every pipeline tick. Lock-free — a single
// scalar. This is the human override of the eventual AI camera decision.
class ManualCameraState {
   public:
    auto Set(std::uint32_t camera_index) -> void {
        index_.store(camera_index, std::memory_order_relaxed);
    }
    [[nodiscard]] auto Get() const -> std::uint32_t {
        return index_.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<std::uint32_t> index_{0};
};

}  // namespace sst::decision
