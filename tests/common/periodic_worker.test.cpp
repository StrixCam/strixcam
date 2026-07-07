#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "domain/common/services/periodic-worker.hpp"

using namespace std::chrono_literals;

namespace sst::common {

TEST(PeriodicWorkerTest, TicksRepeatedlyAtInterval) {
    std::atomic<int> ticks{0};
    PeriodicWorker worker(5ms, [&ticks] { ticks.fetch_add(1); });
    worker.Start();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (ticks.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    worker.Stop();
    EXPECT_GE(ticks.load(), 3);
}

TEST(PeriodicWorkerTest, StopInterruptsTheWaitPromptly) {
    std::atomic<int> ticks{0};
    PeriodicWorker worker(1h, [&ticks] { ticks.fetch_add(1); });
    worker.Start();
    // The worker is parked inside a 1h cv wait; Stop() must wake it and join
    // without waiting out the interval.
    const auto before = std::chrono::steady_clock::now();
    worker.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 10s);
    EXPECT_EQ(ticks.load(), 0);  // first tick fires only after a full interval
}

TEST(PeriodicWorkerTest, StartAndStopAreIdempotent) {
    std::atomic<int> ticks{0};
    PeriodicWorker worker(5ms, [&ticks] { ticks.fetch_add(1); });
    worker.Start();
    worker.Start();  // no second thread, no crash
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (ticks.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    worker.Stop();
    worker.Stop();  // already joined — no-op
    EXPECT_GE(ticks.load(), 1);
}

TEST(PeriodicWorkerTest, DestructorJoinsARunningWorker) {
    std::atomic<int> ticks{0};
    {
        PeriodicWorker worker(1ms, [&ticks] { ticks.fetch_add(1); });
        worker.Start();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (ticks.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
    }  // destructor stops + joins — a tick after this point would be UB
    const int after_destruction = ticks.load();
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(ticks.load(), after_destruction);
}

}  // namespace sst::common
