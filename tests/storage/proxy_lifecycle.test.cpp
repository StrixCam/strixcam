// Record-or-stream ref-count for the internal dual-camera proxy: the proxy runs
// while a recording OR a platform stream is active and stops on last-out, named
// by the session's match_uuid read through the session-info provider. Pure —
// fake IProxySink counting transitions and flagging any double-start (Start
// while already capturing), which the lifecycle's single mutex must make
// impossible.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/storage/ports/proxy-sink.hpp"
#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"

namespace {

using sst::storage::ProxyLifecycle;
using sst::storage::ProxySessionInfo;

class FakeSink final : public sst::storage::IProxySink {
   public:
    auto Start(const std::string& match_uuid,
               const std::filesystem::path& output_dir) -> bool override {
        if (capturing_.load()) {
            ++double_start_violations;  // lifecycle must never do this
            return false;
        }
        ++starts;
        last_match = match_uuid;
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
    std::string last_match;
    std::filesystem::path last_dir;

   private:
    std::atomic<bool> capturing_{false};
};

// A settable session-info source standing in for the SessionManager config
// (match_uuid + per-match output dir). nullopt = no session config pushed.
struct FakeSession {
    std::optional<ProxySessionInfo> info;

    auto Provider() -> ProxyLifecycle::SessionInfoProvider {
        return [this] { return info; };
    }
};

// Deterministic epoch-ms clock so the interim stream-minted id is assertable
// ("stream-42").
constexpr std::uint64_t kFixedEpochMs = 42;

auto FixedClock() -> std::uint64_t { return kFixedEpochMs; }

auto MatchInfo(const std::string& match) -> ProxySessionInfo {
    return {.match_uuid = match, .output_dir = "/videos/user/" + match};
}

// The truth table: record start -> up (named by the session's match_uuid, in
// the per-match dir); stream start -> still up (no second start); record stop
// -> still up (stream holds); stream stop -> down.
TEST(ProxyLifecycleTest, RecordOrStreamTruthTable) {
    FakeSink sink;
    FakeSession session{.info = MatchInfo("match-1")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnRecordingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 1);
    EXPECT_EQ(sink.last_match, "match-1");
    EXPECT_EQ(sink.last_dir, std::filesystem::path{"/videos/user/match-1"});

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

// Stream-only start WITH a pushed session config pairs the proxy with the
// match, exactly like the record leg — streaming-only matches still land their
// development footage in the match folder.
TEST(ProxyLifecycleTest, StreamOnlyWithSessionUsesMatchIdentity) {
    FakeSink sink;
    FakeSession session{.info = MatchInfo("match-3")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.last_match, "match-3");
    EXPECT_EQ(sink.last_dir, std::filesystem::path{"/videos/user/match-3"});

    lifecycle.OnStreamingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// Stream-only with NO session config: the lifecycle mints an interim
// `stream-<epoch-ms>` id at the sink root (no match id, no per-match dir).
TEST(ProxyLifecycleTest, StreamOnlyWithoutSessionMintsInterimIdAtSinkRoot) {
    FakeSink sink;
    FakeSession session;  // no config pushed
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.last_match, "stream-42");
    EXPECT_TRUE(sink.last_dir.empty());  // sink construction root

    lifecycle.OnStreamingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// A record start rebinds an interim (stream-minted) proxy to the session's
// match_uuid: the match id is the on-device pairing contract.
TEST(ProxyLifecycleTest, RecordStartRebindsInterimProxyToMatch) {
    FakeSink sink;
    FakeSession session;  // stream starts before any config exists
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnStreamingStart();
    ASSERT_EQ(sink.last_match, "stream-42");

    session.info = MatchInfo("match-7");  // config pushed, then record starts
    lifecycle.OnRecordingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 2);  // stopped + restarted under the match id
    EXPECT_EQ(sink.stops, 1);
    EXPECT_EQ(sink.last_match, "match-7");
    EXPECT_EQ(sink.last_dir, std::filesystem::path{"/videos/user/match-7"});

    // Both legs still hold; releasing them one by one stops on last-out only.
    lifecycle.OnStreamingStop();
    EXPECT_TRUE(sink.IsCapturing());
    lifecycle.OnRecordingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// A record start already running under the SAME match does not restart the
// proxy (idempotent re-entry, e.g. a same-match config re-push).
TEST(ProxyLifecycleTest, RecordStartUnderSameMatchDoesNotRestart) {
    FakeSink sink;
    FakeSession session{.info = MatchInfo("match-1")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnStreamingStart();  // proxy up under match-1 already
    lifecycle.OnRecordingStart();
    EXPECT_EQ(sink.starts, 1);  // no rebind — same identity
    EXPECT_EQ(sink.stops, 0);
}

// A record start with NO session info starts nothing (there is no match to
// pair with — defensive) but still HOLDS a stream-started proxy: the proxy
// runs while recording OR streaming.
TEST(ProxyLifecycleTest, SessionlessRecordHoldsButNeverStartsProxy) {
    FakeSink sink;
    FakeSession session;
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnRecordingStart();
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_EQ(sink.starts, 0);

    lifecycle.OnStreamingStart();
    EXPECT_TRUE(sink.IsCapturing());

    lifecycle.OnStreamingStop();
    EXPECT_TRUE(sink.IsCapturing());  // the sessionless record still holds

    lifecycle.OnRecordingStop();
    EXPECT_FALSE(sink.IsCapturing());
}

// ForceStop (session end without commanded stops) resets BOTH holds atomically
// with the sink stop — the next session's first start must hit 0->1 again.
TEST(ProxyLifecycleTest, ForceStopResetsHoldsSoNextStartRetriggers) {
    FakeSink sink;
    FakeSession session{.info = MatchInfo("match-1")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnRecordingStart();
    lifecycle.OnStreamingStart();
    ASSERT_TRUE(sink.IsCapturing());

    lifecycle.ForceStop();
    EXPECT_FALSE(sink.IsCapturing());
    EXPECT_FALSE(lifecycle.IsProxyRunning());

    // Stale holds gone: a fresh record start re-triggers the proxy.
    session.info = MatchInfo("match-2");
    lifecycle.OnRecordingStart();
    EXPECT_TRUE(sink.IsCapturing());
    EXPECT_EQ(sink.last_match, "match-2");
}

// Every leg is idempotent: doubled starts/stops never double-start the sink or
// double-release the hold.
TEST(ProxyLifecycleTest, DoubledLegCallsAreIdempotent) {
    FakeSink sink;
    FakeSession session;
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnStreamingStart();
    lifecycle.OnStreamingStart();
    EXPECT_EQ(sink.starts, 1);
    EXPECT_EQ(sink.double_start_violations, 0);

    lifecycle.OnStreamingStop();
    lifecycle.OnStreamingStop();
    EXPECT_EQ(sink.stops, 1);
    EXPECT_FALSE(sink.IsCapturing());
}

// A failed sink Start (best-effort development footage) neither wedges the
// lifecycle nor blocks a later successful start.
TEST(ProxyLifecycleTest, StartFailureDoesNotWedge) {
    FakeSink sink;
    sink.start_ok = false;
    FakeSession session{.info = MatchInfo("match-1")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    lifecycle.OnRecordingStart();
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
    FakeSession session{.info = MatchInfo("match-1")};
    ProxyLifecycle lifecycle(sink, session.Provider(), FixedClock);

    constexpr int kIterations = 500;
    std::thread record_leg([&lifecycle] {
        for (int i = 0; i < kIterations; ++i) {
            lifecycle.OnRecordingStart();
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
