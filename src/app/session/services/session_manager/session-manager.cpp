#include "app/session/services/session_manager/session-manager.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <system_error>

#include "domain/session/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::session {

namespace fs = std::filesystem;

SessionManager::SessionManager(ISessionCleanup& cleanup) : cleanup_(cleanup) {}

auto SessionManager::Phase() const -> SessionPhase {
    std::lock_guard lock(mtx_);
    return state_.phase;
}

auto SessionManager::Snapshot() const -> SessionState {
    std::lock_guard lock(mtx_);
    return state_;
}

auto SessionManager::OnConnect() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase != SessionPhase::kIdle) {
        spdlog::warn("SessionManager::OnConnect rejected: phase={}", state_.phase);
        return false;
    }
    state_.phase = SessionPhase::kConnected;
    spdlog::info("SessionManager: -> Connected");
    return true;
}

auto SessionManager::OnWifiReady() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase < SessionPhase::kConnected) {
        spdlog::warn("SessionManager::OnWifiReady rejected: phase={}", state_.phase);
        return false;
    }
    // Only advance from Connected; re-affirming when already further along is a
    // no-op success (the group is already up).
    if (state_.phase == SessionPhase::kConnected) {
        state_.phase = SessionPhase::kWifiReady;
        spdlog::info("SessionManager: -> WifiReady");
    }
    return true;
}

auto SessionManager::OnWifiStopped() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase == SessionPhase::kIdle) {
        return false;
    }
    // Tearing down the group invalidates everything built on top of it; drop
    // back to Connected and discard the session config.
    state_.phase = SessionPhase::kConnected;
    state_.config.reset();
    state_.match = LiveMatch{};
    spdlog::info("SessionManager: WiFi stopped -> Connected (session config cleared)");
    return true;
}

auto SessionManager::ApplySessionConfig(const SessionConfig& config) -> bool {
    std::lock_guard lock(mtx_);
    // Contract §11 ordering: StartWifiDirect -> PushSessionConfig -> ... . The
    // session config provisions the streaming sinks (RTMP / preview), which
    // require the WiFi-Direct group already up; config before the group exists
    // is out of order. Re-pushing config mid-recording is likewise rejected (the
    // documented flow configures before recording starts).
    if (state_.phase < SessionPhase::kWifiReady || state_.phase == SessionPhase::kRecording) {
        spdlog::warn(
            "SessionManager::ApplySessionConfig rejected (out of order: WiFi Direct group "
            "required first per contract §11): phase={}",
            state_.phase);
        return false;
    }
    if (!PrepareOutputDirs(config)) {
        return false;
    }
    state_.config = config;
    // First config advances WifiReady -> Configured; a re-push while further
    // along keeps the current phase.
    if (state_.phase == SessionPhase::kWifiReady) {
        state_.phase = SessionPhase::kConfigured;
    }
    spdlog::info("SessionManager: session config applied {} (phase={})", config, state_.phase);
    return true;
}

auto SessionManager::OnOverlayConfigured() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase < SessionPhase::kConfigured) {
        spdlog::warn(
            "SessionManager::OnOverlayConfigured rejected: phase={} (need a session "
            "config first)",
            state_.phase);
        return false;
    }
    if (state_.phase == SessionPhase::kConfigured) {
        state_.phase = SessionPhase::kReady;
        spdlog::info("SessionManager: -> Ready");
    }
    // Re-push of the layout while Ready/Recording is allowed and keeps phase.
    return true;
}

auto SessionManager::OnRecordingStart() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase != SessionPhase::kReady) {
        spdlog::warn("SessionManager::OnRecordingStart rejected: phase={}", state_.phase);
        return false;
    }
    state_.phase = SessionPhase::kRecording;
    spdlog::info("SessionManager: -> Recording");
    return true;
}

auto SessionManager::OnRecordingStop() -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase != SessionPhase::kRecording) {
        spdlog::warn("SessionManager::OnRecordingStop rejected: phase={}", state_.phase);
        return false;
    }
    state_.phase = SessionPhase::kReady;
    spdlog::info("SessionManager: recording stopped -> Ready");
    return true;
}

auto SessionManager::OnDisconnect() -> void {
    {
        std::lock_guard lock(mtx_);
        if (state_.phase == SessionPhase::kIdle) {
            return;
        }
        spdlog::info("SessionManager: disconnect from phase={} — cleaning up", state_.phase);
    }

    // Fan out cleanup OUTSIDE the lock — each is idempotent and a no-op when its
    // subsystem is inactive; order is irrelevant (R15). Each step is isolated so
    // a throw in one still runs the rest and the Idle reset below (R11).
    const auto safe = [](const char* step, auto&& action) {
        try {
            action();
        } catch (const std::exception& e) {
            spdlog::error("SessionManager: cleanup step {} threw: {}", step, e.what());
        } catch (...) {
            spdlog::error("SessionManager: cleanup step {} threw (non-std)", step);
        }
    };
    safe("FinalizeRecording", [this] { cleanup_.FinalizeRecording(); });
    safe("StopStreaming", [this] { cleanup_.StopStreaming(); });
    safe("TeardownWifiDirect", [this] { cleanup_.TeardownWifiDirect(); });

    std::lock_guard lock(mtx_);
    state_ = SessionState{};  // back to Idle, all session memory cleared (R11)
    spdlog::info("SessionManager: session cleared -> Idle");
}

auto SessionManager::ApplyMatchUpdate(const std::function<void(LiveMatch&)>& mutate) -> bool {
    std::lock_guard lock(mtx_);
    if (state_.phase < SessionPhase::kConfigured || !mutate) {
        return false;
    }
    mutate(state_.match);
    return true;
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
