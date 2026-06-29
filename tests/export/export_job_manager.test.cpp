// Unit tests for the overlay-burn job manager (#6 F6c). The burner + path/token
// collaborators are fakes, so no real video files are decoded here — the burn
// itself is a hardware-bound test validated on-device.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "app/export/services/export-job-manager.hpp"
#include "app/overlay/ports/overlay-burner.hpp"
#include "domain/network/models/download-token.hpp"
#include "domain/overlay/models/overlay-timeline.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kMaxPolls = 200;
constexpr int kPollSleepMs = 5;

// Configurable fake burn — records the call and returns a preset result.
class FakeBurner final : public sst::overlay::IOverlayBurner {
   public:
    auto Burn(const fs::path& l1_path, const sst::overlay::OverlayTimeline& /*timeline*/,
              const fs::path& l2_path, const std::atomic<bool>& /*cancel*/) -> bool override {
        ++calls;
        last_l1 = l1_path;
        last_l2 = l2_path;
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

auto TempExportDir() -> fs::path { return fs::temp_directory_path() / "sst-export-test"; }

}  // namespace

TEST(ExportJobManagerTest, ReadyWithTokenOnSuccess) {
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, [](const std::string&) { return std::optional<fs::path>{"/v/match.mp4"}; },
        [](const fs::path&) { return sst::overlay::OverlayTimeline{}; },
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; },
        TempExportDir());

    const auto job_id = manager.RequestExport("match");
    const auto view = WaitForTerminal(manager, job_id);

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kReady);
    ASSERT_TRUE(view.token.has_value());
    if (view.token) {  // narrows for clang-tidy (it can't see through ASSERT_TRUE)
        EXPECT_EQ(view.token->token, "tok-abc");
    }
    EXPECT_EQ(burner.calls.load(), 1);
}

TEST(ExportJobManagerTest, FailsWhenRecordingNotFound) {
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, [](const std::string&) { return std::optional<fs::path>{}; },  // unresolved
        [](const fs::path&) { return sst::overlay::OverlayTimeline{}; },
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; },
        TempExportDir());

    const auto view = WaitForTerminal(manager, manager.RequestExport("ghost"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kFailed);
    EXPECT_EQ(burner.calls.load(), 0);  // never reached the burn
    EXPECT_FALSE(view.error.empty());
}

TEST(ExportJobManagerTest, FailsWhenBurnFails) {
    FakeBurner burner;
    burner.result = false;
    sst::exportjob::ExportJobManager manager(
        burner, [](const std::string&) { return std::optional<fs::path>{"/v/match.mp4"}; },
        [](const fs::path&) { return sst::overlay::OverlayTimeline{}; },
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; },
        TempExportDir());

    const auto view = WaitForTerminal(manager, manager.RequestExport("match"));

    EXPECT_EQ(view.state, sst::exportjob::ExportState::kFailed);
    EXPECT_EQ(burner.calls.load(), 1);
    EXPECT_FALSE(view.token.has_value());
}

TEST(ExportJobManagerTest, PollUnknownJobIsNullopt) {
    FakeBurner burner;
    sst::exportjob::ExportJobManager manager(
        burner, [](const std::string&) { return std::optional<fs::path>{"/v/match.mp4"}; },
        [](const fs::path&) { return sst::overlay::OverlayTimeline{}; },
        [](const fs::path&, const std::string&) { return std::optional{MakeToken()}; },
        TempExportDir());

    EXPECT_FALSE(manager.Poll("export-999").has_value());
}
