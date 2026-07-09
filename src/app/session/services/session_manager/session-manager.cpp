#include "app/session/services/session_manager/session-manager.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "domain/session/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::session {

namespace fs = std::filesystem;

namespace {

// Isolate one cleanup step so a throw in it still runs the rest of the fan-out
// and the Idle reset.
auto SafeStep(const char* step, const std::function<void()>& action) -> void {
    try {
        action();
    } catch (const std::exception& e) {
        spdlog::error("SessionManager: cleanup step {} threw: {}", step, e.what());
    } catch (...) {
        spdlog::error("SessionManager: cleanup step {} threw (non-std)", step);
    }
}

}  // namespace

SessionManager::SessionManager(ISessionCleanup& cleanup, ISessionSummaryStore* store,
                               SessionTiming timing)
    : cleanup_(cleanup), store_(store), timing_(timing) {
    if (store_ != nullptr) {
        // Boot reconciliation: the stored summary is either the last clean
        // session end or the write-ahead reboot record of a session the crash
        // orphaned — either way it is the truth about the previous session.
        state_.last_summary = store_->Load();
        if (state_.last_summary) {
            spdlog::info("SessionManager: last-session summary loaded: {}", *state_.last_summary);
        }
    }
    timer_thread_ = std::thread([this] { TimerLoop(); });
}

SessionManager::~SessionManager() {
    {
        const std::lock_guard lock(mtx_);
        stop_thread_ = true;
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    // Shutdown safety net: a still-active session (SIGTERM mid-match) is
    // finalized cleanly so the MP4 gets its moov atom. main() calls
    // FinalizeSession explicitly before stopping the pipeline; this is the
    // last-resort path and a no-op after it.
    FinalizeSession(SessionEndReason::kAppStop);
}

auto SessionManager::Phase() const -> SessionPhase {
    const std::lock_guard lock(mtx_);
    return state_.phase;
}

auto SessionManager::Snapshot() const -> SessionState {
    const std::lock_guard lock(mtx_);
    SessionState copy = state_;
    auto elapsed = recording_accumulated_;
    if (recording_started_at_) {
        elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() -
                                                                         *recording_started_at_);
    }
    copy.recording_elapsed_seconds = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    return copy;
}

auto SessionManager::OnConnect() -> bool {
    const std::lock_guard lock(mtx_);
    state_.app_connected = true;
    // Cancel-vs-fire is decided under mtx_: whichever side takes the lock first
    // wins, so a connect at timeout−ε deterministically prevents the stop.
    CancelAutoStopLocked();
    spdlog::info("SessionManager: app connected (session={})", state_.phase);
    return true;
}

auto SessionManager::OnDisconnect() -> void {
    const std::lock_guard lock(mtx_);
    state_.app_connected = false;
    if (state_.phase == SessionPhase::kFinalizing) {
        // A finalize is already dismantling everything; nothing to guard.
        spdlog::info("SessionManager: app disconnected during finalize");
        return;
    }
    if (state_.phase != SessionPhase::kIdle) {
        // An active session (Configured/Ready/Recording) must survive the
        // disconnect; the auto-stop safety net bounds how long it runs unattended.
        ArmAutoStopLocked();
        spdlog::info("SessionManager: app disconnected — session={} kept, auto-stop armed",
                     state_.phase);
        return;
    }
    if (state_.wifi_group_up) {
        // Idle, but the WiFi-Direct group is still up (preview only). It serves
        // only the now-gone app AND starves BLE advertising on the shared 2.4GHz
        // combo radio (RTL8822CE coexistence), so the camera cannot be
        // rediscovered while it lingers. Arm a SHORT grace, not the 30-min
        // auto-stop: a quick reconnect (OnConnect cancels it) keeps the group so
        // preview resumes without a disruptive P2P reform; a real disconnect lets
        // it tear down within seconds and advertising recovers.
        ArmIdleGroupGraceLocked();
        spdlog::info(
            "SessionManager: app disconnected (idle, group up) — group-teardown grace armed ({}s)",
            std::chrono::duration_cast<std::chrono::seconds>(timing_.idle_group_grace).count());
        return;
    }
    spdlog::info("SessionManager: app disconnected (idle, no group)");
}

auto SessionManager::OnWifiReady() -> bool {
    const std::lock_guard lock(mtx_);
    if (state_.phase == SessionPhase::kFinalizing) {
        // The finalize fan-out is dismantling the group right now; accepting a
        // group-up here would be silently clobbered by the post-fan-out reset
        // (wifi_group_up = false) while the caller believes the group is live.
        spdlog::warn("SessionManager::OnWifiReady rejected: session finalizing");
        return false;
    }
    if (!state_.app_connected) {
        // A group with no app and no session would leak until the timer; the
        // command surface only reaches here over BLE anyway.
        spdlog::warn("SessionManager::OnWifiReady rejected: no app connected");
        return false;
    }
    if (!state_.wifi_group_up) {
        state_.wifi_group_up = true;
        spdlog::info("SessionManager: WiFi group up");
    }
    return true;
}

auto SessionManager::OnWifiStopped() -> bool {
    std::unique_lock lock(mtx_);
    if (state_.phase == SessionPhase::kFinalizing) {
        return false;  // already ending; finalize owns the teardown
    }
    if (state_.phase != SessionPhase::kIdle) {
        // The group going down ends the session built on it — finalize FIRST so
        // a recording in flight is EOSed, not dropped (the old data-loss path).
        spdlog::info("SessionManager: WiFi stopped with session={} — finalizing", state_.phase);
        return FinalizeLocked(lock, SessionEndReason::kAppStop);
    }
    if (!state_.wifi_group_up) {
        return false;
    }
    state_.wifi_group_up = false;
    // Nothing is left to guard: an armed idle-with-group timer is moot.
    CancelAutoStopLocked();
    spdlog::info("SessionManager: WiFi group down (no session)");
    return true;
}

auto SessionManager::ApplySessionConfig(const SessionConfig& config) -> bool {
    const std::lock_guard lock(mtx_);
    // Contract §11 ordering: StartWifiDirect -> PushSessionConfig -> ... . The
    // session config provisions the streaming sinks (RTMP / preview), which
    // require the WiFi-Direct group already up; config before the group exists
    // is out of order. Re-pushing config mid-recording (or while a finalize is
    // dismantling the session) is likewise rejected.
    if (!state_.wifi_group_up || state_.phase == SessionPhase::kRecording ||
        state_.phase == SessionPhase::kFinalizing) {
        spdlog::warn(
            "SessionManager::ApplySessionConfig rejected (WiFi Direct group required first per "
            "contract §11; not accepted while recording/finalizing): session={} group={}",
            state_.phase, state_.wifi_group_up ? "up" : "down");
        return false;
    }
    if (!PrepareOutputDirs(config)) {
        return false;
    }
    // A config for a DIFFERENT match resets the display-only match state
    // (scores / period / clock back to zero) so a new match never inherits the
    // previous match's scoreboard, and restarts the recording-elapsed tracking.
    // A same-match re-push (same match_uuid) keeps the live values the app
    // already pushed.
    const bool is_new_match =
        !state_.config.has_value() || state_.config->match_uuid != config.match_uuid;
    state_.config = config;
    if (is_new_match) {
        state_.match = LiveMatch{};
        recording_accumulated_ = std::chrono::milliseconds{0};
        recording_started_at_.reset();
        recording_file_valid_ = false;
    }
    // First config advances Idle -> Configured; a re-push while further along
    // keeps the current session state.
    if (state_.phase == SessionPhase::kIdle) {
        state_.phase = SessionPhase::kConfigured;
    }
    spdlog::info("SessionManager: session config applied {} (session={})", config, state_.phase);
    return true;
}

auto SessionManager::OnOverlayConfigured() -> bool {
    const std::lock_guard lock(mtx_);
    if (!HasActiveConfigLocked()) {
        spdlog::warn(
            "SessionManager::OnOverlayConfigured rejected: session={} (need a session "
            "config first)",
            state_.phase);
        return false;
    }
    if (state_.phase == SessionPhase::kConfigured) {
        state_.phase = SessionPhase::kReady;
        spdlog::info("SessionManager: -> Ready");
    }
    // Re-push of the layout while Ready/Recording is allowed and keeps state.
    return true;
}

auto SessionManager::OnRecordingStart() -> bool {
    const std::lock_guard lock(mtx_);
    if (state_.phase != SessionPhase::kReady) {
        spdlog::warn("SessionManager::OnRecordingStart rejected: session={}", state_.phase);
        return false;
    }
    state_.phase = SessionPhase::kRecording;
    recording_started_at_ = SteadyClock::now();
    spdlog::info("SessionManager: -> Recording");
    // Write-ahead orphan record: if the firmware dies before this recording
    // is finalized, the next boot's summary already names the match and
    // flags the file invalid (reason=Reboot). Persisted UNDER mtx_ — every
    // summary write is serialized by the lock, so a stale write-ahead can never
    // land after the finalize summary (the store is a small local file write
    // and takes no locks of its own — no ordering cycle).
    LastSessionSummary write_ahead;
    write_ahead.match_uuid = state_.config ? state_.config->match_uuid : "";
    write_ahead.end_reason = SessionEndReason::kReboot;
    write_ahead.end_clock_seconds = state_.match.clock_seconds;
    write_ahead.file_valid = false;
    PersistSummary(write_ahead);
    return true;
}

auto SessionManager::OnRecordingStop() -> bool {
    const std::lock_guard lock(mtx_);
    if (state_.phase != SessionPhase::kRecording) {
        spdlog::warn("SessionManager::OnRecordingStop rejected: session={}", state_.phase);
        return false;
    }
    state_.phase = SessionPhase::kReady;
    StopElapsedClockLocked();
    // The recorder was stopped by command (EOS + moov written) — a later
    // session end reports the file valid even though its own
    // FinalizeRecording finds nothing left to do.
    recording_file_valid_ = true;
    spdlog::info("SessionManager: recording stopped -> Ready");
    // Refresh the write-ahead record: a crash from here on still orphans
    // the SESSION (reason=Reboot) but the file itself is already playable.
    // Persisted UNDER mtx_ (see OnRecordingStart) so it can never overtake or
    // trail the finalize summary out of order.
    LastSessionSummary write_ahead;
    write_ahead.match_uuid = state_.config ? state_.config->match_uuid : "";
    write_ahead.end_reason = SessionEndReason::kReboot;
    write_ahead.end_clock_seconds = state_.match.clock_seconds;
    write_ahead.file_valid = true;
    PersistSummary(write_ahead);
    return true;
}

auto SessionManager::FinalizeSession(SessionEndReason reason) -> bool {
    std::unique_lock lock(mtx_);
    return FinalizeLocked(lock, reason);
}

auto SessionManager::ApplyMatchUpdate(const std::function<void(LiveMatch&)>& mutate) -> bool {
    const std::lock_guard lock(mtx_);
    if (!HasActiveConfigLocked() || !mutate) {
        return false;
    }
    mutate(state_.match);
    return true;
}

auto SessionManager::HasActiveConfigLocked() const -> bool {
    return state_.config.has_value() && state_.phase != SessionPhase::kIdle &&
           state_.phase != SessionPhase::kFinalizing;
}

auto SessionManager::ArmTimerLocked(std::chrono::milliseconds timeout) -> void {
    auto_stop_deadline_ = SteadyClock::now() + timeout;
    timer_cv_.notify_all();
}

auto SessionManager::ArmAutoStopLocked() -> void {
    const auto timeout = (state_.config && state_.config->auto_stop_minutes > 0)
                             ? state_.config->auto_stop_minutes * timing_.config_minute
                             : timing_.default_auto_stop;
    ArmTimerLocked(timeout);
}

auto SessionManager::ArmIdleGroupGraceLocked() -> void {
    ArmTimerLocked(timing_.idle_group_grace);
}

auto SessionManager::CancelAutoStopLocked() -> void {
    if (auto_stop_deadline_) {
        auto_stop_deadline_.reset();
        timer_cv_.notify_all();
        spdlog::info("SessionManager: auto-stop timer cancelled");
    }
}

auto SessionManager::TimerLoop() -> void {
    std::unique_lock lock(mtx_);
    while (!stop_thread_) {
        if (!auto_stop_deadline_) {
            timer_cv_.wait(lock,
                           [this] { return stop_thread_ || auto_stop_deadline_.has_value(); });
            continue;
        }
        const auto deadline = *auto_stop_deadline_;
        const bool interrupted = timer_cv_.wait_until(lock, deadline, [this, deadline] {
            return stop_thread_ || !auto_stop_deadline_ || *auto_stop_deadline_ != deadline;
        });
        if (interrupted) {
            continue;  // cancelled / re-armed / shutting down — re-evaluate
        }
        // Deadline elapsed with the arm still current: claim it. OnConnect
        // clears the deadline under this same mutex, so exactly one of
        // {cancel, fire} wins — never both.
        auto_stop_deadline_.reset();
        HandleAutoStopLocked(lock);
    }
}

auto SessionManager::HandleAutoStopLocked(std::unique_lock<std::mutex>& lock) -> void {
    if (state_.phase != SessionPhase::kIdle && state_.phase != SessionPhase::kFinalizing) {
        spdlog::warn("SessionManager: auto-stop fired — finalizing session={}", state_.phase);
        FinalizeLocked(lock, SessionEndReason::kAutoStop);
        return;
    }
    if (state_.phase == SessionPhase::kIdle && state_.wifi_group_up) {
        // Preview-only group with no app: tear the group down (frees BLE
        // advertising for rediscovery); no session ended so no summary is written.
        spdlog::info("SessionManager: idle group-teardown grace elapsed — tearing down WiFi group");
        lock.unlock();
        SafeStep("TeardownWifiDirect", [this] { cleanup_.TeardownWifiDirect(); });
        lock.lock();
        state_.wifi_group_up = false;
    }
}

// The single finalize gate. The CAS below is the only writer of kFinalizing;
// all four end triggers (app stop, auto-stop, camera failure, shutdown) funnel
// through here, and losers return before touching anything.
auto SessionManager::FinalizeLocked(std::unique_lock<std::mutex>& lock,
                                    SessionEndReason reason) -> bool {
    if (state_.phase == SessionPhase::kIdle || state_.phase == SessionPhase::kFinalizing) {
        return false;  // no session, or another trigger already claimed it
    }
    const bool was_recording = state_.phase == SessionPhase::kRecording;
    state_.phase = SessionPhase::kFinalizing;
    StopElapsedClockLocked();

    LastSessionSummary summary;
    summary.match_uuid = state_.config ? state_.config->match_uuid : "";
    summary.end_reason = reason;
    summary.end_clock_seconds = state_.match.clock_seconds;
    bool file_valid = recording_file_valid_;
    spdlog::info("SessionManager: finalizing (reason={}, was_recording={})", reason, was_recording);

    // Fan out cleanup OUTSIDE the lock — each step is idempotent and a no-op
    // when its subsystem is inactive; a throw in one still runs the rest.
    // Snapshot() during this window reads phase=Finalizing with the pre-finalize
    // config/match intact — never a torn intermediate.
    lock.unlock();
    bool finalize_ok = false;
    SafeStep("FinalizeRecording",
             [this, &finalize_ok] { finalize_ok = cleanup_.FinalizeRecording(); });
    SafeStep("StopStreaming", [this] { cleanup_.StopStreaming(); });
    SafeStep("ResetSelections", [this] { cleanup_.ResetSelections(); });
    SafeStep("TeardownWifiDirect", [this] { cleanup_.TeardownWifiDirect(); });
    if (was_recording) {
        file_valid = finalize_ok;
    }
    summary.file_valid = file_valid;

    lock.lock();
    // Persist AFTER re-acquiring mtx_: all summary writes go through the lock,
    // so a write-ahead from OnRecordingStart/Stop can never land after this
    // final record (those paths persist-under-lock and are rejected once the
    // phase is kFinalizing anyway).
    PersistSummary(summary);
    state_.phase = SessionPhase::kIdle;
    state_.config.reset();
    state_.match = LiveMatch{};
    state_.wifi_group_up = false;
    state_.last_summary = summary;
    recording_accumulated_ = std::chrono::milliseconds{0};
    recording_started_at_.reset();
    recording_file_valid_ = false;
    CancelAutoStopLocked();  // the end this timer guarded against has happened
    spdlog::info("SessionManager: session ended -> Idle ({})", summary);
    return true;
}

auto SessionManager::StopElapsedClockLocked() -> void {
    if (recording_started_at_) {
        recording_accumulated_ += std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now() - *recording_started_at_);
        recording_started_at_.reset();
    }
}

auto SessionManager::PersistSummary(const LastSessionSummary& summary) -> void {
    if (store_ == nullptr) {
        return;
    }
    SafeStep("PersistSummary", [this, &summary] {
        if (!store_->Persist(summary)) {
            spdlog::warn("SessionManager: summary persist failed (in-memory copy still valid)");
        }
    });
}

auto SessionManager::PrepareOutputDirs(const SessionConfig& config) -> bool {
    const auto make_parent = [](const std::string& file_path) -> bool {
        if (file_path.empty()) {
            return true;
        }
        const fs::path parent = fs::path{file_path}.parent_path();
        if (parent.empty()) {
            return true;
        }
        std::error_code error;
        fs::create_directories(parent, error);
        if (error) {
            spdlog::error("SessionManager: mkdir({}) failed: {}", parent.string(), error.message());
            return false;
        }
        return true;
    };

    return make_parent(config.video_output_path) && make_parent(config.thumbnail_output_path);
}

}  // namespace sst::session
