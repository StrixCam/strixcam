// Unit tests for the export-burn handler (#6 F6c): the LIVE_SESSION_ACTIVE gate
// and the command/response mapping over a job manager with a fake burner.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "app/control/services/handlers/export-burn.handler.hpp"
#include "app/export/services/export-job-manager.hpp"
#include "app/overlay/ports/overlay-burner.hpp"
#include "bluetooth.pb.h"
#include "domain/network/models/download-token.hpp"
#include "domain/overlay/models/overlay-timeline.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kMaxPolls = 200;
constexpr int kPollSleepMs = 5;
constexpr std::uint32_t kDownloadPort = 8080;

class FakeBurner final : public sst::overlay::IOverlayBurner {
   public:
    auto Burn(const fs::path& /*l1*/, const sst::overlay::OverlayTimeline& /*timeline*/,
              const fs::path& /*l2*/) -> bool override {
        return true;
    }
};

auto MakeManager(FakeBurner& burner) -> sst::exportjob::ExportJobManager {
    return sst::exportjob::ExportJobManager(
        burner, [](const std::string&) { return std::optional<fs::path>{"/v/match.mp4"}; },
        [](const fs::path&) { return sst::overlay::OverlayTimeline{}; },
        [](const fs::path&, const std::string& download_id) {
            sst::network::DownloadToken token;
            token.recording_id = download_id;
            token.token = "tok";
            token.expires_at_unix = 1;
            return std::optional{token};
        },
        fs::temp_directory_path() / "sst-export-handler-test");
}

auto ExportCmd(const std::string& recording_id) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.mutable_export_overlayed()->set_recording_id(recording_id);
    return cmd;
}

auto PollCmd(const std::string& job_id) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.mutable_poll_export()->set_job_id(job_id);
    return cmd;
}

}  // namespace

TEST(ExportBurnHandlerTest, RefusesExportWhileLive) {
    FakeBurner burner;
    auto manager = MakeManager(burner);
    sst::control::ExportBurnHandler handler(
        manager, [] { return true; }, "192.168.49.1", kDownloadPort);

    const auto resp = handler.Handle(ExportCmd("match"));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::LIVE_SESSION_ACTIVE);
    EXPECT_FALSE(resp.has_export_job());
}

TEST(ExportBurnHandlerTest, QueuesExportWhenIdle) {
    FakeBurner burner;
    auto manager = MakeManager(burner);
    sst::control::ExportBurnHandler handler(
        manager, [] { return false; }, "192.168.49.1", kDownloadPort);

    const auto resp = handler.Handle(ExportCmd("match"));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_TRUE(resp.has_export_job());
    EXPECT_FALSE(resp.export_job().job_id().empty());
    EXPECT_EQ(resp.export_job().state(), sst_cam::EXPORT_JOB_PENDING);
}

TEST(ExportBurnHandlerTest, PollUnknownJobReportsUnknown) {
    FakeBurner burner;
    auto manager = MakeManager(burner);
    sst::control::ExportBurnHandler handler(
        manager, [] { return false; }, "192.168.49.1", kDownloadPort);

    const auto resp = handler.Handle(PollCmd("export-404"));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.export_job().state(), sst_cam::EXPORT_JOB_UNKNOWN);
}

TEST(ExportBurnHandlerTest, PollReadyCarriesDownloadToken) {
    FakeBurner burner;
    auto manager = MakeManager(burner);
    sst::control::ExportBurnHandler handler(
        manager, [] { return false; }, "192.168.49.1", kDownloadPort);

    const auto queued = handler.Handle(ExportCmd("match"));
    const std::string job_id = queued.export_job().job_id();

    for (int i = 0; i < kMaxPolls; ++i) {
        const auto resp = handler.Handle(PollCmd(job_id));
        if (resp.export_job().state() == sst_cam::EXPORT_JOB_READY) {
            EXPECT_TRUE(resp.export_job().has_token());
            EXPECT_EQ(resp.export_job().token().auth_token(), "tok");
            EXPECT_FALSE(resp.export_job().token().http_url().empty());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollSleepMs));
    }
    FAIL() << "export job never reached READY";
}
