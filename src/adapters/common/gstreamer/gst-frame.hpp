#pragma once

#include <gst/gst.h>

#include <cstddef>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"

// Frame → GStreamer plumbing shared by the output adapters (recorder, RTMP,
// RTSP, raw-capture proxy). Each previously carried its own copy of these.
namespace sst::adapters::gst_common {

// Caps `format=` string for a Frame's pixel format. `fallback` preserves each
// call site's historical default for a format outside the enum (unreachable
// today — the switch covers every PixelFormat the pipeline produces).
auto GstFormatFor(sst::common::PixelFormat fmt, const char* fallback) -> const char*;

// Sum of all plane byte sizes.
auto FrameByteSize(const sst::capture::Frame& frame) -> std::size_t;

// Allocate a GstBuffer of the frame's total size and copy every plane into it
// contiguously. Returns nullptr when the frame is empty or alloc/map fails.
// Timestamps are left unset: the do-timestamp=true appsrc stamps a valid
// running-time PTS (forcing GST_CLOCK_TIME_NONE corrupts the x264enc stream;
// nvv4l2h264enc tolerated it, x264enc does not).
auto MakeGstBufferFromFrame(const sst::capture::Frame& frame) -> GstBuffer*;

}  // namespace sst::adapters::gst_common
