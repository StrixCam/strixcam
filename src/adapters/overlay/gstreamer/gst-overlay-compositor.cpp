#include "adapters/overlay/gstreamer/gst-overlay-compositor.hpp"

#include <gst/app/gstappsrc.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace sst::adapters::overlay {

GstOverlayCompositor::GstOverlayCompositor(sst::common::OutputSize size)
    : width_(size.width), height_(size.height) {
    gst_init(nullptr, nullptr);

    appsrc_ = gst_element_factory_make("appsrc", "overlay_src");
    if (appsrc_ == nullptr) {
        spdlog::error("GstOverlayCompositor: failed to create appsrc");
        return;
    }
    // Keep a ref we own; the orchestrator adds this to its bin (U14).
    gst_object_ref_sink(appsrc_);

    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                        G_TYPE_INT, static_cast<int>(width_), "height", G_TYPE_INT,
                                        static_cast<int>(height_), "framerate", GST_TYPE_FRACTION,
                                        0, 1, nullptr);
    g_object_set(appsrc_, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp",
                 TRUE, nullptr);
    gst_caps_unref(caps);
}

GstOverlayCompositor::~GstOverlayCompositor() {
    if (appsrc_ != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
}

auto GstOverlayCompositor::PushFrame(const sst::overlay::RgbaImage& frame) -> void {
    if (appsrc_ == nullptr) {
        return;
    }
    const gsize size = frame.pixels.size();
    if (size == 0) {
        return;
    }
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (buffer == nullptr) {
        spdlog::warn("GstOverlayCompositor::PushFrame: gst_buffer_new_allocate({}) failed", size);
        return;
    }
    gst_buffer_fill(buffer, 0, frame.pixels.data(), size);

    const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (ret != GST_FLOW_OK) {
        spdlog::warn("GstOverlayCompositor::PushFrame: push_buffer returned {}",
                     static_cast<int>(ret));
    }
}

}  // namespace sst::adapters::overlay
