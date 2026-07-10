#include "adapters/storage/gstreamer/recorder-launch.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <string>

#include "adapters/storage/gstreamer/encode-fragment.hpp"

namespace sst::adapters::storage {

auto BuildRecorderLaunch(const std::string& output_mp4, const sst::common::VideoQuality& quality,
                         bool use_vic) -> std::string {
    // Record-encode quality is env-tunable so it can be dialed on-device without
    // a rebuild — the "cheap camera" look is mostly x264 ultrafast starved at
    // 8 Mbps. Bitrate is a pure quality lever (no CPU cost). superfast is the
    // validated default preset: it holds 1080p30 under the full pipeline load
    // (2x capture + preview + stream) where veryfast+ drop frames, and looks
    // markedly better than ultrafast. The preset override (SST_X264_PRESET) and
    // the convert-scale / leaky-queue / x264enc shape — including the VIC
    // offload and the moov-protecting drop-oldest queue — live in the shared
    // encode fragment (encode-fragment.hpp).
    const char* bitrate_env = std::getenv("SST_REC_BITRATE_KBPS");
    const int bitrate_kbps =
        (bitrate_env != nullptr) ? std::atoi(bitrate_env) : kRecorderBitrateKbps;

    // Pre-encode buffer depth is dialable on metal (U6) without a rebuild.
    const char* queue_ms_env = std::getenv("SST_REC_QUEUE_MS");
    const int queue_max_time_ms =
        (queue_ms_env != nullptr) ? std::atoi(queue_ms_env) : kRecorderQueueMaxTimeMs;

    // Record runs the QUALITY profile: drop tune=zerolatency, add B-frames +
    // lookahead, and deepen the pre-encode queue in time to ride out brief dips.
    // The clean 1080p master is preserved upstream (the orchestrator pushes clean
    // frames to record_sink_ before compositing) — nothing here overlays.
    const std::string fragment = BuildEncodeFragment({
        .input = EncodeInput::kBgr,
        .target = quality,
        .framerate = kRecorderDefaultFramerate,
        .use_vic = use_vic,
        .default_preset = "superfast",
        .bitrate_kbps = bitrate_kbps,
        .queue_max_buffers = kRecorderQueueMaxBuffers,
        .low_latency = false,
        .bframes = kRecorderBframes,
        .rc_lookahead = kRecorderRcLookahead,
        .queue_max_time_ms = queue_max_time_ms,
        .encoder_name = kRecorderEncoderName,
    });

    return fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true ! "
        "{frag} ! "
        "h264parse config-interval=-1 ! mp4mux ! filesink location={loc}",
        fmt::arg("src", kRecorderAppsrcName), fmt::arg("frag", fragment),
        fmt::arg("loc", output_mp4));
}

}  // namespace sst::adapters::storage
