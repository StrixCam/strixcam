#include "adapters/streaming/gst_rtmp/rtmp-launch.hpp"

#include <fmt/format.h>

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

auto BuildRtmpLaunch(const sst::streaming::PlatformStreamConfig& cfg) -> std::string {
    // Software H.264 (the Orin Nano has no NVENC): x264enc reads system memory,
    // so the nvvidconv/NVMM hop is dropped. rtmp2sink replaces the deprecated
    // rtmpsink — it takes a clean location URL. The uplink queue is leaky-
    // downstream + non-blocking so a stalled RTMP socket can never back-pressure
    // the capture/encode path. x264enc bitrate is in kbit/s. do-timestamp=true on
    // the appsrc supplies valid PTS (x264enc requires it). videoscale/videorate
    // conform the source frame to the requested stream resolution/fps. flvmux is
    // named so a silent AAC pad can attach (platforms like YouTube require an
    // audio track).
    return fmt::format(
        "flvmux name=mux streamable=true ! rtmp2sink location=\"{loc}\" sync=false "
        "appsrc name={src} is-live=true format=time do-timestamp=true "
        " ! videoconvert ! videoscale ! videorate "
        " ! video/x-raw,width={w},height={h},framerate={fps}/1 "
        " ! x264enc speed-preset=ultrafast tune=zerolatency bitrate={brk} key-int-max={gik} "
        " ! h264parse config-interval=-1 "
        " ! queue leaky=downstream max-size-buffers=3 ! mux.video "
        // Silent AAC track — YouTube et al. reject video-only FLV. Both pads must
        // produce timestamped buffers or flvmux stalls (is-live + do-timestamp).
        "audiotestsrc is-live=true wave=silence do-timestamp=true "
        " ! audioconvert ! voaacenc ! aacparse ! queue ! mux.audio ",
        fmt::arg("src", kRtmpAppsrcName), fmt::arg("w", cfg.width), fmt::arg("h", cfg.height),
        fmt::arg("fps", cfg.framerate), fmt::arg("brk", cfg.bitrate_kbps),
        fmt::arg("gik", cfg.framerate * 2), fmt::arg("loc", BuildRtmpLocation(cfg)));
}

}  // namespace sst::adapters::streaming
