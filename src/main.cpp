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
#include "adapters/control/system/proc-system-stats.hpp"
#include "adapters/control/wifi/wpa_supplicant/dnsmasq-dhcp-server.hpp"
#include "adapters/control/wifi/wpa_supplicant/ip-network-configurator.hpp"
#include "adapters/control/wifi/wpa_supplicant/wpa-wifi-manager.hpp"
#include "adapters/network/http/http-download-server.hpp"
#include "adapters/overlay/caching/caching-overlay-sink.hpp"
#include "adapters/overlay/cairo/cairo-overlay-renderer.hpp"
#include "adapters/processing/opencv/opencv-postprocessor.hpp"
#include "adapters/processing/opencv/opencv-preprocessor.hpp"
#include "adapters/storage/filesystem/filesystem-disk-guard.hpp"
#include "adapters/storage/gstreamer/gst-continuous-recorder.hpp"
#include "adapters/storage/opencv/opencv-jpeg-encoder.hpp"
#include "adapters/storage/opencv/opencv-thumbnail-writer.hpp"
#include "adapters/storage/raw_capture/filesystem-raw-capture-sink.hpp"
#include "adapters/streaming/gst_rtmp/gst-rtmp-streamer.hpp"
#include "adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.hpp"
#include "app/buffer/services/fan_out_sink/fan-out-sink.hpp"
#include "app/config/services/config_loader/config-loader.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/device.handler.hpp"
#include "app/control/services/handlers/download.handler.hpp"
#include "app/control/services/handlers/match-state.handler.hpp"
#include "app/control/services/handlers/match.handler.hpp"
#include "app/control/services/handlers/overlay.handler.hpp"
#include "app/control/services/handlers/raw-capture.handler.hpp"
#include "app/control/services/handlers/recording.handler.hpp"
#include "app/control/services/handlers/session.handler.hpp"
#include "app/control/services/handlers/streaming.handler.hpp"
#include "app/control/services/handlers/thumbnail.handler.hpp"
#include "app/control/services/handlers/wifi-direct.handler.hpp"
#include "app/decision/services/static_decision/static-decision.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/session/services/session_cleanup/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "app/storage/services/recording_service/recording-service.hpp"
#include "app/streaming/services/streaming_service/streaming-service.hpp"
#include "domain/capture/models/camera-config.hpp"
#include "domain/control/utils/advertised-name.hpp"

namespace sst::paths {

constexpr const char* kConfigDir = "/etc/sst/cam/config";
constexpr const char* kConfigFormat = "json";
constexpr const char* kVideoRootFallback = "/var/lib/sst/cam/videos";

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
    sst::adapters::control::WpaWifiManager wifi_manager;
    sst::adapters::control::IpNetworkConfigurator network_configurator;
    sst::adapters::control::DnsmasqDhcpServer dhcp_server;

    // ── Session (lifecycle SM + disconnect cleanup) ────────────────────
    sst::session::SessionCleanup cleanup(recording_service, streaming_service, wifi_manager,
                                         dhcp_server);
    sst::session::SessionManager session_manager(cleanup);

    // ── Overlay (scene -> Cairo/Pango RGBA -> caching sink) ────────────
    // The controller renders on change into the caching sink; the pipeline
    // consumer reads the latest RGBA from the same sink and alpha-blends it onto
    // each final BGR frame (CPU composite, single path for all output branches).
    sst::adapters::overlay::CairoOverlayRenderer overlay_renderer;
    sst::adapters::overlay::CachingOverlaySink overlay_sink;
    sst::overlay::OverlayController overlay_controller(
        overlay_renderer, overlay_sink,
        sst::common::OutputSize{sst::runtime_defaults::kOverlayWidth,
                                sst::runtime_defaults::kOverlayHeight});

    // ── Downloads ──────────────────────────────────────────────────────
    sst::network::DownloadServer download_server(video_root, NowUnixSeconds);
    // The HTTP server hands out token-gated byte ranges. Bind on all interfaces
    // (0.0.0.0) rather than the GO IP: that address only exists once a WiFi
    // Direct session is up, and INADDR_ANY still accepts connections on it when
    // it appears. Per-request bearer tokens (not network reachability) gate
    // access; in the field the P2P link is the only active interface.
    sst::adapters::network::HttpDownloadServer http_download_server(
        "0.0.0.0", static_cast<std::uint16_t>(sst::runtime_defaults::kDownloadPort),
        [&download_server](const std::string& token) {
            return download_server.ValidateToken(token);
        });

    // ── System stats (telemetry source) ────────────────────────────────
    sst::adapters::control::ProcSystemStats system_stats(video_root);

    // Raw dual-camera capture sink — taps both materialized camera chains,
    // independent of the final recording. Writes per-camera raw files under the
    // video root. Constructed here (ahead of the pipeline that pushes into it)
    // so DeviceHandler can report its IsCapturing() state as is_raw_capturing
    // telemetry.
    sst::adapters::storage::FilesystemRawCaptureSink raw_capture_sink(
        cfg.storage.video.value_or(sst::paths::kVideoRootFallback), /*camera_count=*/2);

    // ── Control plane: dispatcher + per-concern handlers ───────────────
    sst::control::CommandDispatcher dispatcher;

    // ★ Extensibility point — one Register() line per concern.
    dispatcher.Register(std::make_shared<sst::control::DeviceHandler>(
        cfg.device, system_stats,
        [&recording_service] {
            return recording_service.CurrentState() != sst::storage::RecordingState::kIdle;
        },
        [&streaming_service] { return !streaming_service.ListActivePlatformStreams().empty(); },
        [&raw_capture_sink] { return raw_capture_sink.IsCapturing(); },
        [&wifi_manager] { return wifi_manager.State(); }));
    dispatcher.Register(std::make_shared<sst::control::SessionHandler>(session_manager));
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
    dispatcher.Register(
        std::make_shared<sst::control::RecordingHandler>(session_manager, recording_service));
    dispatcher.Register(std::make_shared<sst::control::StreamingHandler>(streaming_service));
    dispatcher.Register(std::make_shared<sst::control::DownloadHandler>(
        download_server, sst::runtime_defaults::kGroupOwnerIp, sst::runtime_defaults::kDownloadPort,
        sst::runtime_defaults::kDownloadTokenTtlSeconds));

    // ── BLE transport ──────────────────────────────────────────────────
    const std::string advertised_name = sst::control::MakeAdvertisedName(
        sst::control::DeriveUnitNumber(cfg.device.serial_number.value_or("")));
    sst::adapters::control::BluezBleTransport ble_transport(advertised_name);
    ble_transport.SetOnCommand(
        [&dispatcher](const sst_cam::Command& cmd) { return dispatcher.Dispatch(cmd); });
    ble_transport.SetOnConnect([&session_manager] { session_manager.OnConnect(); });
    ble_transport.SetOnDisconnect([&session_manager] { session_manager.OnDisconnect(); });

    // ── Pipeline (capture -> preprocess -> buffer -> postprocess -> fan-out) ──
    // FanOutSink feeds both storage + streaming; the overlay is composited onto
    // the shared frame within each output branch so both carry identical pixels.
    sst::buffer::FanOutSink final_frame_sink(
        std::vector<sst::buffer::IFrameSink*>{&recording_service, &streaming_service});

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
    auto postprocessor = std::make_unique<sst::adapters::processing::OpenCvPostprocessor>();
    auto decision = std::make_unique<sst::decision::StaticDecision>();

    sst::pipeline::PipelineOrchestrator pipeline(
        std::move(camera_chains), std::move(postprocessor), std::move(decision), final_frame_sink,
        sst::pipeline::PipelineConfig{}, &raw_capture_sink, &overlay_sink);
    dispatcher.Register(std::make_shared<sst::control::RawCaptureHandler>(raw_capture_sink));

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
