#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

#include "app/buffer/ports/frame-buffer.hpp"
#include "domain/buffer/models/buffer-policy.hpp"
#include "domain/buffer/models/buffer-stats.hpp"

namespace sst::buffer {

template <typename T>
class LatestOnlySlot final : public IFrameBuffer<T> {
   public:
    LatestOnlySlot() = default;

    LatestOnlySlot(const LatestOnlySlot&) = delete;
    auto operator=(const LatestOnlySlot&) -> LatestOnlySlot& = delete;
    LatestOnlySlot(LatestOnlySlot&&) = delete;
    auto operator=(LatestOnlySlot&&) -> LatestOnlySlot& = delete;

    ~LatestOnlySlot() override = default;

    auto Push(T value) -> bool override {
        bool clean{true};
        {
            std::lock_guard lock(mtx_);
            if (closed_) {
                ++dropped_;
                return false;
            }
            if (slot_.has_value()) {
                clean = false;
                ++dropped_;
            }
            slot_.emplace(std::move(value));
            ++pushed_;
        }
        cv_.notify_one();
        return clean;
    }

    auto Pop(std::chrono::milliseconds timeout) -> std::optional<T> override {
        std::unique_lock lock(mtx_);
        if (!cv_.wait_for(lock, timeout, [this] { return slot_.has_value() || closed_; })) {
            return std::nullopt;
        }
        return Take();
    }

    auto TryPop() -> std::optional<T> override {
        std::lock_guard lock(mtx_);
        return Take();
    }

    auto Close() -> void override {
        {
            std::lock_guard lock(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    auto Stats() const -> BufferStats override {
        std::lock_guard lock(mtx_);
        return BufferStats{
            .policy = BufferPolicy::LatestOnly,
            .capacity = 1,
            .depth = slot_.has_value() ? 1U : 0U,
            .pushed = pushed_,
            .popped = popped_,
            .dropped = dropped_,
        };
    }

   private:
    auto Take() -> std::optional<T> {
        if (!slot_.has_value()) {
            return std::nullopt;
        }
        std::optional<T> out = std::move(slot_);
        slot_.reset();
        ++popped_;
        return out;
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::optional<T> slot_;
    bool closed_{false};
    std::uint64_t pushed_{0};
    std::uint64_t popped_{0};
    std::uint64_t dropped_{0};
};

}  // namespace sst::buffer
