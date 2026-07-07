#pragma once

#include <string>
#include <string_view>

#include "domain/common/models/video-quality.hpp"

namespace sst::adapters::storage {

// Which raw format the consumer's appsrc feeds into the encode fragment.
// - kBgr: postprocess output (recorder, RTMP egress, RTSP preview). nvvidconv
//   does NOT accept packed BGR directly (verified on JP7.2), so the VIC path
//   first repacks BGR->BGRx with a cheap software videoconvert (24->32 bit, no
//   colour math) and VIC then does the heavy BGRx->I420 convert + scale.
// - kNv12: raw camera frames (training proxy). NV12 is VIC-native, so the VIC
//   path uses nvvidconv directly with no repack.
enum class EncodeInput : std::uint8_t { kBgr, kNv12 };

// Fallbacks for fragment params a caller leaves unset (tests, prototyping) —
// every production consumer passes its own validated values explicitly.
inline constexpr int kFragmentDefaultFramerate = 30;
inline constexpr int kFragmentDefaultBitrateKbps = 4000;
inline constexpr int kFragmentDefaultQueueMaxBuffers = 30;

// Parameters for the shared convert-scale + leaky-queue + x264enc fragment.
// Per-consumer defaults stay with the consumer (recorder: superfast/14 Mbps,
// RTMP/RTSP/proxy: ultrafast at their own bitrates/queue depths); the fragment
// SHAPE and the env-override handling live here, once.
struct EncodeFragmentParams {
    EncodeInput input{EncodeInput::kBgr};

    // When IsSet(), a scale + framerate conform to this mode is inserted
    // (videoscale software / nvvidconv VIC; videorate is always software — the
    // VIC does not resample framerate). Unset keeps the source geometry and
    // only colour-converts to I420.
    sst::common::VideoQuality target{};

    // Effective output fps when `target` is unset (key-int-max = 2x fps so a
    // decoder can join/seek within ~2 s). Ignored when target.IsSet() — the
    // target fps wins.
    int framerate{kFragmentDefaultFramerate};

    // true offloads scale + colour-convert to the Jetson VIC (nvvidconv).
    // Consumers derive this from the SST_DISABLE_VIC escape hatch at pipeline
    // build time; the proven full-software path is the fallback.
    bool use_vic{false};

    // x264 speed preset when SST_X264_PRESET is not set. A slower preset costs
    // CPU on the one shared software encoder (no NVENC), so each consumer keeps
    // its validated default.
    std::string_view default_preset{"ultrafast"};

    int bitrate_kbps{kFragmentDefaultBitrateKbps};

    // Bound on the mandatory leaky (drop-oldest) pre-encoder queue. A software
    // encode that falls behind realtime drops frames instead of backing up the
    // is-live appsrc unbounded — otherwise the finalize wait times out and
    // mp4mux never writes a valid moov (see
    // docs/solutions/tooling-decisions/software-h264-encode-ceiling-no-nvenc-2026-07-01.md).
    int queue_max_buffers{kFragmentDefaultQueueMaxBuffers};

    // Non-empty names the x264enc element (name=<x>) so the consumer can look
    // it up after gst_parse_launch. Empty leaves it unnamed.
    std::string_view encoder_name;
};

// Builds the shared "convert-scale ! leaky queue ! x264enc" launch fragment all
// four software-encode consumers (recorder, RTMP, RTSP preview, training proxy)
// splice between their appsrc and their parse/mux/sink tail. Pure (no element
// instantiation) so the shape is unit-testable without hardware.
//
// Env overrides (uniform across every consumer):
// - SST_X264_PRESET overrides the consumer's default x264 speed preset. One
//   knob for the one shared CPU encoder pool — dialable on-device without a
//   rebuild.
// (SST_REC_BITRATE_KBPS stays recorder-only: the other consumers' bitrates are
// app/config-supplied, not firmware defaults.)
//
// x264enc always runs tune=zerolatency (no reorder buffer to drain at stop) and
// receives system-memory I420 — 4:2:0 is pinned before the encoder because BGR
// input otherwise encodes High 4:4:4, which most decoders reject.
[[nodiscard]] auto BuildEncodeFragment(const EncodeFragmentParams& params) -> std::string;

}  // namespace sst::adapters::storage
