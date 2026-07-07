#include "adapters/common/gstreamer/gst-frame.hpp"

#include <cstring>

namespace sst::adapters::gst_common {

auto GstFormatFor(sst::common::PixelFormat fmt, const char* fallback) -> const char* {
    switch (fmt) {
        case sst::common::PixelFormat::BGR8:
            return "BGR";
        case sst::common::PixelFormat::RGB8:
            return "RGB";
        case sst::common::PixelFormat::BGRA8:
            return "BGRA";
        case sst::common::PixelFormat::RGBA8:
            return "RGBA";
        case sst::common::PixelFormat::GRAY8:
            return "GRAY8";
        case sst::common::PixelFormat::NV12:
            return "NV12";
        case sst::common::PixelFormat::I420:
            return "I420";
        case sst::common::PixelFormat::YUYV:
            return "YUY2";
    }
    return fallback;
}

auto FrameByteSize(const sst::capture::Frame& frame) -> std::size_t {
    std::size_t total = 0;
    for (const auto& plane : frame.planes) {
        total += plane.size;
    }
    return total;
}

auto MakeGstBufferFromFrame(const sst::capture::Frame& frame) -> GstBuffer* {
    const std::size_t total = FrameByteSize(frame);
    if (total == 0) {
        return nullptr;
    }
    GstBuffer* gst_buf = gst_buffer_new_allocate(nullptr, total, nullptr);
    if (gst_buf == nullptr) {
        return nullptr;
    }
    GstMapInfo map{};
    if (gst_buffer_map(gst_buf, &map, GST_MAP_WRITE) == 0) {
        gst_buffer_unref(gst_buf);
        return nullptr;
    }
    std::size_t offset = 0;
    for (const auto& plane : frame.planes) {
        if (plane.data != nullptr && plane.size > 0) {
            std::memcpy(map.data + offset, plane.data, plane.size);
            offset += plane.size;
        }
    }
    gst_buffer_unmap(gst_buf, &map);
    return gst_buf;
}

}  // namespace sst::adapters::gst_common
