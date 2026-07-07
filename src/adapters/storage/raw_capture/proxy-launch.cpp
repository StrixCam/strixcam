#include "adapters/storage/raw_capture/proxy-launch.hpp"

#include <fmt/format.h>

#include "adapters/storage/gstreamer/encode-fragment.hpp"
#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::storage {

auto BuildProxyLaunch(const std::string& output_mp4, bool use_vic) -> std::string {
    // NV12 source -> scale to the internal proxy mode + I420 for x264enc, via
    // the shared encode fragment (encode-fragment.hpp). NV12 is VIC-native, so
    // the VIC path uses nvvidconv directly (no BGRx repack, unlike the
    // record/RTMP branches whose source is packed BGR).
    const std::string fragment = sst::adapters::storage::BuildEncodeFragment({
        .input = sst::adapters::storage::EncodeInput::kNv12,
        .target = sst::common::kProxyVideoMode,
        .framerate = sst::common::kProxyVideoMode.fps,
        .use_vic = use_vic,
        .default_preset = "ultrafast",
        .bitrate_kbps = kProxyBitrateKbps,
        .queue_max_buffers = kProxyQueueMaxBuffers,
        .encoder_name = kProxyEncoderName,
    });

    return fmt::format(
        "appsrc name={src} is-live=true format=time do-timestamp=true block=false ! "
        "{frag} ! "
        "h264parse config-interval=-1 ! mp4mux ! filesink location={loc}",
        fmt::arg("src", kProxyAppsrcName), fmt::arg("frag", fragment), fmt::arg("loc", output_mp4));
}

}  // namespace sst::adapters::storage
