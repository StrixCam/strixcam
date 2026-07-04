#include <spdlog/spdlog.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "adapters/capture/frame/gstreamer/gstreamer.hpp"
#include "adapters/control/ble/bluez/bluez-ble-transport.hpp"
#include "adapters/control/network/ip-route-uplink-probe.hpp"
#include "adapters/control/network/iw-station-rssi-probe.hpp"
#include "adapters/control/network/nmcli-uplink-configurator.hpp"
#include "adapters/control/network/subprocess.hpp"
#include "adapters/control/system/proc-system-stats.hpp"
#include "adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.hpp"
#include "adapters/control/wifi/wpa_supplicant/ip-network-configurator.hpp"
#include "adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.hpp"
#include "adapters/focus/i2c-focuser.hpp"
#include "adapters/network/http/http-download-server.hpp"
#include "adapters/network/json/json-uplink-store.hpp"
#include "adapters/overlay/burn/opencv-overlay-burner.hpp"
#include "adapters/overlay/caching/caching-overlay-sink.hpp"
#include "adapters/overlay/cairo/cairo-overlay-renderer.hpp"
#include "adapters/overlay/timeline/filesystem-overlay-timeline-recorder.hpp"
#include "adapters/overlay/timeline/overlay-timeline-loader.hpp"
#include "adapters/processing/opencv/opencv-postprocessor.hpp"
#include "adapters/processing/opencv/opencv-preprocessor.hpp"
#include "adapters/processing/opencv/opencv-side-by-side-compositor.hpp"
#include "adapters/raw_capture/filesystem-raw-capture-sink.hpp"
#include "adapters/raw_capture/proxy-retention.hpp"
#include "adapters/storage/filesystem/filesystem-disk-guard.hpp"
#include "adapters/storage/gstreamer/gst-continuous-recorder.hpp"
#include "adapters/storage/opencv/opencv-jpeg-encoder.hpp"
#include "adapters/storage/opencv/opencv-thumbnail-writer.hpp"
#include "adapters/streaming/gst_rtmp/gst-rtmp-streamer.hpp"
#include "adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.hpp"
#include "app/config/services/config_loader/config-loader.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/active-camera.handler.hpp"
#include "app/control/services/handlers/auto-white-balance.handler.hpp"
#include "app/control/services/handlers/camera-calibration.handler.hpp"
#include "app/control/services/handlers/camera-focus.handler.hpp"
#include "app/control/services/handlers/device.handler.hpp"
#include "app/control/services/handlers/download.handler.hpp"
#include "app/control/services/handlers/export-burn.handler.hpp"
#include "app/control/services/handlers/match-state.handler.hpp"
#include "app/control/services/handlers/match.handler.hpp"
#include "app/control/services/handlers/network.handler.hpp"
#include "app/control/services/handlers/overlay.handler.hpp"
#include "app/control/services/handlers/preview-layout.handler.hpp"
#include "app/control/services/handlers/raw-capture.handler.hpp"
#include "app/control/services/handlers/reboot.handler.hpp"
#include "app/control/services/handlers/recording.handler.hpp"
#include "app/control/services/handlers/session.handler.hpp"
#include "app/control/services/handlers/streaming.handler.hpp"
#include "app/control/services/handlers/thumbnail.handler.hpp"
#include "app/control/services/handlers/wifi-direct.handler.hpp"
#include "app/control/services/telemetry_probe/telemetry-probe.hpp"
#include "app/decision/services/manual_decision/manual-decision.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/session/services/session_cleanup/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "app/storage/services/recording_service/recording-service.hpp"
#include "app/streaming/services/streaming_service/streaming-service.hpp"
#include "domain/capture/models/camera-config.hpp"
#include "domain/control/utils/advertised-name.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/focus/models/focus-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"

namespace sst::paths {

constexpr const char* kConfigDir = "/etc/sst/cam/config";
constexpr const char* kConfigFormat = "json";
constexpr const char* kVideoRootFallback = "/var/lib/sst/cam/videos";
constexpr const char* kThumbnailRootFallback = "/var/lib/sst/cam/thumbnails";

}  // namespace sst::paths

namespace sst::runtime_defaults {

constexpr std::uint16_t kCamera0Index = 0;
constexpr std::uint16_t kCamera1Index = 1;
constexpr std::uint32_t kOverlayWidth = 1280;  // matches postprocess output
constexpr std::uint32_t kOverlayHeight = 720;
constexpr std::uint32_t kPreviewPort = 8554;   // RTSP preview (wifi.proto)
constexpr std::uint32_t kDownloadPort = 8080;  // HTTP downloads
constexpr std::uint64_t kDownloadTokenTtlSeconds = 3600;
constexpr const char* kGroupOwnerIp = "192.168.49.1";
// Bound for the `systemctl reboot` exec — the call returns quickly, but the
// deadline keeps a hung systemd/D-Bus from stalling the dispatcher thread.
constexpr std::chrono::seconds kRebootTimeout{10};

}  // namespace sst::runtime_defaults

namespace {

auto NowMs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

auto NowUnixSeconds() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// Wall-clock epoch milliseconds. Distinct from NowMs (monotonic steady_clock,
// used for relative overlay/match durations): this is the absolute Unix-epoch
// timestamp the app decodes for MatchState.updated_at and
// ThumbnailResponse.capture_timestamp, so it must come from system_clock.
auto NowEpochMs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

// App-as-source-of-truth firmware: a stateless executor of the sst-cam-proto
// contract. main() wires config + pipeline + the full control plane (BLE
// transport -> proto dispatcher -> session SM + per-concern handlers) and runs
// until terminated.
namespace {
auto RunFirmware() -> int {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("sst-cam-firmware starting");

    // ── Config (device identity + lens calibration — the only persistent state) ─
    sst::config::app::ConfigLoader config(sst::paths::kConfigDir, sst::paths::kConfigFormat);
    auto cfg = config.get();
    const std::filesystem::path video_root =
        cfg.storage.video.value_or(sst::paths::kVideoRootFallback);
    const std::filesystem::path thumbnail_root =
        cfg.storage.thumbnails.value_or(sst::paths::kThumbnailRootFallback);

    // Ensure the storage root exists before anything reads it. Without this,
    // fs::space(video_root) fails on a fresh device (ENOENT) and telemetry reports
    // 0 bytes free/total until the first recording would create it.
    std::error_code video_root_ec;
    std::filesystem::create_directories(video_root, video_root_ec);
    if (video_root_ec) {
        spdlog::warn("could not create storage root {}: {}", video_root.string(),
                     video_root_ec.message());
    }

    // ── Storage (single continuous MP4) ────────────────────────────────
    sst::adapters::storage::FilesystemDiskGuard disk_guard(video_root, cfg.storage.min_free_bytes);
    sst::storage::RecordingService recording_service(
        std::make_unique<sst::adapters::storage::GstContinuousRecorder>(),
        std::make_unique<sst::adapters::storage::OpenCvThumbnailWriter>(), disk_guard);

    // ── Streaming (RTSP preview + RTMP egress) ─────────────────────────
    auto app_stream_server = std::make_unique<sst::adapters::streaming::GstRtspAppStreamServer>();
    sst::streaming::StreamingService streaming_service(std::move(app_stream_server), [] {
        return std::make_unique<sst::adapters::streaming::GstRtmpStreamer>();
    });

    // ── WiFi Direct + DHCP ─────────────────────────────────────────────
    // "auto": detect the interface at runtime (names vary per board —
    // wlP1p1s0 on this Jetson, not wlan0). See ResolveWifiInterface.
    // The `sst-cam-NNNN` name doubles as the P2P SSID postfix, so the WiFi-Direct
    // SSID (`DIRECT-XY-sst-cam-NNNN`) is recognizably the same camera as BLE.
    const std::string advertised_name = sst::control::MakeAdvertisedName(
        sst::control::DeriveUnitNumber(cfg.device.serial_number.value_or("")));
    sst::adapters::control::WpaWifiManager wifi_manager("auto", "/run/wpa_supplicant",
                                                        advertised_name);
    sst::adapters::control::IpNetworkConfigurator network_configurator;
    sst::adapters::control::DnsmasqDhcpServer dhcp_server;

    // Internet uplink (ethernet / gated wifi-STA) for cloud streaming — a separate
    // plane from the WiFi-Direct GO that serves the phone its preview. Applied from
    // persisted config on boot; re-pushable from the app over BLE (U4). Driven via
    // NetworkManager (the camera's ethernet is NM-managed, unlike the P2P radio).
    sst::adapters::control::NmcliUplinkConfigurator uplink_configurator;
    sst::adapters::control::IpRouteUplinkProbe uplink_probe;
    // Telemetry signals that need a subprocess to read — internet uplink (`ip
    // route`) and WiFi-Direct peer RSSI (`iw`) — are sampled on a background
    // thread, not inline on the BLE dispatcher. A per-telemetry-request fork
    // there risked stalling every BLE command on a hung tool; the handler now
    // reads cached atomics. Declared after uplink_probe (borrows it), torn down
    // before it.
    sst::adapters::control::IwStationRssiProbe wifi_signal_probe;
    sst::control::TelemetryProbe telemetry_probe(uplink_probe, wifi_signal_probe);
    telemetry_probe.Start();
    sst::network::UplinkManager uplink_manager(uplink_configurator);
    sst::adapters::network::JsonUplinkStore uplink_store(std::string(sst::paths::kConfigDir) +
                                                         "/uplink.json");
    // Bring up enabled uplinks on boot OFF the main path: nmcli con up can stall
    // (NM mid-restart, DHCP wait), and the subprocess deadline is generous (45s).
    // Doing it synchronously here would delay BLE/WiFi-Direct/preview start by
    // that long on a bad boot. Detach so BLE comes up regardless; the manager +
    // configurator outlive main()'s run loop, and a copy of the config is moved
    // into the thread, so there is no use-after-free of locals.
    std::thread([&uplink_manager, boot_cfg = cfg.uplink]() mutable {
        uplink_manager.Apply(boot_cfg);
    }).detach();

    // Training-proxy sink — per-camera H.264 proxy, taps both materialized camera
    // chains, coupled to the match record lifecycle (RecordingHandler drives
    // Start/Stop; SessionCleanup force-stops it on disconnect). Constructed here,
    // ahead of both SessionCleanup and the pipeline that pushes into it, so both
    // can reference it and DeviceHandler can report its IsCapturing() state.
    sst::adapters::raw_capture::FilesystemRawCaptureSink raw_capture_sink(
        cfg.storage.video.value_or(sst::paths::kVideoRootFallback), /*camera_count=*/2);

    // Session-scoped UI selections. Declared here, ahead of SessionCleanup, so
    // it can reset them on disconnect: a reconnect must start from the app's
    // fresh-UI defaults (camera 0 / single view), else the app shows Left while
    // the firmware still serves the previous session's camera (mismatch). The
    // SetActiveCamera / SetPreviewLayout handlers write them; ManualDecision and
    // the pipeline consumer read them each tick.
    static sst::decision::ManualCameraState manual_camera_state;
    sst::streaming::PreviewLayoutState preview_layout_state;

    // ── Session (lifecycle SM + disconnect cleanup) ────────────────────
    sst::session::SessionCleanup cleanup(recording_service, streaming_service, raw_capture_sink,
                                         wifi_manager, dhcp_server, manual_camera_state,
                                         preview_layout_state);
    sst::session::SessionManager session_manager(cleanup);

    // ── Overlay (scene -> Cairo/Pango RGBA -> caching sink) ────────────
    // The controller renders on change into the caching sink; the pipeline
    // consumer reads the latest RGBA from the same sink and alpha-blends it onto
    // each final BGR frame (CPU composite, single path for all output branches).
    sst::adapters::overlay::CairoOverlayRenderer overlay_renderer;
    sst::adapters::overlay::CachingOverlaySink overlay_sink;
    // Persists the overlay scene timeline beside each L1 recording (#6 F6b) so
    // the overlay can be burned onto the clean recording on demand (F6c).
    sst::adapters::overlay::FilesystemOverlayTimelineRecorder overlay_timeline;
    sst::overlay::OverlayController overlay_controller(
        overlay_renderer, overlay_sink,
        sst::common::OutputSize{sst::runtime_defaults::kOverlayWidth,
                                sst::runtime_defaults::kOverlayHeight},
        &overlay_timeline);

    // ── Downloads ──────────────────────────────────────────────────────
    sst::network::DownloadServer download_server(video_root, thumbnail_root, NowUnixSeconds);

    // Training-proxy retention (U7): a detached periodic sweep bounds total proxy
    // footage (the raw__*.mp4 pairs accumulate one per match). Delete-oldest,
    // protected against the actively-writing group (mtime grace inside Sweep) and
    // any file with a live download token. Off-thread so it never blocks the BLE
    // surface; a no-op when proxy_max_total_bytes is unset.
    static sst::adapters::raw_capture::ProxyRetention proxy_retention(
        video_root, cfg.storage.proxy_max_total_bytes.value_or(0));
    if (cfg.storage.proxy_max_total_bytes) {
        std::thread([&download_server] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::minutes(5));
                proxy_retention.Sweep([&download_server](const std::filesystem::path& file) {
                    return download_server.IsTokened(file);
                });
            }
        }).detach();
    }
    // The HTTP server hands out token-gated byte ranges. Bind on all interfaces
    // (0.0.0.0) rather than the GO IP: that address only exists once a WiFi
    // Direct session is up, and INADDR_ANY still accepts connections on it when
    // it appears. Per-request bearer tokens (not network reachability) gate
    // access; in the field the P2P link is the only active interface.
    sst::adapters::network::HttpDownloadServer http_download_server(
        "0.0.0.0", static_cast<std::uint16_t>(sst::runtime_defaults::kDownloadPort),
        [&download_server](const std::string& token) {
            return download_server.ValidateToken(token);
        },
        [&download_server](const std::string& recording_id) {
            return download_server.ResolveThumbnailPath(recording_id);
        });

    // ── Overlay export (on-demand burn, #6 F6c) ─────────────────────────
    // Burns the recorded overlay timeline onto a clean L1 into an L2 living
    // OUTSIDE the video root (so it never shows up in the recordings list),
    // tokened for download exactly like a normal recording.
    sst::adapters::overlay::OpenCvOverlayBurner overlay_burner;
    sst::exportjob::ExportJobManager export_manager(
        overlay_burner,
        [&download_server](const std::string& recording_id) {
            return download_server.ResolveRecordingPath(recording_id);
        },
        [](const std::filesystem::path& l1_path) {
            const auto timeline_path =
                l1_path.parent_path() / (l1_path.stem().string() + ".timeline.json");
            return sst::adapters::overlay::LoadOverlayTimeline(timeline_path)
                .value_or(sst::overlay::OverlayTimeline{});
        },
        [&download_server](const std::filesystem::path& l2_path, const std::string& job_id) {
            return download_server.MintTokenForFile(
                l2_path, job_id, sst::runtime_defaults::kDownloadTokenTtlSeconds);
        },
        video_root.parent_path() / "exports");

    // ── System stats (telemetry source) ────────────────────────────────
    sst::adapters::control::ProcSystemStats system_stats(video_root);

    // ── Control plane: dispatcher + per-concern handlers ───────────────
    sst::control::CommandDispatcher dispatcher;

    // ★ Extensibility point — one Register() line per concern.
    dispatcher.Register(std::make_shared<sst::control::DeviceHandler>(
        cfg.device, system_stats,
        sst::control::DeviceHandler::Providers{
            .is_recording =
                [&recording_service] {
                    return recording_service.CurrentState() != sst::storage::RecordingState::kIdle;
                },
            .is_streaming =
                [&streaming_service] {
                    return !streaming_service.ListActivePlatformStreams().empty();
                },
            .is_raw_capturing = [&raw_capture_sink] { return raw_capture_sink.IsCapturing(); },
            .wifi_state = [&wifi_manager] { return wifi_manager.State(); },
            .internet_reachable =
                [&telemetry_probe] { return telemetry_probe.InternetReachable(); },
            .wifi_signal_dbm = [&telemetry_probe] { return telemetry_probe.WifiSignalDbm(); }}));
    dispatcher.Register(
        std::make_shared<sst::control::SessionHandler>(session_manager, overlay_controller));
    dispatcher.Register(std::make_shared<sst::control::WifiDirectHandler>(
        session_manager, wifi_manager, network_configurator, dhcp_server, streaming_service,
        sst::control::PreviewPort{sst::runtime_defaults::kPreviewPort},
        sst::control::DownloadPort{sst::runtime_defaults::kDownloadPort}));
    dispatcher.Register(
        std::make_shared<sst::control::OverlayHandler>(session_manager, overlay_controller, NowMs));
    auto match_handler =
        std::make_shared<sst::control::MatchHandler>(session_manager, overlay_controller, NowMs);
    dispatcher.Register(match_handler);
    dispatcher.Register(
        std::make_shared<sst::control::MatchStateHandler>(session_manager, NowEpochMs));
    dispatcher.Register(std::make_shared<sst::control::RecordingHandler>(
        session_manager, recording_service, overlay_timeline, raw_capture_sink, NowMs));
    dispatcher.Register(
        std::make_shared<sst::control::StreamingHandler>(streaming_service, &uplink_probe));
    dispatcher.Register(std::make_shared<sst::control::DownloadHandler>(
        download_server, sst::runtime_defaults::kGroupOwnerIp, sst::runtime_defaults::kDownloadPort,
        sst::runtime_defaults::kDownloadTokenTtlSeconds));
    dispatcher.Register(std::make_shared<sst::control::ExportBurnHandler>(
        export_manager,
        [&recording_service, &streaming_service] {
            return recording_service.CurrentState() != sst::storage::RecordingState::kIdle ||
                   !streaming_service.ListActivePlatformStreams().empty();
        },
        sst::runtime_defaults::kGroupOwnerIp, sst::runtime_defaults::kDownloadPort));

    // ── BLE transport ──────────────────────────────────────────────────
    // advertised_name computed above (shared with the WiFi-Direct SSID postfix).
    sst::adapters::control::BluezBleTransport ble_transport(advertised_name);
    ble_transport.SetOnCommand(
        [&dispatcher](const sst_cam::Command& cmd) { return dispatcher.Dispatch(cmd); });
    ble_transport.SetOnConnect([&session_manager] { session_manager.OnConnect(); });
    ble_transport.SetOnDisconnect([&session_manager] { session_manager.OnDisconnect(); });

    // ── Pipeline (capture -> preprocess -> buffer -> postprocess -> split) ──
    // The recorder gets the CLEAN post-processed frame; streaming (RTSP + RTMP,
    // which StreamingService fans out internally) gets the overlaid copy. The
    // overlay is composited only on the stream branch inside the consumer, so a
    // recording plays with or without overlays — overlay is burned on demand (#6).

    // Two camera chains (sensor-id 0 and 1). Both run; StaticDecision presents
    // camera 0 full-frame (the intelligence seam), camera 1 ages out unchosen
    // but stays live so raw dual capture can tap it.
    const sst::capture::CameraConfig camera_cfg{};
    const std::string device_model = cfg.device.model.value_or("");
    std::vector<sst::pipeline::CameraChain> camera_chains;
    camera_chains.push_back(sst::pipeline::CameraChain{
        .capture = std::make_unique<sst::capture::GStreamerAdapter>(
            camera_cfg, device_model, sst::runtime_defaults::kCamera0Index),
        .preprocessor = std::make_unique<sst::adapters::processing::OpenCvPreprocessor>()});
    camera_chains.push_back(sst::pipeline::CameraChain{
        .capture = std::make_unique<sst::capture::GStreamerAdapter>(
            camera_cfg, device_model, sst::runtime_defaults::kCamera1Index),
        .preprocessor = std::make_unique<sst::adapters::processing::OpenCvPreprocessor>()});
    // WB magenta-correction gains are env-tunable so the cast can be dialed in
    // on-device without a rebuild (a fixed grey-world gain over/under-shoots per
    // scene). SST_WB_DISABLE turns it off; SST_WB_{R,G,B}GAIN override each gain.
    sst::processing::PostprocessConfig postproc_cfg;
    auto& wb = postproc_cfg.color_correction;
    if (std::getenv("SST_WB_DISABLE") != nullptr) {
        wb.enabled = false;
    }
    const auto env_gain = [](const char* name, float fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr) {
            return fallback;
        }
        try {
            return std::stof(value);
        } catch (const std::exception&) {
            return fallback;
        }
    };
    wb.r_gain = env_gain("SST_WB_RGAIN", wb.r_gain);
    wb.g_gain = env_gain("SST_WB_GGAIN", wb.g_gain);
    wb.b_gain = env_gain("SST_WB_BGAIN", wb.b_gain);
    spdlog::info("Postprocess WB correction: enabled={} R={:.2f} G={:.2f} B={:.2f}", wb.enabled,
                 wb.r_gain, wb.g_gain, wb.b_gain);
    // Live WB calibration state (diagnostic Calibration screen). Seeded from the
    // resolved default/env gains; the postprocessor samples it each frame and the
    // SetCameraCalibration handler writes it, so slider drags retune the preview
    // live. Must outlive the postprocessor (moved into the pipeline below).
    sst::processing::ColorCalibrationState calibration_state(
        {.r = wb.r_gain,
         .g = wb.g_gain,
         .b = wb.b_gain,
         .enabled = wb.enabled,
         .saturation = env_gain("SST_SATURATION", sst::processing::kDefaultSaturation),
         .contrast = env_gain("SST_CONTRAST", sst::processing::kDefaultContrast),
         .brightness = env_gain("SST_BRIGHTNESS", sst::processing::kDefaultBrightness)});
    // Motorized-focus VCM driver + shared per-camera focus mode/position (U8/U9).
    // Must outlive the handlers + the AF loop below.
    static sst::adapters::focus::I2cFocuser focuser;
    static sst::focus::FocusState focus_state;

    // Latest pre-correction frame average — auto-white-balance reads it to compute
    // grey-world gains. Must outlive the postprocessor (moved into the pipeline).
    static sst::processing::FrameColorStats frame_color_stats;
    auto postprocessor = std::make_unique<sst::adapters::processing::OpenCvPostprocessor>(
        postproc_cfg, &calibration_state, &frame_color_stats);
    // Manual tracking: ManualDecision reads manual_camera_state (declared above,
    // near SessionCleanup which resets it on disconnect) each tick. Becomes the
    // override once AI decision lands.
    auto decision = std::make_unique<sst::decision::ManualDecision>(manual_camera_state);

    // #6 F6d dual preview: the SetPreviewLayout handler flips preview_layout_state
    // (declared above, reset on disconnect); the pipeline consumer reads it each
    // tick and composites cam0 | cam1 (clean) into the preview stream via the
    // side-by-side compositor. The composite targets the same output canvas as the
    // single stream, so the RTSP encoder caps never change — connected viewers are
    // not dropped on a layout switch.
    sst::adapters::processing::OpenCvSideBySideCompositor side_by_side_compositor(
        sst::runtime_defaults::kOverlayWidth, sst::runtime_defaults::kOverlayHeight);

    sst::pipeline::PipelineOrchestrator pipeline(
        std::move(camera_chains), std::move(postprocessor), std::move(decision), recording_service,
        streaming_service, sst::pipeline::PipelineConfig{}, &raw_capture_sink, &overlay_sink,
        &side_by_side_compositor, &preview_layout_state);
    dispatcher.Register(std::make_shared<sst::control::RawCaptureHandler>(raw_capture_sink));
    dispatcher.Register(std::make_shared<sst::control::PreviewLayoutHandler>(
        preview_layout_state, sst::runtime_defaults::kOverlayWidth,
        sst::runtime_defaults::kOverlayHeight));
    dispatcher.Register(
        std::make_shared<sst::control::CameraCalibrationHandler>(calibration_state));
    dispatcher.Register(std::make_shared<sst::control::AutoWhiteBalanceHandler>(frame_color_stats,
                                                                                calibration_state));
    dispatcher.Register(std::make_shared<sst::control::CameraFocusHandler>(focuser, focus_state));
    dispatcher.Register(std::make_shared<sst::control::ActiveCameraHandler>(manual_camera_state));
    dispatcher.Register(
        std::make_shared<sst::control::NetworkHandler>(uplink_manager, uplink_store, cfg.uplink));
    // Reboot: `systemctl reboot` requires the sst-cam service user to be
    // permitted (polkit / sudoers — provisioned by deploy/install.sh). Run
    // bounded so a hung call can't stall the single dispatcher thread.
    dispatcher.Register(std::make_shared<sst::control::RebootHandler>([] {
        return sst::adapters::control::RunBounded({"systemctl", "reboot"},
                                                  sst::runtime_defaults::kRebootTimeout);
    }));

    // On-demand thumbnail: snapshot the latest pipeline frame + encode to JPEG
    // in memory. Registered here (after the pipeline exists) but before the BLE
    // transport starts, so it's wired by the time a command can arrive.
    sst::adapters::storage::OpenCvJpegEncoder jpeg_encoder;
    dispatcher.Register(
        std::make_shared<sst::control::ThumbnailHandler>(pipeline, jpeg_encoder, NowEpochMs));

    if (!pipeline.Start()) {
        spdlog::error("pipeline failed to start (camera unavailable?) — aborting");
        return 1;
    }
    ble_transport.Start();
    if (!ble_transport.IsRunning()) {
        spdlog::error("BLE transport failed to start — aborting");
        pipeline.Stop();
        return 1;
    }
    if (!http_download_server.Start()) {
        // Non-fatal: the camera still records/streams; only downloads are off.
        spdlog::warn("HTTP download server failed to bind on :{} — downloads disabled",
                     sst::runtime_defaults::kDownloadPort);
    }

    // Drive the display-only match clock at ~1 Hz (the app is still the timing
    // authority; this only advances the on-screen clock between app events).
    std::atomic<bool> clock_running{true};
    std::thread clock_thread([&match_handler, &clock_running] {
        while (clock_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            match_handler->TickClock();
        }
    });

    spdlog::info("startup complete — advertising as {}", advertised_name);

    // Run until terminated (SIGINT/SIGTERM). Destructors stop the pipeline +
    // transport (which finalizes any active session via the disconnect hook).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        spdlog::error("sigprocmask failed — cannot guarantee clean shutdown; aborting");
        clock_running.store(false);
        clock_thread.join();
        http_download_server.Stop();
        ble_transport.Stop();
        pipeline.Stop();
        return 1;
    }
    int signo = 0;
    sigwait(&mask, &signo);
    spdlog::info("signal {} received — shutting down", signo);

    clock_running.store(false);
    clock_thread.join();
    telemetry_probe.Stop();
    http_download_server.Stop();
    ble_transport.Stop();
    pipeline.Stop();
    return 0;
}
}  // namespace

// Catch any startup/runtime exception so the failure is LOGGED and the exit is
// clean, instead of a silent std::terminate/abort that systemd merely restarts
// (e.g. a malformed config file, or a subsystem that can't reach its hardware).
auto main() -> int {
    try {
        return RunFirmware();
    } catch (const std::exception& e) {
        spdlog::critical("sst-cam-firmware: fatal error: {}", e.what());
        return 1;
    } catch (...) {
        spdlog::critical("sst-cam-firmware: fatal error (unknown exception)");
        return 1;
    }
}
