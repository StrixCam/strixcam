#include <spdlog/spdlog.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "adapters/control/ble/bluez/bluez-ble-transport.hpp"
#include "adapters/control/network/ip-route-uplink-probe.hpp"
#include "adapters/control/network/iw-station-rssi-probe.hpp"
#include "adapters/control/network/nmcli-uplink-configurator.hpp"
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
#include "adapters/processing/opencv/opencv-side-by-side-compositor.hpp"
#include "adapters/session/json/json-session-summary-store.hpp"
#include "adapters/storage/filesystem/filesystem-disk-guard.hpp"
#include "adapters/storage/gstreamer/gst-continuous-recorder.hpp"
#include "adapters/storage/opencv/opencv-jpeg-encoder.hpp"
#include "adapters/storage/opencv/opencv-thumbnail-writer.hpp"
#include "adapters/storage/raw_capture/filesystem-raw-capture-sink.hpp"
#include "adapters/storage/raw_capture/proxy-retention.hpp"
#include "adapters/streaming/gst_rtmp/gst-rtmp-streamer.hpp"
#include "adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.hpp"
#include "app/config/services/config_loader/config-loader.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/telemetry_probe/telemetry-probe.hpp"
#include "app/decision/services/manual_decision/manual-decision.hpp"
#include "app/export/services/export-job-manager.hpp"
#include "app/focus/services/autofocus/autofocus-service.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/session/services/session_cleanup/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"
#include "app/storage/services/recording_service/recording-service.hpp"
#include "app/streaming/services/streaming_service/streaming-service.hpp"
#include "bootstrap/control/control-plane.hpp"
#include "bootstrap/pipeline/camera-chains.hpp"
#include "bootstrap/pipeline/color-calibration.hpp"
#include "bootstrap/runtime/runtime-clock.hpp"
#include "bootstrap/runtime/runtime-defaults.hpp"
#include "domain/capture/models/camera-config.hpp"
#include "domain/common/services/periodic-worker.hpp"
#include "domain/control/utils/advertised-name.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/focus/models/focus-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"

// App-as-source-of-truth firmware: a stateless executor of the sst-cam-proto
// contract. main() wires config + pipeline + the full control plane (BLE
// transport -> proto dispatcher -> session SM + per-concern handlers) and runs
// until terminated. Pure composition: construction order IS destruction-order
// safety here (locals are destroyed in reverse, so everything is declared
// before what borrows it), and the policy/wiring details live in
// src/bootstrap/.
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
    sst::streaming::StreamingService streaming_service(
        std::make_unique<sst::adapters::streaming::GstRtspAppStreamServer>(),
        [] { return std::make_unique<sst::adapters::streaming::GstRtmpStreamer>(); });

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
    // that long on a bad boot. A jthread so BLE comes up regardless, but shutdown
    // still joins it (bounded by the subprocess deadline) — declared after
    // uplink_manager, so it joins before the manager it borrows is destroyed.
    std::jthread uplink_boot_apply(
        [&uplink_manager, boot_cfg = cfg.uplink]() mutable { uplink_manager.Apply(boot_cfg); });

    // Training-proxy sink — per-camera H.264 proxy, taps both materialized camera
    // chains. Constructed here, ahead of both SessionCleanup and the pipeline
    // that pushes into it, so both can reference it and DeviceHandler can report
    // its IsCapturing() state.
    sst::adapters::storage::FilesystemRawCaptureSink raw_capture_sink(
        cfg.storage.video.value_or(sst::paths::kVideoRootFallback), /*camera_count=*/2);
    // Record-or-stream proxy ref-count (U5): the proxy runs while a recording OR
    // an RTMP egress is active and stops on last-out, so streaming-only matches
    // still produce training footage. RecordingHandler drives the record leg,
    // StreamingHandler the stream leg, SessionCleanup force-stops (and resets
    // both holds) at session end. The always-on RTSP preview takes no hold.
    sst::storage::ProxyLifecycle proxy_lifecycle(raw_capture_sink);

    // Session-scoped UI selections. Declared here, ahead of SessionCleanup, so
    // it can reset them on disconnect: a reconnect must start from the app's
    // fresh-UI defaults (camera 0 / single view), else the app shows Left while
    // the firmware still serves the previous session's camera (mismatch). The
    // SetActiveCamera / SetPreviewLayout handlers write them; ManualDecision and
    // the pipeline consumer read them each tick.
    sst::decision::ManualCameraState manual_camera_state;
    sst::streaming::PreviewLayoutState preview_layout_state;

    // Persists the overlay scene timeline beside each L1 recording (#6 F6b) so
    // the overlay can be burned onto the clean recording on demand (F6c).
    // Declared ahead of SessionCleanup, which flushes it at session end.
    sst::adapters::overlay::FilesystemOverlayTimelineRecorder overlay_timeline;

    // ── Session (orthogonal axes SM + session-end cleanup fan-out) ─────
    // The summary store persists the last-session summary in the config dir
    // (write-ahead on recording start), so a crash mid-recording is reconciled
    // into a reboot/file-invalid summary at the next boot.
    sst::session::SessionCleanup cleanup(recording_service, streaming_service, proxy_lifecycle,
                                         overlay_timeline, wifi_manager, dhcp_server,
                                         manual_camera_state, preview_layout_state);
    sst::adapters::session::JsonSessionSummaryStore session_summary_store(
        std::string(sst::paths::kConfigDir) + "/last-session.json");
    sst::session::SessionManager session_manager(cleanup, &session_summary_store);

    // ── Overlay (scene -> Cairo/Pango RGBA -> caching sink) ────────────
    // The controller renders on change into the caching sink; the pipeline
    // consumer reads the latest RGBA from the same sink and alpha-blends it onto
    // each final BGR frame (CPU composite, single path for all output branches).
    sst::adapters::overlay::CairoOverlayRenderer overlay_renderer;
    sst::adapters::overlay::CachingOverlaySink overlay_sink;
    sst::overlay::OverlayController overlay_controller(
        overlay_renderer, overlay_sink,
        sst::common::OutputSize{sst::runtime_defaults::kOverlayWidth,
                                sst::runtime_defaults::kOverlayHeight},
        &overlay_timeline);

    // ── Downloads ──────────────────────────────────────────────────────
    sst::network::DownloadServer download_server(video_root, thumbnail_root,
                                                 sst::bootstrap::NowUnixSeconds);

    // Training-proxy retention (U7): a periodic sweep bounds total proxy footage
    // (the raw__*.mp4 pairs accumulate one per match). Delete-oldest, protected
    // against the actively-writing group (mtime grace inside Sweep) and any file
    // with a live download token. Off-thread so it never blocks the BLE surface
    // (owned worker, joined at shutdown); a no-op when proxy_max_total_bytes is
    // unset.
    sst::adapters::storage::ProxyRetention proxy_retention(
        video_root, cfg.storage.proxy_max_total_bytes.value_or(0));
    constexpr std::chrono::minutes kProxySweepInterval{5};
    sst::common::PeriodicWorker proxy_retention_sweep(
        kProxySweepInterval, [&proxy_retention, &download_server] {
            proxy_retention.Sweep([&download_server](const std::filesystem::path& file) {
                return download_server.IsTokened(file);
            });
        });
    if (cfg.storage.proxy_max_total_bytes) {
        proxy_retention_sweep.Start();
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

    // ── Pipeline (capture -> preprocess -> buffer -> postprocess -> split) ──
    // Constructed BEFORE the control plane below: the telemetry / snapshot
    // health providers and the start-class health gates all read the
    // orchestrator's frame-truth health (U3), so the handlers capture it.
    // The recorder gets the CLEAN post-processed frame; streaming (RTSP + RTMP,
    // which StreamingService fans out internally) gets the overlaid copy. The
    // overlay is composited only on the stream branch inside the consumer, so a
    // recording plays with or without overlays — overlay is burned on demand (#6).

    // Two camera chains (sensor-id 0 and 1). Both run; ManualDecision presents
    // one camera full-frame (the intelligence seam), the other ages out unchosen
    // but stays live so raw dual capture can tap it.
    const sst::capture::CameraConfig camera_cfg{};
    auto camera_chains =
        sst::bootstrap::BuildCameraChains(camera_cfg, cfg.device.model.value_or(""));

    const auto postproc_cfg = sst::bootstrap::ResolvePostprocessConfig();
    // Live WB calibration state (diagnostic Calibration screen). Seeded from the
    // resolved default/env gains; the postprocessor samples it each frame and the
    // SetCameraCalibration handler writes it, so slider drags retune the preview
    // live. Must outlive the postprocessor (moved into the pipeline below).
    sst::processing::ColorCalibrationState calibration_state(
        sst::bootstrap::InitialCalibrationGains(postproc_cfg));
    // Motorized-focus VCM driver + shared per-camera focus mode/position (U8/U9).
    // Must outlive the handlers + the AF loop below.
    sst::adapters::focus::I2cFocuser focuser;
    sst::focus::FocusState focus_state;
    // Latest pre-correction frame average — auto-white-balance reads it to compute
    // grey-world gains. Must outlive the postprocessor (moved into the pipeline).
    sst::processing::FrameColorStats frame_color_stats;
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
    // Mid-recording camera death (U3): fired exactly when a camera's Nth
    // consecutive watchdog restart completes and fails (the RECOVERING window
    // is the hold — origin decision "hold one window, then finalize"). Only a
    // RECORDING session is ended; Ready/Configured sessions merely stay gated.
    // FinalizeSession is CAS-claimed, so a race with app-stop/auto-stop
    // finalizes exactly once.
    pipeline.SetOnCameraDown([&session_manager](std::size_t camera_index) {
        spdlog::error("camera {} DOWN — finalizing active recording (camera failure)",
                      camera_index);
        if (session_manager.Phase() == sst::session::SessionPhase::kRecording) {
            session_manager.FinalizeSession(sst::session::SessionEndReason::kCameraFailure);
        }
    });

    // ── Autofocus loop (U6) ────────────────────────────────────────────
    // Owns its own low-rate thread (never the BLE dispatcher): hunts only
    // cameras the app flipped to AUTO (CameraFocus handler ↔ focus_state),
    // taps the producer path for sharpness samples, skips unhealthy cameras
    // (same U3 frame-truth derivation as the gates above), and pauses while a
    // recording session is active unless SST_AF_DURING_RECORDING=1 (R15 —
    // metal validation decides the default flip). On a fixed lens (focuser
    // unavailable) the loop idles entirely. Started after pipeline.Start()
    // below; declared after the pipeline so it is destroyed (joined) first.
    sst::focus::AutofocusService autofocus(
        focuser, focus_state, pipeline,
        [&pipeline](std::size_t camera_index) { return pipeline.CameraHealthStatus(camera_index); },
        [&recording_service] {
            return recording_service.CurrentState() != sst::storage::RecordingState::kIdle;
        });

    // ── System stats (telemetry source) ────────────────────────────────
    sst::adapters::control::ProcSystemStats system_stats(video_root);

    // ── Control plane: dispatcher + per-concern handlers ───────────────
    // One Register() per concern, grouped in bootstrap/control/control-plane.cpp
    // (the ★ extensibility point). MatchHandler comes back so the ~1 Hz
    // display-clock worker below can tick it.
    sst::control::CommandDispatcher dispatcher;
    sst::adapters::storage::OpenCvJpegEncoder jpeg_encoder;
    auto match_handler = sst::bootstrap::RegisterControlPlane(
        dispatcher, sst::bootstrap::ControlPlaneDeps{.device_cfg = cfg.device,
                                                     .uplink_cfg = cfg.uplink,
                                                     .system_stats = system_stats,
                                                     .telemetry_probe = telemetry_probe,
                                                     .session_manager = session_manager,
                                                     .overlay_controller = overlay_controller,
                                                     .overlay_timeline = overlay_timeline,
                                                     .recording_service = recording_service,
                                                     .streaming_service = streaming_service,
                                                     .uplink_probe = uplink_probe,
                                                     .raw_capture_sink = raw_capture_sink,
                                                     .proxy_lifecycle = proxy_lifecycle,
                                                     .wifi_manager = wifi_manager,
                                                     .network_configurator = network_configurator,
                                                     .dhcp_server = dhcp_server,
                                                     .uplink_manager = uplink_manager,
                                                     .uplink_store = uplink_store,
                                                     .download_server = download_server,
                                                     .export_manager = export_manager,
                                                     .pipeline = pipeline,
                                                     .jpeg_encoder = jpeg_encoder,
                                                     .manual_camera_state = manual_camera_state,
                                                     .preview_layout_state = preview_layout_state,
                                                     .calibration_state = calibration_state,
                                                     .frame_color_stats = frame_color_stats,
                                                     .focuser = focuser,
                                                     .focus_state = focus_state});

    // ── BLE transport ──────────────────────────────────────────────────
    // advertised_name computed above (shared with the WiFi-Direct SSID postfix).
    sst::adapters::control::BluezBleTransport ble_transport(advertised_name);
    ble_transport.SetOnCommand(
        [&dispatcher](const sst_cam::Command& cmd) { return dispatcher.Dispatch(cmd); });
    ble_transport.SetOnConnect([&session_manager] { session_manager.OnConnect(); });
    ble_transport.SetOnDisconnect([&session_manager] { session_manager.OnDisconnect(); });

    // Start() tolerates unprimed cameras (a cold nvargus-daemon boot begins
    // RECOVERING and the watchdog heals it); it fails only with no cameras
    // configured, which is a wiring bug worth aborting on.
    if (!pipeline.Start()) {
        spdlog::error("pipeline failed to start (no cameras configured) — aborting");
        return 1;
    }
    autofocus.Start();
    ble_transport.Start();
    if (!ble_transport.IsRunning()) {
        spdlog::error("BLE transport failed to start — aborting");
        autofocus.Stop();
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
    // cv-interruptible, so shutdown never waits out the tick interval.
    sst::common::PeriodicWorker match_clock(std::chrono::seconds(1),
                                            [&match_handler] { match_handler->TickClock(); });
    match_clock.Start();

    spdlog::info("startup complete — advertising as {}", advertised_name);

    // Run until terminated (SIGINT/SIGTERM). The shutdown sequence below
    // finalizes any still-active session explicitly (a disconnect no longer
    // does — sessions outlive connections).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        spdlog::error("sigprocmask failed — cannot guarantee clean shutdown; aborting");
        match_clock.Stop();
        http_download_server.Stop();
        ble_transport.Stop();
        session_manager.FinalizeSession(sst::session::SessionEndReason::kAppStop);
        autofocus.Stop();
        pipeline.Stop();
        return 1;
    }
    int signo = 0;
    sigwait(&mask, &signo);
    spdlog::info("signal {} received — shutting down", signo);

    match_clock.Stop();
    proxy_retention_sweep.Stop();
    telemetry_probe.Stop();
    http_download_server.Stop();
    ble_transport.Stop();
    // End any still-active session BEFORE the pipeline stops feeding the
    // recorder, so the MP4 is EOSed with its last frames (a disconnect no
    // longer finalizes — sessions outlive connections).
    session_manager.FinalizeSession(sst::session::SessionEndReason::kAppStop);
    // Stop the AF loop before the pipeline: its sample grabs tap the producer
    // threads the pipeline is about to join.
    autofocus.Stop();
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
