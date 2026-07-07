// Shared encode-fragment builder (state-health cycle U5): the ONE
// convert-scale + leaky-queue + x264enc fragment all four software-encode
// consumers (recorder, RTMP, RTSP preview, training proxy) splice into their
// launch strings. Pure string-level assertions — no GStreamer elements, no
// hardware.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "adapters/storage/gstreamer/encode-fragment.hpp"
#include "domain/common/models/video-quality.hpp"

namespace {

using sst::adapters::storage::BuildEncodeFragment;
using sst::adapters::storage::EncodeFragmentParams;
using sst::adapters::storage::EncodeInput;

auto Contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

// NOLINTBEGIN(readability-magic-numbers)
// Literals are self-evident synthetic encode parameters (resolutions, fps,
// bitrates, queue depths); naming each would obscure the fragment shapes.

// Fully-initialized baseline the tests mutate per scenario.
auto Params() -> EncodeFragmentParams {
    return {.input = EncodeInput::kBgr,
            .target = {},
            .framerate = 30,
            .use_vic = false,
            .default_preset = "ultrafast",
            .bitrate_kbps = 4000,
            .queue_max_buffers = 30,
            .encoder_name = {}};
}

// BGR source, software path, scale target: the proven videoconvert !
// videoscale ! videorate chain with I420 pinned for the encoder.
TEST(EncodeFragmentTest, BgrSoftwareWithTargetUsesVideoscale) {
    auto params = Params();
    params.target = {1920, 1080, 30};
    params.default_preset = "superfast";
    params.bitrate_kbps = 14000;
    const auto frag = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(frag, "videoconvert ! videoscale ! videorate"));
    EXPECT_TRUE(Contains(frag, "format=I420,width=1920,height=1080,framerate=30/1"));
    EXPECT_FALSE(Contains(frag, "nvvidconv"));
    EXPECT_TRUE(Contains(frag, "x264enc speed-preset=superfast tune=zerolatency"));
    EXPECT_TRUE(Contains(frag, "bitrate=14000"));
    EXPECT_TRUE(Contains(frag, "key-int-max=60"));  // 2x target fps
}

// BGR source, VIC path: cheap software BGR->BGRx repack (nvvidconv rejects
// packed BGR), then the heavy convert + scale on VIC; videorate stays software.
TEST(EncodeFragmentTest, BgrVicWithTargetRepacksToBgrxAndDropsVideoscale) {
    auto params = Params();
    params.target = {1280, 720, 60};
    params.use_vic = true;
    const auto frag = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(frag, "videoconvert ! video/x-raw,format=BGRx ! nvvidconv"));
    EXPECT_TRUE(Contains(frag, "format=I420,width=1280,height=720"));
    EXPECT_TRUE(Contains(frag, "videorate ! video/x-raw,framerate=60/1"));
    EXPECT_FALSE(Contains(frag, "videoscale"));  // scale moved to VIC
    EXPECT_TRUE(Contains(frag, "key-int-max=120"));
}

// No scale target (consumer's appsrc caps already carry the geometry — RTSP,
// or unset record quality): colour-convert only, no videoscale/videorate.
TEST(EncodeFragmentTest, BgrNoTargetConvertsOnly) {
    auto params = Params();
    params.bitrate_kbps = 1500;
    params.queue_max_buffers = 2;
    const auto software = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(software, "videoconvert ! video/x-raw,format=I420 ! queue"));
    EXPECT_FALSE(Contains(software, "videoscale"));
    EXPECT_FALSE(Contains(software, "videorate"));
    EXPECT_TRUE(Contains(software, "key-int-max=60"));  // 2x explicit framerate

    params.use_vic = true;
    const auto vic = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(vic,
                         "videoconvert ! video/x-raw,format=BGRx ! nvvidconv ! "
                         "video/x-raw,format=I420 ! queue"));
    EXPECT_FALSE(Contains(vic, "videoscale"));
}

// NV12 source (training proxy) is VIC-native: nvvidconv directly, no BGRx
// repack; the software fallback is the same videoconvert ! videoscale chain.
TEST(EncodeFragmentTest, Nv12VicUsesNvvidconvDirectly) {
    auto params = Params();
    params.input = EncodeInput::kNv12;
    params.target = {854, 480, 15};
    params.use_vic = true;
    params.bitrate_kbps = 1500;
    const auto vic = BuildEncodeFragment(params);
    EXPECT_FALSE(Contains(vic, "BGRx"));
    EXPECT_FALSE(Contains(vic, "videoconvert"));
    EXPECT_TRUE(Contains(vic, "nvvidconv ! video/x-raw,format=I420,width=854,height=480"));
    EXPECT_TRUE(Contains(vic, "videorate ! video/x-raw,framerate=15/1"));
    EXPECT_TRUE(Contains(vic, "key-int-max=30"));

    params.use_vic = false;
    const auto software = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(software, "videoconvert ! videoscale ! videorate"));
    EXPECT_FALSE(Contains(software, "nvvidconv"));
}

// The leaky (drop-oldest) pre-encoder queue is MANDATORY in every fragment —
// a software encode that falls behind drops frames, never corrupts a moov.
TEST(EncodeFragmentTest, LeakyPreEncoderQueueAlwaysPresent) {
    for (const bool use_vic : {false, true}) {
        for (const auto input : {EncodeInput::kBgr, EncodeInput::kNv12}) {
            auto params = Params();
            params.input = input;
            params.target = {1280, 720, 30};
            params.use_vic = use_vic;
            params.queue_max_buffers = 17;
            const auto frag = BuildEncodeFragment(params);
            EXPECT_TRUE(Contains(
                frag,
                "queue leaky=downstream max-size-buffers=17 max-size-time=0 max-size-bytes=0"))
                << frag;
            EXPECT_TRUE(Contains(frag, "tune=zerolatency")) << frag;
        }
    }
}

// A named encoder gets `name=` for post-parse lookup; empty stays unnamed.
TEST(EncodeFragmentTest, EncoderNameOptional) {
    auto params = Params();
    params.encoder_name = "enc";
    const auto named = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(named, "x264enc name=enc speed-preset="));

    params.encoder_name = {};
    const auto unnamed = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(unnamed, "x264enc speed-preset="));
    EXPECT_FALSE(Contains(unnamed, "x264enc name="));
}

// SST_X264_PRESET overrides the per-consumer default preset UNIFORMLY — one
// knob for the one shared CPU encoder pool.
TEST(EncodeFragmentTest, EnvPresetOverrideIsUniform) {
    auto params = Params();
    params.default_preset = "superfast";
    setenv("SST_X264_PRESET", "veryfast", /*overwrite=*/1);
    const auto overridden = BuildEncodeFragment(params);
    unsetenv("SST_X264_PRESET");
    EXPECT_TRUE(Contains(overridden, "speed-preset=veryfast"));

    const auto defaulted = BuildEncodeFragment(params);
    EXPECT_TRUE(Contains(defaulted, "speed-preset=superfast"));
}

// NOLINTEND(readability-magic-numbers)

}  // namespace
