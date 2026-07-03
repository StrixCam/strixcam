#include "adapters/raw_capture/proxy-launch.hpp"

#include <fmt/format.h>

#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::raw_capture {

auto BuildProxyLaunch(const std::string& output_mp4, bool use_vic) -> std::string {
    const auto& mode = sst::common::kProxyVideoMode;
    const int key_int_max = mode.fps * 2;

    // NV12 source -> scale to the proxy mode + I420 for x264enc. VIC path uses
    // nvvidconv directly (NV12 is VIC-native); software path uses videoconvert +
    // videoscale. videorate (fps) stays software either way.
    const std::string scale =
        use_vic ? fmt::format("nvvidconv ! video/x-raw,format=I420,width={w},height={h} "
                              "! videorate ! video/x-raw,framerate={fps}/1",
                              fmt::arg("w", mode.width), fmt::arg("h", mode.height),
                              fmt::arg("fps", mode.fps))
                : fmt::format("videoconvert ! videoscale ! videorate "
                              "! video/x-raw,format=I420,width={w},height={h},framerate={fps}/1",
                              fmt::arg("w", mode.width), fmt::arg("h", mode.height),
                              fmt::arg("fps", mode.fps));

    return fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true block=false ! "
        "{scale} ! "
        "queue leaky=downstream max-size-buffers={qbuf} max-size-time=0 max-size-bytes=0 ! "
        "x264enc name={enc} speed-preset=ultrafast tune=zerolatency "
        "bitrate={kbps} key-int-max={gik} ! "
        "h264parse config-interval=-1 ! mp4mux ! filesink location={loc}",
        fmt::arg("src", kProxyAppsrcName), fmt::arg("enc", kProxyEncoderName),
        fmt::arg("scale", scale), fmt::arg("qbuf", kProxyQueueMaxBuffers),
        fmt::arg("kbps", kProxyBitrateKbps), fmt::arg("gik", key_int_max),
        fmt::arg("loc", output_mp4));
}

}  // namespace sst::adapters::raw_capture
