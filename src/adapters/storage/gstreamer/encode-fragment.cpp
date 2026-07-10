#include "adapters/storage/gstreamer/encode-fragment.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <string>

namespace sst::adapters::storage {

namespace {

// The convert + (optional) scale head of the fragment. VIC (`nvvidconv`,
// hardware) takes the heavy YUV matrix + resample; the software fallback is the
// proven `videoconvert ! videoscale`. `videorate` (cheap) stays software either
// way — the VIC does not resample framerate.
auto BuildConvertScale(const EncodeFragmentParams& params) -> std::string {
    if (params.use_vic) {
        // Packed BGR needs a cheap software repack to BGRx before nvvidconv
        // (a bare `BGR ! nvvidconv` errors on JP7.2); NV12 is VIC-native.
        const std::string vic_head = (params.input == EncodeInput::kBgr)
                                         ? "videoconvert ! video/x-raw,format=BGRx ! nvvidconv"
                                         : "nvvidconv";
        if (!params.target.IsSet()) {
            return fmt::format("{head} ! video/x-raw,format=I420", fmt::arg("head", vic_head));
        }
        return fmt::format(
            "{head} ! video/x-raw,format=I420,width={w},height={h} ! "
            "videorate ! video/x-raw,framerate={fps}/1",
            fmt::arg("head", vic_head), fmt::arg("w", params.target.width),
            fmt::arg("h", params.target.height), fmt::arg("fps", params.target.fps));
    }
    if (!params.target.IsSet()) {
        return "videoconvert ! video/x-raw,format=I420";
    }
    return fmt::format(
        "videoconvert ! videoscale ! videorate ! "
        "video/x-raw,format=I420,width={w},height={h},framerate={fps}/1",
        fmt::arg("w", params.target.width), fmt::arg("h", params.target.height),
        fmt::arg("fps", params.target.fps));
}

}  // namespace

auto BuildEncodeFragment(const EncodeFragmentParams& params) -> std::string {
    // Uniform env override: one preset knob for the one shared CPU encoder pool
    // (per-consumer defaults otherwise). See header for rationale.
    const char* preset_env = std::getenv("SST_X264_PRESET");
    const std::string_view preset =
        (preset_env != nullptr) ? std::string_view{preset_env} : params.default_preset;

    const int effective_fps = params.target.IsSet() ? params.target.fps : params.framerate;
    const int key_int_max = effective_fps * 2;

    const std::string encoder_name =
        params.encoder_name.empty() ? std::string{} : fmt::format("name={} ", params.encoder_name);

    // Encode profile. Low-latency (default) keeps tune=zerolatency — no reorder
    // buffer, the mandatory preview/proxy behavior. Quality drops zerolatency and
    // opts into B-frames + lookahead (b-adapt=1 lets x264 place them
    // adaptively) — the quality-per-bit levers the record/stream paths pay for
    // with output latency.
    const std::string profile =
        params.low_latency
            ? std::string{"tune=zerolatency"}
            : fmt::format("bframes={bf} b-adapt=1 rc-lookahead={rc}",
                          fmt::arg("bf", params.bframes), fmt::arg("rc", params.rc_lookahead));

    // Leaky (drop-oldest) pre-encoder queue is mandatory on every profile (moov
    // safety). queue_max_time_ms>0 additionally deepens it in TIME so brief
    // sub-realtime dips buffer rather than drop; leaky stays the sustained-
    // overload backstop. 0 keeps the byte-for-byte buffer-count-only queue.
    const int64_t queue_max_time_ns = static_cast<int64_t>(params.queue_max_time_ms) * 1'000'000;

    return fmt::format(
        "{cs} ! "
        "queue leaky=downstream max-size-buffers={qbuf} max-size-time={qtime} max-size-bytes=0 ! "
        "x264enc {name}speed-preset={preset} {profile} bitrate={kbps} key-int-max={gik}",
        fmt::arg("cs", BuildConvertScale(params)), fmt::arg("qbuf", params.queue_max_buffers),
        fmt::arg("qtime", queue_max_time_ns), fmt::arg("name", encoder_name),
        fmt::arg("preset", preset), fmt::arg("profile", profile),
        fmt::arg("kbps", params.bitrate_kbps), fmt::arg("gik", key_int_max));
}

}  // namespace sst::adapters::storage
