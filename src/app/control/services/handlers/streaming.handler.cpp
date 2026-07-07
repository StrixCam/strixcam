#include "app/control/services/handlers/streaming.handler.hpp"

#include <utility>

#include "app/control/services/handlers/quality-mapping.hpp"
#include "domain/streaming/models/platform-stream-config.hpp"

namespace sst::control {

StreamingHandler::StreamingHandler(sst::streaming::IStreamingService& streaming,
                                   sst::streaming::IUplinkProbe* uplink_probe,
                                   StartHealthGate health_gate)
    : streaming_(streaming), uplink_probe_(uplink_probe), health_gate_(std::move(health_gate)) {}

auto StreamingHandler::HandledCases() const -> std::vector<sst_cam::Command::PayloadCase> {
    return {sst_cam::Command::kStreamingControl, sst_cam::Command::kSetStreamingConfig};
}

auto StreamingHandler::Handle(const sst_cam::Command& cmd) -> sst_cam::CommandResponse {
    if (cmd.payload_case() == sst_cam::Command::kSetStreamingConfig) {
        return HandleSetConfig(cmd.set_streaming_config());
    }
    return HandleControl(cmd.streaming_control());
}

auto StreamingHandler::HandleControl(const sst_cam::StreamingControlCommand& cmd)
    -> sst_cam::CommandResponse {
    sst_cam::CommandResponse resp;

    if (cmd.action() == sst_cam::STREAMING_START) {
        // Health gate first (U3): don't open an egress that would stream a
        // stalled camera. STOP below is never gated.
        if (const auto reason = health_gate_.RejectReason()) {
            resp.set_status(sst_cam::ResponseStatus::DEVICE_INOPERABLE);
            resp.set_error_message("streaming start rejected: " + *reason);
            return resp;
        }
        std::string destination = cmd.destination();
        if (destination.empty()) {
            std::lock_guard lock(mtx_);
            destination = configured_rtmp_url_;
        }
        if (destination.empty()) {
            resp.set_status(sst_cam::ResponseStatus::ERROR);
            resp.set_error_message("streaming start: no destination provided or configured");
            return resp;
        }

        // Cloud streaming egresses over the camera's internet uplink (ethernet /
        // wifi-STA), never the link-local WiFi-Direct GO. With no uplink there is
        // no route to the cloud — fail clearly so the app points the user at
        // Settings -> Network rather than surfacing an opaque rtmp connect error.
        if (uplink_probe_ != nullptr && !uplink_probe_->HasInternetUplink()) {
            resp.set_status(sst_cam::ResponseStatus::ERROR);
            resp.set_error_message(
                "streaming start: no internet uplink — configure ethernet or wifi in "
                "Settings -> Network");
            return resp;
        }

        sst::streaming::PlatformStreamConfig config;
        config.stream_id = kEgressStreamId;
        config.name = "egress";
        config.type = sst::streaming::PlatformStreamType::kRtmp;
        config.url = destination;  // full RTMP URL (app supplies key inline)

        // Stream quality is independent of the record quality (e.g. record 1080p
        // while streaming 720p). Unset/unsupported → keep the config's default
        // resolution/fps. The per-branch scaler in the RTMP streamer conforms the
        // source frame to this.
        const auto quality = ResolveQuality(cmd.has_quality(), cmd.quality());
        if (quality.IsSet()) {
            config.width = quality.width;
            config.height = quality.height;
            config.framerate = quality.fps;
        }

        if (!streaming_.StartPlatformStream(config)) {
            resp.set_status(sst_cam::ResponseStatus::ERROR);
            resp.set_error_message("streaming start failed (already streaming or bad destination)");
            return resp;
        }
        resp.set_status(sst_cam::ResponseStatus::OK);
        return resp;
    }

    if (cmd.action() == sst_cam::STREAMING_STOP) {
        const bool stopped = streaming_.StopPlatformStream(kEgressStreamId);
        resp.set_status(stopped ? sst_cam::ResponseStatus::OK : sst_cam::ResponseStatus::ERROR);
        if (!stopped) {
            resp.set_error_message("no active stream to stop");
        }
        return resp;
    }

    resp.set_status(sst_cam::ResponseStatus::ERROR);
    resp.set_error_message("unknown streaming action");
    return resp;
}

auto StreamingHandler::HandleSetConfig(const sst_cam::SetStreamingConfigCommand& cmd)
    -> sst_cam::CommandResponse {
    {
        std::lock_guard lock(mtx_);
        // The custom RTMP URL is the fallback destination for a START with no
        // explicit destination. Platform stream keys are app-side concerns.
        configured_rtmp_url_ = cmd.config().custom_rtmp_url();
    }
    sst_cam::CommandResponse resp;
    resp.set_status(sst_cam::ResponseStatus::OK);
    return resp;
}

}  // namespace sst::control
