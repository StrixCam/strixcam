#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace sst::composition {

// Composition-root helper: runs `tick` every `interval` on an owned background
// thread until destroyed (or Stop()). The wait is condition-variable based, so
// shutdown is immediate rather than sleeping out the interval — the same
// cv-interruptible poller pattern the telemetry probe and autofocus loop use,
// packaged for main()'s ad-hoc periodic threads (match clock, proxy-retention
// sweep). The first tick fires one interval after Start(), matching the
// sleep-then-tick loops it replaces.
class PeriodicWorker {
   public:
    PeriodicWorker(std::chrono::milliseconds interval, std::function<void()> tick)
        : interval_(interval), tick_(std::move(tick)) {}

    ~PeriodicWorker() { Stop(); }

    PeriodicWorker(const PeriodicWorker&) = delete;
    auto operator=(const PeriodicWorker&) -> PeriodicWorker& = delete;
    PeriodicWorker(PeriodicWorker&&) = delete;
    auto operator=(PeriodicWorker&&) -> PeriodicWorker& = delete;

    // Idempotent. The worker owns its thread; Stop() joins it promptly (a tick
    // in flight finishes first).
    auto Start() -> void {
        std::lock_guard lock(mtx_);
        if (running_) {
            return;
        }
        running_ = true;
        thread_ = std::thread([this] { Loop(); });
    }

    auto Stop() -> void {
        {
            std::lock_guard lock(mtx_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

   private:
    auto Loop() -> void {
        std::unique_lock lock(mtx_);
        for (;;) {
            // Wait out the interval, waking immediately on Stop() so shutdown
            // never blocks on a sleeping worker.
            if (cv_.wait_for(lock, interval_, [this] { return !running_; })) {
                return;
            }
            lock.unlock();
            tick_();
            lock.lock();
        }
    }

    const std::chrono::milliseconds interval_;
    const std::function<void()> tick_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool running_{false};
    std::thread thread_;
};

}  // namespace sst::composition
