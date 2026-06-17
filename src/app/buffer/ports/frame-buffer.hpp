#pragma once

#include <chrono>
#include <optional>

#include "domain/buffer/models/buffer-stats.hpp"

namespace sst::buffer {

template <typename T>
class IFrameBuffer {
   public:
    virtual ~IFrameBuffer() = default;

    // Push a value. Always succeeds; returns true if the value was stored without
    // displacing another (clean push), false if a previous value was overwritten
    // or evicted to make room. Producers should treat the return value as
    // telemetry only; control flow does not depend on it.
    virtual auto Push(T value) -> bool = 0;

    // Block up to `timeout` waiting for a value. Returns std::nullopt on timeout
    // or after Close() with no remaining values.
    virtual auto Pop(std::chrono::milliseconds timeout) -> std::optional<T> = 0;

    // Non-blocking pop. Returns std::nullopt if empty.
    virtual auto TryPop() -> std::optional<T> = 0;

    // Wakes any blocked consumers. Subsequent Pop calls drain remaining values
    // then return std::nullopt. Subsequent Push calls are dropped.
    virtual auto Close() -> void = 0;

    [[nodiscard]] virtual auto Stats() const -> BufferStats = 0;
};

}  // namespace sst::buffer
