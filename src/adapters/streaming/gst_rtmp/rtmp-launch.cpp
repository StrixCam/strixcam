#include "adapters/streaming/gst_rtmp/rtmp-launch.hpp"

#include <fmt/format.h>

#include <cstdlib>

#include "adapters/storage/gstreamer/encode-fragment.hpp"
#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::streaming {

auto BuildRtmpLocation(const sst::streaming::PlatformStreamConfig& cfg) -> std::string {
    // The app contract inlines the stream key into the URL and sends no separate
    // key, so an empty stream_key means `url` is already the complete ingest
    // location — use it verbatim. Appending "/" here would push to "<url>/",
    // a DIFFERENT stream name on the ingest server (e.g. nginx-rtmp sees the
    // trailing-slash path as another stream).
    if (cfg.stream_key.empty()) {
        return cfg.url;
    }
    if (cfg.url.empty()) {
        return cfg.stream_key;
    }
    if (cfg.url.back() == '/') {
        return cfg.url + cfg.stream_key;
    }
    return cfg.url + "/" + cfg.stream_key;
}

auto BuildRtmpLaunch(const sst::streaming::PlatformStreamConfig& cfg, bool use_vic) -> std::string {
    // Convert-scale + leaky-queue + x264enc come from the shared encode fragment
    // (encode-fragment.hpp): appsrc source is packed BGR, the per-branch scale
    // conforms it to the app-requested stream resolution/fps (independent of the
    // record and raw branches), I420 is pinned before x264enc, and the
    // drop-oldest pre-encoder queue bounds the backlog when the software encode
    // falls behind realtime (no NVENC).
    // Stream runs the QUALITY profile like the recorder: drop tune=zerolatency,
    // add B-frames + lookahead, deepen the pre-encode queue in time. SST_STREAM_
    // BITRATE_KBPS mirrors the recorder's SST_REC_BITRATE_KBPS so stream bitrate
    // is dialable on metal without a rebuild; when unset cfg.bitrate_kbps carries
    // the raised kDefaultBitrateKbps (StreamingHandler never sets it from proto).
    const char* stream_bitrate_env = std::getenv("SST_STREAM_BITRATE_KBPS");
    const int bitrate_kbps =
        (stream_bitrate_env != nullptr) ? std::atoi(stream_bitrate_env) : cfg.bitrate_kbps;

    const char* queue_ms_env = std::getenv("SST_STREAM_QUEUE_MS");
    const int queue_max_time_ms =
        (queue_ms_env != nullptr) ? std::atoi(queue_ms_env) : kRtmpPreEncodeQueueMaxTimeMs;

    const std::string fragment = sst::adapters::storage::BuildEncodeFragment({
        .input = sst::adapters::storage::EncodeInput::kBgr,
        .target = sst::common::VideoQuality{cfg.width, cfg.height, cfg.framerate},
        .framerate = cfg.framerate,
        .use_vic = use_vic,
        .default_preset = "superfast",
        .bitrate_kbps = bitrate_kbps,
        .queue_max_buffers = kRtmpPreEncodeQueueBuffers,
        .low_latency = false,
        .bframes = kRtmpBframes,
        .rc_lookahead = kRtmpRcLookahead,
        .queue_max_time_ms = queue_max_time_ms,
        .encoder_name = {},
    });

    // Software H.264 (the Orin Nano has no NVENC): x264enc reads system memory.
    // rtmp2sink replaces the deprecated rtmpsink — it takes a clean location
    // URL. The uplink queue is leaky-downstream + non-blocking so a stalled RTMP
    // socket can never back-pressure the capture/encode path. do-timestamp=true
    // on the appsrc supplies valid PTS (x264enc requires it). flvmux is named so
    // a silent AAC pad can attach (platforms like YouTube require an audio
    // track).
    return fmt::format(
        "flvmux name=mux streamable=true ! rtmp2sink location=\"{loc}\" sync=false "
        "appsrc name={src} is-live=true format=time do-timestamp=true "
        " ! {frag} "
        " ! h264parse config-interval=-1 "
        " ! queue leaky=downstream max-size-buffers=3 ! mux.video "
        // Silent AAC track — YouTube et al. reject video-only FLV. Both pads must
        // produce timestamped buffers or flvmux stalls (is-live + do-timestamp).
        "audiotestsrc is-live=true wave=silence do-timestamp=true "
        " ! audioconvert ! voaacenc ! aacparse ! queue ! mux.audio ",
        fmt::arg("src", kRtmpAppsrcName), fmt::arg("frag", fragment),
        fmt::arg("loc", BuildRtmpLocation(cfg)));
}

}  // namespace sst::adapters::streaming
