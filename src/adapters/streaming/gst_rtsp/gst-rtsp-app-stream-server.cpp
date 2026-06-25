#include "adapters/streaming/gst_rtsp/gst-rtsp-app-stream-server.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

#include "domain/streaming/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace sst::adapters::streaming {

namespace {

constexpr const char* kAppsrcName = "src";

auto FrameByteSize(const sst::capture::Frame& frame) -> std::size_t {
    std::size_t total = 0;
    for (const auto& plane : frame.planes) {
        total += plane.size;
    }
    return total;
}

auto BuildLaunch(const sst::streaming::AppStreamConfig& cfg) -> std::string {
    // Software H.264: the Orin Nano has no NVENC. x264enc reads system memory,
    // so the nvvidconv→NVMM hop is dropped (appsrc already provides BGR system
    // memory). do-timestamp=true stamps PTS on arrival — required for x264enc,
    // which (unlike nvv4l2h264enc) corrupts the stream on absent/invalid PTS.
    // x264enc bitrate is in kbit/s (nvv4l2h264enc used bit/s). gst-rtsp-server
    // shares one encode across all viewers (set_shared), so this CPU cost is
    // paid once regardless of viewer count.
    return fmt::format(
        "( appsrc name={src} is-live=true format=time do-timestamp=true "
        "    caps=\"video/x-raw,format=BGR,width={w},height={h},framerate={fps}/1\" "
        "  ! videoconvert "
        "  ! x264enc speed-preset=ultrafast tune=zerolatency bitrate={brk} key-int-max={gik} "
        "  ! h264parse config-interval=1 "
        "  ! rtph264pay name=pay0 pt=96 config-interval=1 )",
        fmt::arg("src", kAppsrcName), fmt::arg("w", cfg.width), fmt::arg("h", cfg.height),
        fmt::arg("fps", cfg.framerate), fmt::arg("brk", cfg.bitrate_kbps),
        fmt::arg("gik", cfg.framerate * 2));
}

}  // namespace

GstRtspAppStreamServer::GstRtspAppStreamServer() { gst_init(nullptr, nullptr); }

GstRtspAppStreamServer::~GstRtspAppStreamServer() {
    if (running_) {
        Stop();
    }
}

auto GstRtspAppStreamServer::Start(const sst::streaming::AppStreamConfig& config) -> bool {
    std::lock_guard lock(mtx_);
    if (running_) {
        spdlog::warn("GstRtspAppStreamServer::Start: already running");
        return true;
    }
    config_ = config;
    spdlog::info("GstRtspAppStreamServer::Start {}", config_);

    context_ = g_main_context_new();
    loop_ = g_main_loop_new(context_, FALSE);
    g_main_context_push_thread_default(context_);

    server_ = gst_rtsp_server_new();
    // Bind to the configured address only (WiFi Direct GO IP in production) so
    // the preview is unreachable on the cellular interface (R22).
    gst_rtsp_server_set_address(server_, config_.address.c_str());
    const std::string port_str = std::to_string(config_.port);
    gst_rtsp_server_set_service(server_, port_str.c_str());

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    factory_ = gst_rtsp_media_factory_new();

    const std::string launch = BuildLaunch(config_);
    spdlog::info("GstRtspAppStreamServer: launch = {}", launch);
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    g_signal_connect(factory_, "media-configure",
                     G_CALLBACK(&GstRtspAppStreamServer::OnMediaConfigureStatic), this);

    gst_rtsp_mount_points_add_factory(mounts, config_.mount_point.c_str(), factory_);
    g_object_unref(mounts);

    source_id_ = gst_rtsp_server_attach(server_, context_);
    g_main_context_pop_thread_default(context_);

    if (source_id_ == 0) {
        spdlog::error("GstRtspAppStreamServer: gst_rtsp_server_attach failed");
        if (server_ != nullptr) {
            g_object_unref(server_);
            server_ = nullptr;
        }
        if (loop_ != nullptr) {
            g_main_loop_unref(loop_);
            loop_ = nullptr;
        }
        if (context_ != nullptr) {
            g_main_context_unref(context_);
            context_ = nullptr;
        }
        return false;
    }

    running_ = true;
    loop_thread_ = std::thread([this] {
        g_main_context_push_thread_default(context_);
        g_main_loop_run(loop_);
        g_main_context_pop_thread_default(context_);
    });

    spdlog::info("GstRtspAppStreamServer: serving rtsp://{}:{}{}", config_.address, config_.port,
                 config_.mount_point);
    return true;
}

auto GstRtspAppStreamServer::Stop() -> void {
    std::thread to_join;
    {
        std::lock_guard lock(mtx_);
        if (!running_) {
            return;
        }
        spdlog::info("GstRtspAppStreamServer::Stop");

        if (appsrc_ != nullptr) {
            gst_app_src_end_of_stream(appsrc_);
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
        }
        if (loop_ != nullptr && g_main_loop_is_running(loop_) != 0) {
            g_main_loop_quit(loop_);
        }
        to_join = std::move(loop_thread_);
        running_ = false;
    }

    if (to_join.joinable()) {
        to_join.join();
    }

    std::lock_guard lock(mtx_);
    if (source_id_ != 0 && context_ != nullptr) {
        GSource* src = g_main_context_find_source_by_id(context_, source_id_);
        if (src != nullptr) {
            g_source_destroy(src);
        }
        source_id_ = 0;
    }
    if (server_ != nullptr) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (loop_ != nullptr) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
    }
    if (context_ != nullptr) {
        g_main_context_unref(context_);
        context_ = nullptr;
    }
    factory_ = nullptr;
}

auto GstRtspAppStreamServer::IsRunning() const -> bool { return running_; }

auto GstRtspAppStreamServer::Push(const sst::capture::Frame& frame) -> void {
    GstAppSrc* target = nullptr;
    {
        std::lock_guard lock(mtx_);
        if (!running_ || appsrc_ == nullptr) {
            return;
        }
        target = appsrc_;
        gst_object_ref(target);
    }

    const auto total = FrameByteSize(frame);
    if (total == 0) {
        gst_object_unref(target);
        return;
    }

    auto* gst_buf = gst_buffer_new_allocate(nullptr, total, nullptr);
    if (gst_buf == nullptr) {
        gst_object_unref(target);
        return;
    }
    GstMapInfo map{};
    if (gst_buffer_map(gst_buf, &map, GST_MAP_WRITE) == 0) {
        gst_buffer_unref(gst_buf);
        gst_object_unref(target);
        return;
    }
    std::size_t offset = 0;
    for (const auto& plane : frame.planes) {
        if (plane.data != nullptr && plane.size > 0) {
            std::memcpy(map.data + offset, plane.data, plane.size);
            offset += plane.size;
        }
    }
    gst_buffer_unmap(gst_buf, &map);

    // Leave the buffer timestamps unset: the appsrc is configured with
    // do-timestamp=true, so it stamps a valid running-time PTS on each buffer as
    // it is pushed. Forcing GST_CLOCK_TIME_NONE here defeats that and corrupts
    // the x264enc software-encoded stream (nvv4l2h264enc tolerated it; x264enc
    // does not).

    (void)gst_app_src_push_buffer(target, gst_buf);
    gst_object_unref(target);
}

auto GstRtspAppStreamServer::OnMediaConfigureStatic(GstRTSPMediaFactory* /*factory*/,
                                                    GstRTSPMedia* media, gpointer user_data)
    -> void {
    auto* self = static_cast<GstRtspAppStreamServer*>(user_data);
    self->OnMediaConfigure(media);
}

auto GstRtspAppStreamServer::OnMediaConfigure(GstRTSPMedia* media) -> void {
    GstElement* element = gst_rtsp_media_get_element(media);
    if (element == nullptr) {
        return;
    }
    GstElement* src = gst_bin_get_by_name(GST_BIN(element), kAppsrcName);
    gst_object_unref(element);
    if (src == nullptr) {
        spdlog::warn("GstRtspAppStreamServer::OnMediaConfigure: appsrc not found");
        return;
    }

    {
        std::lock_guard lock(mtx_);
        if (appsrc_ != nullptr) {
            gst_object_unref(appsrc_);
        }
        appsrc_ = GST_APP_SRC(src);
    }
    spdlog::info("GstRtspAppStreamServer: client connected, appsrc bound");
}

}  // namespace sst::adapters::streaming
