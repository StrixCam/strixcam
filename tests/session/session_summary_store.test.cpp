// JSON last-session summary store: roundtrip + corrupt/missing-file tolerance.
// Pure — writes under an isolated per-test temp dir.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "adapters/session/json/json-session-summary-store.hpp"
#include "domain/session/models/session-summary.hpp"

namespace fs = std::filesystem;

namespace {

using sst::adapters::session::JsonSessionSummaryStore;
using sst::session::LastSessionSummary;
using sst::session::SessionEndReason;

auto MakeTempDir() -> fs::path {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("sst_summary_" + std::to_string(stamp) + "_" +
                                                std::to_string(counter.fetch_add(1)));
    fs::create_directories(dir);
    return dir;
}

TEST(JsonSessionSummaryStoreTest, PersistThenLoadRoundtrips) {
    const auto dir = MakeTempDir();
    JsonSessionSummaryStore store((dir / "last-session.json").string());

    constexpr std::uint32_t kEndClock = 2700;
    LastSessionSummary summary;
    summary.match_uuid = "match-42";
    summary.end_reason = SessionEndReason::kAutoStop;
    summary.end_clock_seconds = kEndClock;
    summary.file_valid = true;

    ASSERT_TRUE(store.Persist(summary));

    // A FRESH store instance (as after a reboot) reads the same record back.
    JsonSessionSummaryStore rebooted((dir / "last-session.json").string());
    const auto loaded = rebooted.Load();
    ASSERT_TRUE(loaded.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(loaded->match_uuid, "match-42");
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(loaded->end_reason, SessionEndReason::kAutoStop);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(loaded->end_clock_seconds, kEndClock);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_TRUE(loaded->file_valid);

    fs::remove_all(dir);
}

TEST(JsonSessionSummaryStoreTest, PersistOverwritesPreviousSummary) {
    const auto dir = MakeTempDir();
    JsonSessionSummaryStore store((dir / "last-session.json").string());

    LastSessionSummary write_ahead;
    write_ahead.match_uuid = "match-a";
    write_ahead.end_reason = SessionEndReason::kReboot;
    write_ahead.file_valid = false;
    ASSERT_TRUE(store.Persist(write_ahead));

    LastSessionSummary final_summary;
    final_summary.match_uuid = "match-a";
    final_summary.end_reason = SessionEndReason::kAppStop;
    final_summary.file_valid = true;
    ASSERT_TRUE(store.Persist(final_summary));

    const auto loaded = store.Load();
    ASSERT_TRUE(loaded.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(loaded->end_reason, SessionEndReason::kAppStop);
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_TRUE(loaded->file_valid);

    fs::remove_all(dir);
}

// Crash-safety: Persist goes through tmp + fsync + atomic rename, so a partial
// write (simulated here as a garbage half-written temp file — what a crash
// mid-write leaves behind) never shadows or corrupts the committed summary,
// and a successful Persist leaves no temp file behind.
TEST(JsonSessionSummaryStoreTest, PartialTempWriteNeverCorruptsCommittedSummary) {
    const auto dir = MakeTempDir();
    const auto path = dir / "last-session.json";
    const auto tmp_path = path.string() + ".tmp";
    JsonSessionSummaryStore store(path.string());

    LastSessionSummary committed;
    committed.match_uuid = "match-committed";
    committed.end_reason = SessionEndReason::kAppStop;
    committed.file_valid = true;
    ASSERT_TRUE(store.Persist(committed));
    EXPECT_FALSE(fs::exists(tmp_path));  // rename consumed the temp

    // Simulate a crash mid-write of a LATER persist: garbage sits in the temp
    // file, the committed file untouched. Load (as after reboot) still returns
    // the committed record — the partial write is invisible until renamed.
    {
        std::ofstream out(tmp_path);
        out << R"({"match_uuid":"half-writ)";
    }
    JsonSessionSummaryStore rebooted(path.string());
    const auto loaded = rebooted.Load();
    ASSERT_TRUE(loaded.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(loaded->match_uuid, "match-committed");

    // The next Persist discards the stale temp and commits cleanly.
    committed.match_uuid = "match-next";
    ASSERT_TRUE(rebooted.Persist(committed));
    EXPECT_FALSE(fs::exists(tmp_path));
    const auto reloaded = rebooted.Load();
    ASSERT_TRUE(reloaded.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) // floor-ok: ASSERT_TRUE guards
    EXPECT_EQ(reloaded->match_uuid, "match-next");

    fs::remove_all(dir);
}

TEST(JsonSessionSummaryStoreTest, MissingFileLoadsNothing) {
    const auto dir = MakeTempDir();
    JsonSessionSummaryStore store((dir / "never-written.json").string());
    EXPECT_FALSE(store.Load().has_value());
    fs::remove_all(dir);
}

TEST(JsonSessionSummaryStoreTest, CorruptFileLoadsNothing) {
    const auto dir = MakeTempDir();
    const auto path = dir / "last-session.json";
    {
        std::ofstream out(path);
        out << "{not json at all";
    }
    JsonSessionSummaryStore store(path.string());
    EXPECT_FALSE(store.Load().has_value());
    fs::remove_all(dir);
}

TEST(JsonSessionSummaryStoreTest, UnknownReasonLoadsNothing) {
    const auto dir = MakeTempDir();
    const auto path = dir / "last-session.json";
    {
        std::ofstream out(path);
        out << R"({"match_uuid":"m","end_reason":"weird","end_clock_seconds":0,)"
            << R"("file_valid":false})";
    }
    JsonSessionSummaryStore store(path.string());
    EXPECT_FALSE(store.Load().has_value());
    fs::remove_all(dir);
}

}  // namespace
