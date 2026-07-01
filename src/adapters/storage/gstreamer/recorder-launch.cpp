#include "adapters/storage/gstreamer/recorder-launch.hpp"

#include <fmt/format.h>

namespace sst::adapters::storage {

auto BuildRecorderLaunch(const std::string& output_mp4,
                         const sst::common::VideoQuality& quality) -> std::string {
    const int framerate = quality.IsSet() ? quality.fps : kRecorderDefaultFramerate;
    const int key_int_max = framerate * 2;

    // Per-branch scaling only when the app pinned a mode; otherwise keep the
    // source resolution/fps and just force I420. videoscale+videorate resample
    // the postprocess-output frame to the requested record resolution/fps.
    const std::string format_caps =
        quality.IsSet()
            ? fmt::format(
                  "videoscale ! videorate ! video/x-raw,format=I420,width={w},height={h},"
                  "framerate={fps}/1",
                  fmt::arg("w", quality.width), fmt::arg("h", quality.height),
                  fmt::arg("fps", quality.fps))
            : std::string{"video/x-raw,format=I420"};

    return fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true ! "
        "videoconvert ! {caps} ! "
        "x264enc name={enc} speed-preset=ultrafast tune=zerolatency "
        "bitrate={kbps} key-int-max={gik} ! "
        "h264parse config-interval=-1 ! mp4mux ! filesink location={loc}",
        fmt::arg("src", kRecorderAppsrcName), fmt::arg("enc", kRecorderEncoderName),
        fmt::arg("caps", format_caps), fmt::arg("kbps", kRecorderBitrateKbps),
        fmt::arg("gik", key_int_max), fmt::arg("loc", output_mp4));
}

}  // namespace sst::adapters::storage
