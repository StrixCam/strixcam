// Record-or-stream proxy ref-count (state-health cycle U5): the training proxy
// runs while a recording OR a platform stream is active and stops on last-out.
// Pure — fake IRawCaptureSink counting transitions and flagging any
// double-start (Start while already capturing), which the lifecycle's single
// mutex must make impossible.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/storage/ports/raw-capture-sink.hpp"
#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"

namespace {

using sst::storage::ProxyLifecycle;

class FakeSink final : public sst::storage::IRawCaptureSink {
   public:
    auto Start(const std::string& capture_group_id,
               const std::filesystem::path& output_dir) -> bool override {
        if (capturing_.load()) {
            ++double_start_violations;  // lifecycle must never do this
            return false;
        }
        ++starts;
        last_group = capture_group_id;
        last_dir = output_dir;
        capturing_ = start_ok;
        return start_ok;
    }
    auto PushCamera(std::uint32_t /*camera_index*/,
                    const sst::capture::Frame& /*frame*/) -> void override {}
    auto Stop() -> bool override {
        ++stops;
        const bool was = capturing_.load();
        capturing_ = false;
        return was;
    }
    [[nodiscard]] auto IsCapturing() const -> bool override { return capturing_.load(); }

    bool start_ok{true};
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> double_start_violations{0};
    std::string last_group;
    std::filesystem::path last_dir;

   private:
    std::atomic<bool> capturing_{false};
};

// Deterministic epoch-ms clock so the interim stream-minted group id is
// assertable ("stream-42").
constexpr std::uint64_t kFixedEpochMs = 42;

auto FixedClock() -> std::uint64_t { return kFixedEpochMs; }

// The plan's truth table: record start -> up; stream start -> still up (no
// second start); record stop -> still up (stream holds); stream stop -> down.
TEST(ProxyLifecycleTest, RecordOrStreamTruthTable) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnRecordingStart("match-1", "/videos/match-1");
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 1);
    EXPECT_EQ(sink.last_group, "match-1");
    EXPECT_EQ(sink.last_dir, std::filesystem::path{"/videos/match-1"});

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 1);  // already running — no second start

    lifecycle.OnRecordingStop();
    EXPECT_TRUE(sink.IsCapturing());  // stream still holds
    EXPECT_EQ(sink.stops, 0);

    lifecycle.OnStreamingStop();
    EXPECT_FALSE(sink.IsCapturing());  // last-out stops
    EXPECT_EQ(sink.stops, 1);
}

// Streaming-only session: the lifecycle mints an interim `stream-<epoch-ms>`
// id (no app-minted group, no per-match dir) so training footage still exists.
TEST(ProxyLifecycleTest, StreamOnlyMintsInterimGroupAtSinkRoot) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.last_group, "stream-42");
    EXPECT_TRUE(sink.last_dir.empty());  // sink construction root

    lifecycle.OnStreamingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// A record START with the app-minted match id rebinds an interim
// (stream-minted) proxy: the match id is the download-grouping contract.
TEST(ProxyLifecycleTest, RecordStartRebindsInterimProxyToMatchGroup) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnStreamingStart();
    ASSERT_EQ(sink.last_group, "stream-42");

    lifecycle.OnRecordingStart("match-7", "/videos/match-7");
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 2);  // stopped + restarted under the match id
    EXPECT_EQ(sink.stops, 1);
    EXPECT_EQ(sink.last_group, "match-7");
    EXPECT_EQ(sink.last_dir, std::filesystem::path{"/videos/match-7"});

    // Both legs still hold; releasing them one by one stops on last-out only.
    lifecycle.OnStreamingStop();
    EXPECT_TRUE(sink.IsCapturing());
    lifecycle.OnRecordingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// A record with NO group id starts nothing (pre-coupling contract) but still
// HOLDS a stream-started proxy: proxy runs while recording OR streaming.
TEST(ProxyLifecycleTest, GrouplessRecordHoldsButNeverStartsProxy) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnRecordingStart("", "/videos/m");
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 0);

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());

    lifecycle.OnStreamingStop();
    EXPECT_TRUE(sink.IsCapturing());  // the groupless record still holds

    lifecycle.OnRecordingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// ForceStop (session end without commanded stops) resets BOTH holds atomically
// with the sink stop — the next session's first start must hit 0->1 again.
TEST(ProxyLifecycleTest, ForceStopResetsHoldsSoNextStartRetriggers) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnRecordingStart("match-1", "/videos/match-1");
    lifecycle.OnStreamingStart();
    ASSERT_TRUE(sink.IsCapturing());

    lifecycle.ForceStop();
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_FALSE(lifecycle.IsProxyRunning());

    // Stale holds gone: a fresh record start re-triggers the proxy.
    lifecycle.OnRecordingStart("match-2", "/videos/match-2");
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.last_group, "match-2");
}

// Every leg is idempotent: doubled starts/stops never double-start the sink or
// double-release the hold.
TEST(ProxyLifecycleTest, DoubledLegCallsAreIdempotent) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnStreamingStart();
    lifecycle.OnStreamingStart();
    EXPECT_EQ(sink.starts, 1);
    EXPECT_EQ(sink.double_start_violations, 0);

    lifecycle.OnStreamingStop();
    lifecycle.OnStreamingStop();
    EXPECT_EQ(sink.stops, 1);
    EXPECT_FALSE(sink.IsCapturing());
}

// A failed sink Start (best-effort training footage) neither wedges the
// lifecycle nor blocks a later successful start.
TEST(ProxyLifecycleTest, StartFailureDoesNotWedge) {
    FakeSink sink;
    sink.start_ok = false;
    ProxyLifecycle lifecycle(sink, FixedClock);

    lifecycle.OnRecordingStart("match-1", "/videos/match-1");
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_FALSE(lifecycle.IsProxyRunning());

    lifecycle.OnRecordingStop();  // releases the hold cleanly
    sink.start_ok = true;
    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());
}

// Rapid start/stop interleavings from concurrent record + stream legs must
// never double-start the sink (Start while capturing) nor orphan it (still
// capturing after both legs released + ForceStop).
TEST(ProxyLifecycleTest, RapidInterleavingsNeverDoubleStartOrOrphan) {
    FakeSink sink;
    ProxyLifecycle lifecycle(sink, FixedClock);

    constexpr int kIterations = 500;
    std::thread record_leg([&lifecycle] {
        for (int i = 0; i < kIterations; ++i) {
            lifecycle.OnRecordingStart("match-" + std::to_string(i), "/videos/m");
            lifecycle.OnRecordingStop();
        }
    });
    std::thread stream_leg([&lifecycle] {
        for (int i = 0; i < kIterations; ++i) {
            lifecycle.OnStreamingStart();
            lifecycle.OnStreamingStop();
        }
    });
    record_leg.join();
    stream_leg.join();

    EXPECT_EQ(sink.double_start_violations, 0);
    lifecycle.ForceStop();
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_FALSE(lifecycle.IsProxyRunning());
    // Balanced ledger: everything started was stopped (ForceStop adds at most
    // one trailing idempotent Stop).
    EXPECT_LE(sink.starts.load(), sink.stops.load());
}

}  // namespace
