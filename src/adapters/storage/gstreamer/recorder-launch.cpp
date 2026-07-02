#include "adapters/storage/gstreamer/recorder-launch.hpp"

#include <fmt/format.h>

namespace sst::adapters::storage {

auto BuildRecorderLaunch(const std::string& output_mp4, const sst::common::VideoQuality& quality,
                         bool use_vic) -> std::string {
    const int framerate = quality.IsSet() ? quality.fps : kRecorderDefaultFramerate;
    const int key_int_max = framerate * 2;

    // Scale + colour-convert (NV12/BGR → I420) is the expensive per-frame CPU
    // cost on this branch. `use_vic` moves it onto the Jetson VIC (`nvvidconv`,
    // hardware) so the CPU is freed for the software x264 encoders; `nvvidconv`
    // does NOT resample framerate, so `videorate` (cheap) stays in software. The
    // software path (`videoconvert ! videoscale`) is the proven default and the
    // fallback when VIC caps fail to negotiate on-device. Either way the frame
    // reaches x264enc as system-memory I420 (x264enc is a sysmem consumer; VIC
    // does the sysmem→VIC→sysmem hop internally). See U2 in the capture-transfer
    // plan — VIC frees CPU but does NOT raise the encode ceiling.
    std::string format_caps;
    if (use_vic) {
        format_caps =
            quality.IsSet()
                ? fmt::format(
                      "nvvidconv ! video/x-raw,format=I420,width={w},height={h} ! "
                      "videorate ! video/x-raw,framerate={fps}/1",
                      fmt::arg("w", quality.width), fmt::arg("h", quality.height),
                      fmt::arg("fps", quality.fps))
                : std::string{"nvvidconv ! video/x-raw,format=I420"};
    } else {
        format_caps =
            quality.IsSet()
                ? fmt::format(
                      "videoconvert ! videoscale ! videorate ! "
                      "video/x-raw,format=I420,width={w},height={h},framerate={fps}/1",
                      fmt::arg("w", quality.width), fmt::arg("h", quality.height),
                      fmt::arg("fps", quality.fps))
                : std::string{"videoconvert ! video/x-raw,format=I420"};
    }

    // A leaky (drop-oldest) queue sits between the scaler and the software
    // encoder so a transient encode overload drops frames instead of backing up
    // the appsrc unbounded — otherwise a slow encode (e.g. under full-pipeline
    // load) leaves a huge undrained backlog at Stop, the finalize wait times out,
    // and mp4mux never writes a valid moov (an unplayable "no playable streams"
    // file). Bounded backlog keeps the encoder realtime and the MP4 always
    // finalizes; the cost is dropped frames under overload, never a corrupt file.
    return fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true ! "
        "{caps} ! "
        "queue leaky=downstream max-size-buffers={qbuf} max-size-time=0 max-size-bytes=0 ! "
        "x264enc name={enc} speed-preset=ultrafast tune=zerolatency "
        "bitrate={kbps} key-int-max={gik} ! "
        "h264parse config-interval=-1 ! mp4mux ! filesink location={loc}",
        fmt::arg("src", kRecorderAppsrcName), fmt::arg("enc", kRecorderEncoderName),
        fmt::arg("caps", format_caps), fmt::arg("qbuf", kRecorderQueueMaxBuffers),
        fmt::arg("kbps", kRecorderBitrateKbps), fmt::arg("gik", key_int_max),
        fmt::arg("loc", output_mp4));
}

}  // namespace sst::adapters::storage
