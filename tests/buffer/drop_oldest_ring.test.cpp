#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "domain/buffer/models/buffer-policy.hpp"
#include "domain/buffer/services/drop-oldest-ring.hpp"

using namespace std::chrono_literals;

namespace sst::buffer {

TEST(DropOldestRingTest, RejectsZeroCapacity) {
    EXPECT_THROW(DropOldestRing<int>(0), std::invalid_argument);
}

TEST(DropOldestRingTest, FifoOrdering) {
    DropOldestRing<int> buf(4);
    EXPECT_TRUE(buf.Push(1));
    EXPECT_TRUE(buf.Push(2));
    EXPECT_TRUE(buf.Push(3));

    EXPECT_EQ(*buf.TryPop(), 1);
    EXPECT_EQ(*buf.TryPop(), 2);
    EXPECT_EQ(*buf.TryPop(), 3);
    EXPECT_FALSE(buf.TryPop().has_value());
}

TEST(DropOldestRingTest, OverflowDropsOldestEntry) {
    DropOldestRing<int> buf(2);
    EXPECT_TRUE(buf.Push(1));
    EXPECT_TRUE(buf.Push(2));
    EXPECT_FALSE(buf.Push(3));  // evicts 1
    EXPECT_FALSE(buf.Push(4));  // evicts 2

    EXPECT_EQ(*buf.TryPop(), 3);
    EXPECT_EQ(*buf.TryPop(), 4);

    const auto stats = buf.Stats();
    EXPECT_EQ(stats.policy, BufferPolicy::DropOldest);
    EXPECT_EQ(stats.capacity, 2U);
    EXPECT_EQ(stats.pushed, 4U);
    EXPECT_EQ(stats.popped, 2U);
    EXPECT_EQ(stats.dropped, 2U);
    EXPECT_EQ(stats.depth, 0U);
}

TEST(DropOldestRingTest, PopBlocksUntilPushed) {
    DropOldestRing<int> buf(4);
    std::atomic<bool> got{false};

    std::thread consumer([&] {
        auto val = buf.Pop(500ms);
        if (val && *val == 11) {  // NOLINT(readability-magic-numbers) self-evident payload
            got.store(true);
        }
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(got.load());

    buf.Push(11);  // NOLINT(readability-magic-numbers) self-evident payload
    consumer.join();
    EXPECT_TRUE(got.load());
}

TEST(DropOldestRingTest, PopReturnsNulloptOnTimeout) {
    DropOldestRing<int> buf(4);
    auto start = std::chrono::steady_clock::now();
    auto out = buf.Pop(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(out.has_value());
    EXPECT_GE(elapsed, 45ms);
}

TEST(DropOldestRingTest, CloseDrainsThenReturnsNullopt) {
    DropOldestRing<int> buf(4);
    buf.Push(1);
    buf.Push(2);
    buf.Close();

    EXPECT_EQ(*buf.TryPop(), 1);
    EXPECT_EQ(*buf.TryPop(), 2);
    EXPECT_FALSE(buf.TryPop().has_value());
    EXPECT_FALSE(buf.Push(3));
}

TEST(DropOldestRingTest, ConcurrentProducerConsumerPreservesCount) {
    DropOldestRing<int> buf(8);  // NOLINT(readability-magic-numbers) ring capacity
    constexpr int kIters = 5000;

    std::thread producer([&] {
        for (int i = 0; i < kIters; ++i) {
            buf.Push(i);
        }
    });

    int consumed = 0;
    std::thread consumer([&] {
        while (consumed < kIters) {
            if (buf.Pop(50ms).has_value()) {
                ++consumed;
            } else if (buf.Stats().pushed >= static_cast<std::uint64_t>(kIters) &&
                       buf.Stats().depth == 0) {
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    const auto stats = buf.Stats();
    EXPECT_EQ(stats.pushed, static_cast<std::uint64_t>(kIters));
    EXPECT_EQ(stats.popped + stats.dropped, stats.pushed);
}

}  // namespace sst::buffer
