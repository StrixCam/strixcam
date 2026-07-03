#pragma once

#include <atomic>

namespace sst::processing {

// Rolling average of the most recent postprocessed frame's PRE-correction BGR,
// written by the postprocessor each frame and read by the auto-white-balance
// handler to compute grey-world gains. Lock-free: a torn read across the three
// means is harmless (one stale channel on a still scene changes nothing).
class FrameColorStats {
   public:
    struct Means {
        float b;
        float g;
        float r;
        bool valid;
    };

    auto Set(float mean_b, float mean_g, float mean_r) -> void {
        b_.store(mean_b, std::memory_order_relaxed);
        g_.store(mean_g, std::memory_order_relaxed);
        r_.store(mean_r, std::memory_order_relaxed);
        valid_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] auto Get() const -> Means {
        return {b_.load(std::memory_order_relaxed), g_.load(std::memory_order_relaxed),
                r_.load(std::memory_order_relaxed), valid_.load(std::memory_order_relaxed)};
    }

   private:
    std::atomic<float> b_{0.0F};
    std::atomic<float> g_{0.0F};
    std::atomic<float> r_{0.0F};
    std::atomic<bool> valid_{false};
};

}  // namespace sst::processing
