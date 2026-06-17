#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "app/buffer/ports/frame-buffer.hpp"
#include "domain/buffer/models/buffer-policy.hpp"
#include "domain/buffer/models/buffer-stats.hpp"

namespace sst::buffer {

template <typename T>
class DropOldestRing final : public IFrameBuffer<T> {
   public:
    explicit DropOldestRing(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("DropOldestRing capacity must be > 0");
        }
    }

    DropOldestRing(const DropOldestRing&) = delete;
    auto operator=(const DropOldestRing&) -> DropOldestRing& = delete;
    DropOldestRing(DropOldestRing&&) = delete;
    auto operator=(DropOldestRing&&) -> DropOldestRing& = delete;

    ~DropOldestRing() override = default;

    auto Push(T value) -> bool override {
        bool clean{true};
        {
            std::lock_guard lock(mtx_);
            if (closed_) {
                ++dropped_;
                return false;
            }
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                ++dropped_;
                clean = false;
            }
            queue_.emplace_back(std::move(value));
            ++pushed_;
        }
        cv_.notify_one();
        return clean;
    }

    auto Pop(std::chrono::milliseconds timeout) -> std::optional<T> override {
        std::unique_lock lock(mtx_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; })) {
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
            .policy = BufferPolicy::DropOldest,
            .capacity = capacity_,
            .depth = queue_.size(),
            .pushed = pushed_,
            .popped = popped_,
            .dropped = dropped_,
        };
    }

   private:
    auto Take() -> std::optional<T> {
        if (queue_.empty()) {
            return std::nullopt;
        }
        std::optional<T> out = std::move(queue_.front());
        queue_.pop_front();
        ++popped_;
        return out;
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<T> queue_;
    const std::size_t capacity_;
    bool closed_{false};
    std::uint64_t pushed_{0};
    std::uint64_t popped_{0};
    std::uint64_t dropped_{0};
};

}  // namespace sst::buffer
