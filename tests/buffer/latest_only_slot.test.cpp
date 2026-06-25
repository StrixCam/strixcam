#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "domain/buffer/models/buffer-policy.hpp"
#include "domain/buffer/services/latest-only-slot.hpp"

using namespace std::chrono_literals;

namespace sst::buffer {

TEST(LatestOnlySlotTest, PushPopRoundTrip) {
    LatestOnlySlot<int> buf;
    EXPECT_TRUE(buf.Push(42));

    auto out = buf.TryPop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 42);
    EXPECT_FALSE(buf.TryPop().has_value());
}

TEST(LatestOnlySlotTest, NewerPushOverwritesOlder) {
    LatestOnlySlot<int> buf;
    EXPECT_TRUE(buf.Push(1));
    EXPECT_FALSE(buf.Push(2));  // overwrite -> clean=false
    EXPECT_FALSE(buf.Push(3));

    auto out = buf.TryPop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 3);

    const auto stats = buf.Stats();
    EXPECT_EQ(stats.policy, BufferPolicy::LatestOnly);
    EXPECT_EQ(stats.capacity, 1U);
    EXPECT_EQ(stats.pushed, 3U);
    EXPECT_EQ(stats.popped, 1U);
    EXPECT_EQ(stats.dropped, 2U);
    EXPECT_EQ(stats.depth, 0U);
}

TEST(LatestOnlySlotTest, PopBlocksUntilPushed) {
    LatestOnlySlot<int> buf;
    std::atomic<bool> got{false};

    std::thread consumer([&] {
        auto val = buf.Pop(500ms);
        if (val && *val == 99) {  // NOLINT(readability-magic-numbers) self-evident payload
            got.store(true);
        }
    });

    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(got.load());

    buf.Push(99);  // NOLINT(readability-magic-numbers) self-evident payload
    consumer.join();
    EXPECT_TRUE(got.load());
}

TEST(LatestOnlySlotTest, PopReturnsNulloptOnTimeout) {
    LatestOnlySlot<int> buf;
    auto start = std::chrono::steady_clock::now();
    auto out = buf.Pop(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(out.has_value());
    EXPECT_GE(elapsed, 45ms);
}

TEST(LatestOnlySlotTest, CloseWakesBlockedPop) {
    LatestOnlySlot<int> buf;
    std::atomic<bool> returned{false};

    std::thread consumer([&] {
        auto val = buf.Pop(5s);
        EXPECT_FALSE(val.has_value());
        returned.store(true);
    });

    std::this_thread::sleep_for(20ms);
    buf.Close();
    consumer.join();
    EXPECT_TRUE(returned.load());

    EXPECT_FALSE(buf.Push(7));
}

TEST(LatestOnlySlotTest, MoveOnlyPayload) {
    LatestOnlySlot<std::unique_ptr<int>> buf;
    // 7 is a self-evident payload used only to assert move-through.
    buf.Push(std::make_unique<int>(7));  // NOLINT(readability-magic-numbers)
    auto out = buf.TryPop();
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(*out);
    EXPECT_EQ(**out, 7);  // NOLINT(readability-magic-numbers)
}

}  // namespace sst::buffer
