#include "app/export/services/export-job-manager.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

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
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard lock(jobs_mtx_);
        for (const auto& job : jobs_) {
            job->cancel.store(true);  // ask any in-flight burn to bail out early
        }
        jobs = std::move(jobs_);
        jobs_.clear();
    }
    // Join OUTSIDE the lock so a concurrent Poll() isn't blocked behind an
    // encode winding down.
    for (const auto& job : jobs) {
        if (job->worker.joinable()) {
            job->worker.join();
        }
    }
}

auto ExportJobManager::RequestExport(const std::string& recording_id,
                                     LiveGate is_live) -> std::string {
    auto job = std::make_shared<Job>();
    job->id = "export-" + std::to_string(next_id_.fetch_add(1) + 1);
    job->recording_id = recording_id;
    job->is_live = std::move(is_live);
    {
        const std::lock_guard lock(jobs_mtx_);
        // Dedup: a non-failed burn for the same recording is already in flight or
        // ready — reuse it rather than spawn a second identical transcode.
        for (const auto& existing : jobs_) {
            if (existing->recording_id == recording_id &&
                existing->state.load() != ExportState::kFailed) {
                spdlog::info("ExportJob {}: reused for recording {}", existing->id, recording_id);
                return existing->id;
            }
        }
        EvictFinishedLocked();
        jobs_.push_back(job);
    }
    // The shared_ptr lives in jobs_ until the dtor joins, so the raw pointer the
    // worker holds stays valid for the worker's whole lifetime.
    job->worker = std::thread([this, raw = job.get()] { Run(*raw); });
    spdlog::info("ExportJob {}: queued for recording {}", job->id, recording_id);
    return job->id;
}

void ExportJobManager::EvictFinishedLocked() {
    while (jobs_.size() >= kMaxJobs) {
        const auto victim = std::find_if(jobs_.begin(), jobs_.end(), [](const auto& job) {
            const auto state = job->state.load();
            return state == ExportState::kReady || state == ExportState::kFailed;
        });
        if (victim == jobs_.end()) {
            break;  // everything still in flight — nothing safe to evict
        }
        if ((*victim)->worker.joinable()) {
            (*victim)->worker.join();  // finished job: join returns promptly
        }
        std::error_code remove_ec;
        fs::remove(export_dir_ / ((*victim)->id + kL2Ext), remove_ec);
        spdlog::info("ExportJob {}: evicted (job cap {} reached)", (*victim)->id, kMaxJobs);
        jobs_.erase(victim);
    }
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

    if (job.cancel.load()) {
        fail("export cancelled");
        return;
    }
    // Re-check the live gate on the worker: the request-time check in the
    // handler is a point-in-time read, and a match may have started recording in
    // the window before this burn opens the L1. Never contend with a live encode.
    if (job.is_live && job.is_live()) {
        fail("a match went live before the burn started");
        return;
    }

    const auto l1_path = resolve_(job.recording_id);
    if (!l1_path) {
        fail("recording not found");
        return;
    }
    const auto timeline = load_timeline_(*l1_path);
    const fs::path l2_path = export_dir_ / (job.id + kL2Ext);
    std::error_code dir_ec;
    fs::create_directories(export_dir_, dir_ec);
    if (dir_ec) {
        fail("could not create export dir: " + dir_ec.message());
        return;
    }

    if (!burner_.Burn(*l1_path, timeline, l2_path, job.cancel)) {
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
