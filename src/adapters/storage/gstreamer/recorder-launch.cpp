#include "adapters/storage/gstreamer/recorder-launch.hpp"

#include <fmt/format.h>

namespace sst::adapters::storage {

auto BuildRecorderLaunch(const std::string& output_mp4, const sst::common::VideoQuality& quality,
                         bool use_vic) -> std::string {
    const int framerate = quality.IsSet() ? quality.fps : kRecorderDefaultFramerate;
    const int key_int_max = framerate * 2;

    // Scale + BGR→I420 colour-convert is the expensive per-frame CPU cost on this
    // branch. `use_vic` moves the heavy part onto the Jetson VIC (`nvvidconv`,
    // hardware): the appsrc source is packed BGR (postprocess output), which
    // nvvidconv does NOT accept directly (verified on JP7.2 — a bare
    // `BGR ! nvvidconv` errors), so a cheap software `videoconvert` repacks
    // BGR→BGRx (24→32-bit, no colour math) and VIC then does the BGRx→I420
    // convert + scale. That frees the YUV matrix + resample (the bulk) to
    // hardware while only a light repack stays on CPU. `videorate` (cheap) stays
    // software — nvvidconv does not resample framerate. The full-software path
    // (`videoconvert ! videoscale`, BGR→I420 on CPU) is the proven fallback. Set
    // SST_DISABLE_VIC=1 to force it at runtime without a rebuild. Either way
    // x264enc receives system-memory I420. VIC frees CPU but does NOT raise the
    // encode ceiling (see U2).
    std::string format_caps;
    if (use_vic) {
        format_caps =
            quality.IsSet()
                ? fmt::format(
                      "videoconvert ! video/x-raw,format=BGRx ! "
                      "nvvidconv ! video/x-raw,format=I420,width={w},height={h} ! "
                      "videorate ! video/x-raw,framerate={fps}/1",
                      fmt::arg("w", quality.width), fmt::arg("h", quality.height),
                      fmt::arg("fps", quality.fps))
                : std::string{
                      "videoconvert ! video/x-raw,format=BGRx ! "
                      "nvvidconv ! video/x-raw,format=I420"};
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
