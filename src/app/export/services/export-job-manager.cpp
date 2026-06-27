#include "app/export/services/export-job-manager.hpp"

#include <spdlog/spdlog.h>

#include <system_error>
#include <utility>

namespace sst::exportjob {

namespace {
namespace fs = std::filesystem;
constexpr const char* kL2Ext = ".mp4";
}  // namespace

ExportJobManager::ExportJobManager(sst::overlay::IOverlayBurner& burner, PathResolver resolve,
                                   TimelineLoader load_timeline, TokenMinter mint,
                                   fs::path export_dir)
    : burner_(burner),
      resolve_(std::move(resolve)),
      load_timeline_(std::move(load_timeline)),
      mint_(std::move(mint)),
      export_dir_(std::move(export_dir)) {}

ExportJobManager::~ExportJobManager() {
    const std::lock_guard lock(jobs_mtx_);
    for (const auto& job : jobs_) {
        if (job->worker.joinable()) {
            job->worker.join();
        }
    }
}

auto ExportJobManager::RequestExport(const std::string& recording_id) -> std::string {
    auto job = std::make_shared<Job>();
    job->id = "export-" + std::to_string(next_id_.fetch_add(1) + 1);
    job->recording_id = recording_id;
    {
        const std::lock_guard lock(jobs_mtx_);
        jobs_.push_back(job);
    }
    // The shared_ptr lives in jobs_ until the dtor joins, so the raw pointer the
    // worker holds stays valid for the worker's whole lifetime.
    job->worker = std::thread([this, raw = job.get()] { Run(*raw); });
    spdlog::info("ExportJob {}: queued for recording {}", job->id, recording_id);
    return job->id;
}

auto ExportJobManager::Poll(const std::string& job_id) const -> std::optional<ExportJobView> {
    const std::lock_guard lock(jobs_mtx_);
    for (const auto& job : jobs_) {
        if (job->id != job_id) {
            continue;
        }
        ExportJobView view;
        view.state = job->state.load();
        const std::lock_guard job_lock(job->mtx);
        view.token = job->token;
        view.error = job->error;
        return view;
    }
    return std::nullopt;
}

void ExportJobManager::Run(Job& job) {
    job.state.store(ExportState::kRunning);

    const auto fail = [&job](const std::string& message) {
        const std::lock_guard lock(job.mtx);
        job.error = message;
        job.state.store(ExportState::kFailed);
        spdlog::warn("ExportJob {}: failed: {}", job.id, message);
    };

    const auto l1_path = resolve_(job.recording_id);
    if (!l1_path) {
        fail("recording not found");
        return;
    }
    const auto timeline = load_timeline_(*l1_path);
    const fs::path l2_path = export_dir_ / (job.id + kL2Ext);
    std::error_code dir_ec;
    fs::create_directories(export_dir_, dir_ec);

    if (!burner_.Burn(*l1_path, timeline, l2_path)) {
        fail("overlay burn failed");
        return;
    }
    auto token = mint_(l2_path, job.id);
    if (!token) {
        std::error_code remove_ec;
        fs::remove(l2_path, remove_ec);
        fail("could not mint download token");
        return;
    }
    {
        const std::lock_guard lock(job.mtx);
        job.token = std::move(token);
    }
    job.state.store(ExportState::kReady);
    spdlog::info("ExportJob {}: ready ({})", job.id, l2_path.string());
}

}  // namespace sst::exportjob
