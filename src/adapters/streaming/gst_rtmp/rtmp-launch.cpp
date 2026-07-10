#include "adapters/streaming/gst_rtmp/rtmp-launch.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <string>

namespace sst::adapters::streaming {

auto BuildRtmpLocation(const sst::streaming::PlatformStreamConfig& cfg) -> std::string {
    if (cfg.url.empty()) {
        return cfg.stream_key;
    }
    if (cfg.url.back() == '/') {
        return cfg.url + cfg.stream_key;
    }
    return cfg.url + "/" + cfg.stream_key;
}

auto BuildRtmpLaunch(const sst::streaming::PlatformStreamConfig& cfg, bool use_vic) -> std::string {
    // Scale + BGR→I420 colour-convert: full software (videoconvert ! videoscale,
    // proven default) or VIC-offloaded (U2). The appsrc source is packed BGR,
    // which nvvidconv rejects directly (JP7.2), so a cheap videoconvert repacks
    // BGR→BGRx and VIC does the BGRx→I420 convert + scale; videorate stays
    // software. x264enc consumes system-memory I420 in both. SST_DISABLE_VIC=1
    // forces software at runtime without a rebuild.
    const std::string convert_scale =
        use_vic ? fmt::format(
                      "videoconvert ! video/x-raw,format=BGRx "
                      "! nvvidconv ! video/x-raw,format=I420,width={w},height={h} "
                      "! videorate ! video/x-raw,framerate={fps}/1",
                      fmt::arg("w", cfg.width), fmt::arg("h", cfg.height),
                      fmt::arg("fps", cfg.framerate))
                : fmt::format(
                      "videoconvert ! videoscale ! videorate "
                      "! video/x-raw,format=I420,width={w},height={h},framerate={fps}/1",
                      fmt::arg("w", cfg.width), fmt::arg("h", cfg.height),
                      fmt::arg("fps", cfg.framerate));
    // Stream bitrate + preset are env-dialable on-device without a rebuild
    // (the handler doesn't set bitrate from proto, so cfg.bitrate_kbps carries the
    // raised kDefaultBitrateKbps). SST_X264_PRESET is the shared preset knob.
    const char* br_env = std::getenv("SST_STREAM_BITRATE_KBPS");
    const int bitrate = (br_env != nullptr) ? std::atoi(br_env) : cfg.bitrate_kbps;
    const char* preset_env = std::getenv("SST_X264_PRESET");
    const std::string preset = (preset_env != nullptr) ? preset_env : kRtmpDefaultPreset;

    // Software H.264 (the Orin Nano has no NVENC): x264enc reads system memory,
    // so the nvvidconv/NVMM hop is dropped. rtmp2sink replaces the deprecated
    // rtmpsink — it takes a clean location URL. The uplink queue is leaky-
    // downstream + non-blocking so a stalled RTMP socket can never back-pressure
    // the capture/encode path. x264enc bitrate is in kbit/s. do-timestamp=true on
    // the appsrc supplies valid PTS (x264enc requires it). videoscale/videorate
    // conform the source frame to the requested stream resolution/fps. flvmux is
    // named so a silent AAC pad can attach (platforms like YouTube require an
    // audio track).
    // format=I420 forces 4:2:0 chroma before x264enc (same as the recorder) —
    // without it BGR input can encode High 4:4:4, which most players reject. The
    // leaky=downstream queue between the scaler and the encoder bounds the
    // backlog: a software encode that falls behind realtime (no NVENC) drops
    // frames instead of accumulating raw ~MB frames in the is-live appsrc
    // unbounded. Mirrors the recorder's pre-encoder queue.
    return fmt::format(
        "flvmux name=mux streamable=true ! rtmp2sink location=\"{loc}\" sync=false "
        "appsrc name={src} is-live=true format=time do-timestamp=true "
        " ! {cs} "
        " ! queue leaky=downstream max-size-buffers={qbuf} max-size-time=0 max-size-bytes=0 "
        " ! x264enc speed-preset={preset} bframes={bf} b-adapt=1 rc-lookahead={rl} "
        "bitrate={brk} key-int-max={gik} "
        " ! h264parse config-interval=-1 "
        " ! queue leaky=downstream max-size-buffers=3 ! mux.video "
        // Silent AAC track — YouTube et al. reject video-only FLV. Both pads must
        // produce timestamped buffers or flvmux stalls (is-live + do-timestamp).
        "audiotestsrc is-live=true wave=silence do-timestamp=true "
        " ! audioconvert ! voaacenc ! aacparse ! queue ! mux.audio ",
        fmt::arg("src", kRtmpAppsrcName), fmt::arg("cs", convert_scale),
        fmt::arg("qbuf", kRtmpPreEncodeQueueBuffers), fmt::arg("preset", preset),
        fmt::arg("bf", kRtmpBframes), fmt::arg("rl", kRtmpRcLookahead), fmt::arg("brk", bitrate),
        fmt::arg("gik", cfg.framerate * 2), fmt::arg("loc", BuildRtmpLocation(cfg)));
}

}  // namespace sst::adapters::streaming
