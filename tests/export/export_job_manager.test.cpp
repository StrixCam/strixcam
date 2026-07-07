// Unit tests for the overlay-burn job manager (#6 F6c). The burner + path/token
// collaborators are fakes (the fake burner writes a stub L2 file so the
// persistence contract is observable) — the real encode is a hardware-bound
// concern validated on-device. The L2 contract under test: it lands BESIDE the
// L1 in the same match folder as <l1_stem>-overlay.mp4, PERSISTS (no
// delete-after-download), and a re-export returns the existing file without
// re-burning.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include "app/export/services/export-job-manager.hpp"
#include "app/overlay/ports/overlay-burner.hpp"
#include "domain/network/models/download-token.hpp"
#include "domain/overlay/models/overlay-timeline.hpp"
#include "domain/storage/services/overlay-export-naming.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kMaxPolls = 200;
constexpr int kPollSleepMs = 5;

// Configurable fake burn — records the call, writes a stub L2 (like the real
// burner producing an MP4), and returns a preset result.
class FakeBurner final : public sst::overlay::IOverlayBurner {
   public:
    auto Burn(const fs::path& l1_path, const sst::overlay::OverlayTimeline& /*timeline*/,
              const fs::path& l2_path, const std::atomic<bool>& /*cancel*/) -> bool override {
        ++calls;
        last_l1 = l1_path;
        last_l2 = l2_path;
        if (result) {
            std::ofstream out(l2_path, std::ios::binary);
            out << "l2-bytes";
        }
        return result;
    }
    std::atomic<int> calls{0};
    bool result{true};
    fs::path last_l1;
    fs::path last_l2;
};

auto MakeToken() -> sst::network::DownloadToken {
    sst::network::DownloadToken token;
    token.token = "tok-abc";
    token.expires_at_unix = 1;
    return token;
}

// Poll until the job leaves pending/running (the worker is async). Fakes are
// instant, so this resolves within a couple of iterations.
auto WaitForTerminal(const sst::exportjob::ExportJobManager& manager,
                     const std::string& job_id) -> sst::exportjob::ExportJobView {
    for (int i = 0; i < kMaxPolls; ++i) {
        const auto view = manager.Poll(job_id);
        if (view && (view->state == sst::exportjob::ExportState::kReady ||
                     view->state == sst::exportjob::ExportState::kFailed)) {
            return *view;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollSleepMs));
    }
    return sst::exportjob::ExportJobView{};
}

// A per-test match dir with a stub L1 recording inside, removed on destruction.
class MatchDir {
   public:
    explicit MatchDir(const std::string& tag) {
        dir_ = fs::temp_directory_path() /
               ("sst-export-" + tag + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
        l1_ = dir_ / "match.mp4";
        std::ofstream out(l1_, std::ios::binary);
        out << "l1-bytes";
    }
    ~MatchDir() {
        std::error_code err;
        fs::remove_all(dir_, err);
    }
    [[nodiscard]] auto dir() const -> const fs::path& { return dir_; }
    [[nodiscard]] auto l1() const -> const fs::path& { return l1_; }
    [[nodiscard]] auto l2() const -> fs::path {
        return dir_ / sst::storage::overlay_export_naming::FileName(l1_.stem().string());
    }

   private:
    fs::path dir_;
    fs::path l1_;
};

auto Resolver(const MatchDir& match) -> sst::exportjob::ExportJobManager::PathResolver {
    return [&match](const std::string&) { return std::optional<fs::path>{match.l1()}; };
}

auto EmptyTimeline() -> sst::exportjob::ExportJobManager::TimelineLoader {
    return [](const fs::path&) { return sst::overlay::OverlayTimeline{}; };
}

}  // namespace

// The L2 lands BESIDE its L1 (same match folder), named <l1_stem>-overlay.mp4,
// tokened under its own stem, and PERSISTS after the job is ready.
TEST(ExportJobManagerTest, ReadyBurnsL2BesideL1AndPersists) {
    MatchDir match("beside");
    FakeBurner burner;
    std::string minted_id;
    sst::exportjob::ExportJobManager manager(
        burner, Resolver(match), EmptyTimeline(),
        [&minted_id](const fs::path&, const std::string& file_id) {
            minted_id = file_id;
            return std::optional{MakeToken()};
        });

    const auto job_id = manager.RequestExport("match");
    const auto view = WaitForTerminal(manager, job_id);

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kReady);
    ASSERT_TRUE(view.token.has_value());
    if (view.token) {  // narrows for clang-tidy (it can't see through ASSERT_TRUE)
        EXPECT_EQ(view.token->token, "tok-abc");
    }
    EXPECT_EQ(burner.calls.load(), 1);
    EXPECT_EQ(burner.last_l2, match.l2());  // dirname(L1) + "-overlay" marker
    EXPECT_TRUE(fs::exists(match.l2()));    // persistent — not cleaned up
    EXPECT_EQ(minted_id, "match-overlay");  // token id = the L2 stem
}

// Re-exporting an already-burned recording returns the existing L2 without
// re-encoding (per the proto contract).
TEST(ExportJobManagerTest, ReExportReturnsExistingL2WithoutReburn) {
    MatchDir match("reexport");
    {
        std::ofstream out(match.l2(), std::ios::binary);  // already burned earlier
        out << "existing-l2";
    }
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, Resolver(match), EmptyTimeline(),
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; });

    const auto view = WaitForTerminal(manager, manager.RequestExport("match"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kReady);
    EXPECT_TRUE(view.token.has_value());
    EXPECT_EQ(burner.calls.load(), 0);  // no second transcode
    EXPECT_TRUE(fs::exists(match.l2()));
}

TEST(ExportJobManagerTest, FailsWhenRecordingNotFound) {
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, [](const std::string&) { return std::optional<fs::path>{}; },  // unresolved
        EmptyTimeline(),
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; });

    const auto view = WaitForTerminal(manager, manager.RequestExport("ghost"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kFailed);
    EXPECT_EQ(burner.calls.load(), 0);  // never reached the burn
    EXPECT_FALSE(view.error.empty());
}

TEST(ExportJobManagerTest, FailsWhenBurnFails) {
    MatchDir match("burnfail");
    FakeBurner burner;
    burner.result = false;
    sst::exportjob::ExportJobManager manager(
        burner, Resolver(match), EmptyTimeline(),
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; });

    const auto view = WaitForTerminal(manager, manager.RequestExport("match"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kFailed);
    EXPECT_EQ(burner.calls.load(), 1);
    EXPECT_FALSE(view.token.has_value());
}

// A token-mint failure fails the JOB but keeps the burned L2 on disk — it is a
// persistent recording now, and a retry returns the existing file.
TEST(ExportJobManagerTest, MintFailureKeepsBurnedL2) {
    MatchDir match("mintfail");
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, Resolver(match), EmptyTimeline(), [](const fs::path&, const std::string&) {
            return std::optional<sst::network::DownloadToken>{};
        });

    const auto view = WaitForTerminal(manager, manager.RequestExport("match"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kFailed);
    EXPECT_TRUE(fs::exists(match.l2()));  // the L2 survives the failed mint
}

TEST(ExportJobManagerTest, PollUnknownJobIsNullopt) {
    MatchDir match("poll");
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, Resolver(match), EmptyTimeline(),
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; });

    EXPECT_FALSE(manager.Poll("export-999").has_value());
}
