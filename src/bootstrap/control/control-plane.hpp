#pragma once

#include <memory>

#include "app/control/ports/dhcp-server.hpp"
#include "app/control/ports/network-configurator.hpp"
#include "app/control/ports/system-stats.hpp"
#include "app/control/ports/wifi-manager.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/match.handler.hpp"
#include "app/control/services/telemetry_probe/telemetry-probe.hpp"
#include "app/export/services/export-job-manager.hpp"
#include "app/focus/ports/focuser.hpp"
#include "app/network/ports/uplink-store.hpp"
#include "app/network/services/download_server/download-server.hpp"
#include "app/network/services/uplink-manager/uplink-manager.hpp"
#include "app/overlay/ports/overlay-timeline-recorder.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/pipeline/services/orchestrator/pipeline-orchestrator.hpp"
#include "app/session/ports/session-manager.hpp"
#include "app/storage/ports/jpeg-encoder.hpp"
#include "app/storage/ports/recording-service.hpp"
#include "app/storage/services/proxy_lifecycle/proxy-lifecycle.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "app/streaming/ports/uplink-probe.hpp"
#include "domain/config/models/device.hpp"
#include "domain/config/models/uplink-config.hpp"
#include "domain/decision/models/manual-camera-state.hpp"
#include "domain/focus/models/focus-state.hpp"
#include "domain/processing/models/color-calibration-state.hpp"
#include "domain/processing/models/frame-color-stats.hpp"
#include "domain/streaming/models/preview-layout.hpp"

namespace sst::bootstrap {

// Everything the control plane wires against. References only — main() owns
// every object and each must outlive the dispatcher (i.e. the BLE transport
// that drives it).
struct ControlPlaneDeps {
    const sst::config::DeviceData& device_cfg;
    const sst::config::UplinkData& uplink_cfg;
    sst::control::ISystemStats& system_stats;
    sst::control::TelemetryProbe& telemetry_probe;
    sst::session::ISessionManager& session_manager;
    sst::overlay::OverlayController& overlay_controller;
    sst::overlay::IOverlayTimelineRecorder& overlay_timeline;
    sst::storage::IRecordingService& recording_service;
    sst::streaming::IStreamingService& streaming_service;
    sst::streaming::IUplinkProbe& uplink_probe;
    sst::storage::ProxyLifecycle& proxy_lifecycle;
    sst::control::IWifiManager& wifi_manager;
    sst::control::INetworkConfigurator& network_configurator;
    sst::control::IDhcpServer& dhcp_server;
    sst::network::UplinkManager& uplink_manager;
    sst::network::IUplinkStore& uplink_store;
    sst::network::DownloadServer& download_server;
    sst::exportjob::ExportJobManager& export_manager;
    sst::pipeline::PipelineOrchestrator& pipeline;
    sst::storage::IJpegEncoder& jpeg_encoder;
    sst::decision::ManualCameraState& manual_camera_state;
    sst::streaming::PreviewLayoutState& preview_layout_state;
    sst::processing::ColorCalibrationState& calibration_state;
    sst::processing::FrameColorStats& frame_color_stats;
    sst::focus::IFocuser& focuser;
    sst::focus::FocusState& focus_state;
};

// Registers every per-concern command handler on the dispatcher — one
// Register() per concern (the ★ extensibility point). The shared frame-truth
// health providers and the start-class health gate are derived here from the
// pipeline, so telemetry, the session snapshot and the start gates can never
// disagree about a camera's health (U3). Returns the MatchHandler so main()
// can drive its ~1 Hz display-clock tick.
auto RegisterControlPlane(sst::control::CommandDispatcher& dispatcher, const ControlPlaneDeps& deps)
    -> std::shared_ptr<sst::control::MatchHandler>;

}  // namespace sst::bootstrap
