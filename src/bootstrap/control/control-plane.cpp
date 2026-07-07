#include "bootstrap/control/control-plane.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "adapters/control/network/subprocess.hpp"
#include "adapters/control/system/realtime-clock.hpp"
#include "app/control/services/handlers/active-camera.handler.hpp"
#include "app/control/services/handlers/auto-white-balance.handler.hpp"
#include "app/control/services/handlers/camera-calibration.handler.hpp"
#include "app/control/services/handlers/camera-focus.handler.hpp"
#include "app/control/services/handlers/device.handler.hpp"
#include "app/control/services/handlers/download.handler.hpp"
#include "app/control/services/handlers/export-burn.handler.hpp"
#include "app/control/services/handlers/health-gate.hpp"
#include "app/control/services/handlers/match-state.handler.hpp"
#include "app/control/services/handlers/network.handler.hpp"
#include "app/control/services/handlers/overlay.handler.hpp"
#include "app/control/services/handlers/preview-layout.handler.hpp"
#include "app/control/services/handlers/reboot.handler.hpp"
#include "app/control/services/handlers/recording.handler.hpp"
#include "app/control/services/handlers/session-snapshot.handler.hpp"
#include "app/control/services/handlers/session.handler.hpp"
#include "app/control/services/handlers/set-device-time.handler.hpp"
#include "app/control/services/handlers/set-match-state.handler.hpp"
#include "app/control/services/handlers/streaming.handler.hpp"
#include "app/control/services/handlers/thumbnail.handler.hpp"
#include "app/control/services/handlers/wifi-direct.handler.hpp"
#include "bootstrap/runtime/runtime-clock.hpp"
#include "bootstrap/runtime/runtime-defaults.hpp"

namespace sst::bootstrap {

auto RegisterControlPlane(sst::control::CommandDispatcher& dispatcher, const ControlPlaneDeps& deps)
    -> std::shared_ptr<sst::control::MatchHandler> {
    // Shared frame-truth health providers (U3): telemetry, the session
    // snapshot, and the start-class gates all read THIS one derivation, so no
    // two surfaces can ever disagree about a camera's health.
    auto& pipeline = deps.pipeline;
    const auto camera0_health = [&pipeline]() -> std::optional<sst_cam::CameraHealth> {
        const auto health = pipeline.CameraHealthStatus(0);
        return health ? std::optional{sst::control::ToWireHealth(*health)} : std::nullopt;
    };
    const auto camera1_health = [&pipeline]() -> std::optional<sst_cam::CameraHealth> {
        const auto health = pipeline.CameraHealthStatus(1);
        return health ? std::optional{sst::control::ToWireHealth(*health)} : std::nullopt;
    };
    // Refuses start-class commands (record / stream) with DEVICE_INOPERABLE
    // while any camera is not OK. Stop/finalize, downloads, wifi-direct,
    // reboot and diagnostics reads are never gated. Handlers copy the gate,
    // so this local does not need to outlive the registration.
    const sst::control::StartHealthGate start_health_gate(camera0_health, camera1_health);

    // Shared activity providers: telemetry and the session snapshot read the
    // SAME sources, so the two surfaces can never disagree.
    auto& recording_service = deps.recording_service;
    auto& streaming_service = deps.streaming_service;
    const auto is_recording = [&recording_service] {
        return recording_service.CurrentState() != sst::storage::RecordingState::kIdle;
    };
    const auto is_streaming = [&streaming_service] {
        return !streaming_service.ListActivePlatformStreams().empty();
    };

    // ── Device identity, telemetry + reconnect handshake ───────────────
    auto& wifi_manager = deps.wifi_manager;
    auto& telemetry_probe = deps.telemetry_probe;
    dispatcher.Register(std::make_shared<sst::control::DeviceHandler>(
        deps.device_cfg, deps.system_stats,
        sst::control::DeviceHandler::Providers{
            .is_recording = is_recording,
            .is_streaming = is_streaming,
            .wifi_state = [&wifi_manager] { return wifi_manager.State(); },
            .internet_reachable =
                [&telemetry_probe] { return telemetry_probe.InternetReachable(); },
            .wifi_signal_dbm = [&telemetry_probe] { return telemetry_probe.WifiSignalDbm(); },
            .camera0_health = camera0_health,
            .camera1_health = camera1_health}));
    // Reconnect handshake (state-health cycle): snapshot read + absolute
    // match-state reconcile + wall-clock push.
    dispatcher.Register(std::make_shared<sst::control::SessionSnapshotHandler>(
        deps.session_manager, deps.manual_camera_state, deps.preview_layout_state,
        sst::control::SessionSnapshotHandler::Providers{.is_recording = is_recording,
                                                        .is_streaming = is_streaming,
                                                        .camera0_health = camera0_health,
                                                        .camera1_health = camera1_health},
        NowEpochMs));
    // Device time: direct clock_settime (CAP_SYS_TIME via the systemd unit's
    // AmbientCapabilities — deploy/install.sh) + opportunistic NTP enable when
    // an uplink exists, bounded like every subprocess on the dispatcher thread.
    dispatcher.Register(std::make_shared<sst::control::SetDeviceTimeHandler>(
        [](std::uint64_t epoch_ms) { return sst::adapters::control::SetRealtimeClock(epoch_ms); },
        [&telemetry_probe] { return telemetry_probe.InternetReachable(); },
        [] {
            sst::adapters::control::RunBounded({"timedatectl", "set-ntp", "true"},
                                               sst::runtime_defaults::kNtpEnableTimeout);
        }));
    // Reboot: `systemctl reboot` requires the sst-cam service user to be
    // permitted (polkit / sudoers — provisioned by deploy/install.sh). Run
    // bounded so a hung call can't stall the single dispatcher thread.
    dispatcher.Register(std::make_shared<sst::control::RebootHandler>([] {
        return sst::adapters::control::RunBounded({"systemctl", "reboot"},
                                                  sst::runtime_defaults::kRebootTimeout);
    }));

    // ── Session + match state / overlay ────────────────────────────────
    dispatcher.Register(std::make_shared<sst::control::SessionHandler>(deps.session_manager,
                                                                       deps.overlay_controller));
    dispatcher.Register(std::make_shared<sst::control::SetMatchStateHandler>(
        deps.session_manager, deps.overlay_controller, NowMs));
    dispatcher.Register(
        std::make_shared<sst::control::MatchStateHandler>(deps.session_manager, NowEpochMs));
    dispatcher.Register(std::make_shared<sst::control::OverlayHandler>(
        deps.session_manager, deps.overlay_controller, NowMs));
    auto match_handler = std::make_shared<sst::control::MatchHandler>(
        deps.session_manager, deps.overlay_controller, NowMs);
    dispatcher.Register(match_handler);

    // ── Capture outputs: recording / streaming / thumbnail ─────────────
    // The internal dual-camera proxy has NO handler: it rides the record/stream
    // lifecycle automatically (ProxyLifecycle) and is invisible to the app.
    dispatcher.Register(std::make_shared<sst::control::RecordingHandler>(
        deps.session_manager, recording_service, deps.overlay_timeline, deps.proxy_lifecycle, NowMs,
        start_health_gate));
    dispatcher.Register(std::make_shared<sst::control::StreamingHandler>(
        streaming_service, &deps.uplink_probe, start_health_gate, &deps.proxy_lifecycle));
    // On-demand thumbnail: snapshot the latest pipeline frame + encode to JPEG
    // in memory.
    dispatcher.Register(std::make_shared<sst::control::ThumbnailHandler>(
        deps.pipeline, deps.jpeg_encoder, NowEpochMs));

    // ── Connectivity: WiFi-Direct data plane, uplink, downloads, export ─
    dispatcher.Register(std::make_shared<sst::control::WifiDirectHandler>(
        deps.session_manager, wifi_manager, deps.network_configurator, deps.dhcp_server,
        streaming_service, sst::control::PreviewPort{sst::runtime_defaults::kPreviewPort},
        sst::control::DownloadPort{sst::runtime_defaults::kDownloadPort}));
    dispatcher.Register(std::make_shared<sst::control::NetworkHandler>(
        deps.uplink_manager, deps.uplink_store, deps.uplink_cfg));
    dispatcher.Register(std::make_shared<sst::control::DownloadHandler>(
        deps.download_server, sst::runtime_defaults::kGroupOwnerIp,
        sst::runtime_defaults::kDownloadPort, sst::runtime_defaults::kDownloadTokenTtlSeconds));
    dispatcher.Register(std::make_shared<sst::control::ExportBurnHandler>(
        deps.export_manager,
        [is_recording, is_streaming] { return is_recording() || is_streaming(); },
        sst::runtime_defaults::kGroupOwnerIp, sst::runtime_defaults::kDownloadPort));

    // ── Image tuning + camera selection ────────────────────────────────
    dispatcher.Register(std::make_shared<sst::control::PreviewLayoutHandler>(
        deps.preview_layout_state, sst::runtime_defaults::kOverlayWidth,
        sst::runtime_defaults::kOverlayHeight));
    dispatcher.Register(
        std::make_shared<sst::control::CameraCalibrationHandler>(deps.calibration_state));
    dispatcher.Register(std::make_shared<sst::control::AutoWhiteBalanceHandler>(
        deps.frame_color_stats, deps.calibration_state));
    dispatcher.Register(
        std::make_shared<sst::control::CameraFocusHandler>(deps.focuser, deps.focus_state));
    dispatcher.Register(
        std::make_shared<sst::control::ActiveCameraHandler>(deps.manual_camera_state));

    return match_handler;
}

}  // namespace sst::bootstrap
