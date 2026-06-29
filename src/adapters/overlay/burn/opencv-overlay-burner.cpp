#include "adapters/overlay/burn/opencv-overlay-burner.hpp"

#include <fmt/format.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <spdlog/spdlog.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <optional>
#include <system_error>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/overlay/services/frame-compositor.hpp"

namespace sst::adapters::overlay {

namespace {

namespace fs = std::filesystem;

constexpr double kDefaultFps = 30.0;
constexpr double kMsPerSecond = 1000.0;
constexpr int kBgrChannels = 3;
// How long to wait for the muxer to finalize (write the moov atom) after the last
// frame. Generous: a big clip's tail flush can take a while on software x264.
constexpr int kEosTimeoutSeconds = 120;
constexpr GstClockTime kEosTimeoutNs = static_cast<GstClockTime>(kEosTimeoutSeconds) * GST_SECOND;

// The live capture/encode pipeline already runs hot (~4-5 of the 6 cores). Cap
// the burn's encoder threads and nice the worker so an offline burn can't
// saturate the box and starve wpa_supplicant/networking — which dropped the
// phone's WiFi-Direct route and broke the download right after a burn.
constexpr int kEncoderThreads = 2;
constexpr int kBurnNiceness = 10;

// Wrap an OpenCV BGR Mat as a borrowed BGR8 Frame for the compositor (no copy;
// valid only while `mat` lives).
auto AsFrame(const cv::Mat& mat) -> sst::capture::Frame {
    sst::capture::Frame frame;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.geometry = {static_cast<std::uint32_t>(mat.cols), static_cast<std::uint32_t>(mat.rows)};
    sst::capture::FramePlane plane;
    plane.data = mat.data;
    plane.stride = static_cast<std::uint32_t>(mat.step);
    plane.size = mat.total() * mat.elemSize();
    frame.planes.push_back(plane);
    return frame;
}

// Update `rgba`/`have_rgba` for the scene active at `overlay_ms`, re-rendering
// only when the scene changes (a step function — typically ~once per visible
// update, not per frame).
auto AdvanceOverlay(const sst::overlay::OverlayTimeline& timeline, CairoOverlayRenderer& renderer,
                    const cv::Size& render_size, std::uint64_t overlay_ms,
                    const sst::overlay::RenderScene*& last_scene, sst::overlay::RgbaImage& rgba,
                    bool& have_rgba) -> void {
    const auto* scene = sst::overlay::SceneAtOverlayTime(timeline, overlay_ms);
    if (scene == last_scene) {
        return;  // unchanged — keep the cached rgba/have_rgba
    }
    last_scene = scene;
    have_rgba = false;
    if (scene != nullptr) {
        rgba = renderer.Render(*scene, static_cast<std::uint32_t>(render_size.width),
                               static_cast<std::uint32_t>(render_size.height));
        have_rgba = !rgba.pixels.empty();
    }
}

// Composite the cached overlay onto `frame`. Returns the Mat to encode; when an
// overlay composites, `holder` keeps its buffer alive (the returned Mat borrows
// it). Falls back to the pass-through frame if the composite is rejected.
auto ComposeOut(const cv::Mat& frame, const sst::overlay::RgbaImage& rgba,
                std::optional<sst::capture::Frame>& holder) -> cv::Mat {
    auto composited = sst::overlay::CompositeOverlay(AsFrame(frame), rgba);
    if (composited && !composited->planes.empty()) {
        holder = std::move(composited);
        // Size from the actual decoded frame; the composited buffer is frame-sized.
        return {frame.rows, frame.cols, CV_8UC3, const_cast<std::uint8_t*>(holder->planes[0].data),
                holder->planes[0].stride};
    }
    return frame;  // pass-through (shares the decoded buffer)
}

// Owns a GStreamer "BGR appsrc -> I420 -> x264enc(ultrafast) -> mp4mux -> filesink"
// encode pipeline for one offline burn. Software x264 (the Orin Nano has no
// NVENC), but speed-preset=ultrafast is markedly faster than OpenCV's
// cv::VideoWriter, which gives no preset control and ran the dominant encode
// phase at a slow default — the live recorder already encodes with this same
// element on this silicon (#18).
class GstFileEncoder {
   public:
    GstFileEncoder() = default;
    ~GstFileEncoder() { Teardown(); }
    GstFileEncoder(const GstFileEncoder&) = delete;
    auto operator=(const GstFileEncoder&) -> GstFileEncoder& = delete;
    GstFileEncoder(GstFileEncoder&&) = delete;
    auto operator=(GstFileEncoder&&) -> GstFileEncoder& = delete;

    // NOLINTBEGIN(bugprone-easily-swappable-parameters) floor-ok: caps geometry, one call site
    auto Open(int width, int height, double fps, const fs::path& out_path) -> bool {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        gst_init(nullptr, nullptr);  // idempotent
        width_ = width;
        height_ = height;
        fps_n_ = std::max(1, static_cast<int>(std::lround(fps)));

        // Path goes on the filesink as a property (not the launch string) so a
        // path with awkward characters can't break gst_parse_launch.
        const std::string launch = fmt::format(
            "appsrc name=src is-live=false block=true format=time "
            "caps=video/x-raw,format=BGR,width={w},height={h},framerate={fps}/1 "
            "! videoconvert ! video/x-raw,format=I420 "
            "! x264enc speed-preset=ultrafast threads={threads} ! mp4mux ! filesink name=sink",
            fmt::arg("w", width_), fmt::arg("h", height_), fmt::arg("fps", fps_n_),
            fmt::arg("threads", kEncoderThreads));

        GError* err = nullptr;
        pipeline_ = gst_parse_launch(launch.c_str(), &err);
        if (err != nullptr) {
            spdlog::error("OverlayBurn: encode pipeline build failed: {}", err->message);
            g_error_free(err);
            Teardown();
            return false;
        }
        if (pipeline_ == nullptr) {
            return false;
        }

        GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        if (src == nullptr || sink == nullptr) {
            if (src != nullptr) {
                gst_object_unref(src);
            }
            if (sink != nullptr) {
                gst_object_unref(sink);
            }
            Teardown();
            return false;
        }
        appsrc_ = GST_APP_SRC(src);  // takes over the ref from gst_bin_get_by_name
        g_object_set(sink, "location", out_path.string().c_str(), nullptr);
        gst_object_unref(sink);

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            spdlog::error("OverlayBurn: encode pipeline failed to start");
            Teardown();
            return false;
        }
        return true;
    }

    // Push one tightly-packed BGR frame at the given frame index (its PTS follows
    // the index + framerate). Returns false if the encoder rejected the buffer.
    auto Push(const cv::Mat& bgr, std::uint64_t index) -> bool {
        if (appsrc_ == nullptr || bgr.cols != width_ || bgr.rows != height_) {
            return false;
        }
        const auto row_bytes = static_cast<gsize>(width_) * kBgrChannels;
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, row_bytes * height_, nullptr);
        if (buf == nullptr) {
            return false;
        }
        GstMapInfo map{};
        if (gst_buffer_map(buf, &map, GST_MAP_WRITE) == 0) {
            gst_buffer_unref(buf);
            return false;
        }
        // Copy row by row so a Mat with row padding (stride != width*3) still packs
        // tightly into the BGR caps the encoder declared.
        for (int row = 0; row < height_; ++row) {
            std::memcpy(map.data + (static_cast<gsize>(row) * row_bytes), bgr.ptr(row), row_bytes);
        }
        gst_buffer_unmap(buf, &map);

        const auto fps = static_cast<guint64>(fps_n_);
        GST_BUFFER_PTS(buf) = gst_util_uint64_scale(index, GST_SECOND, fps);
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, fps);
        return gst_app_src_push_buffer(appsrc_, buf) == GST_FLOW_OK;  // consumes buf
    }

    // Signal end-of-stream and wait for the muxer to finalize the file. Returns
    // true only on a clean EOS.
    auto Finish() -> bool {
        if (appsrc_ == nullptr || pipeline_ == nullptr) {
            return false;
        }
        gst_app_src_end_of_stream(appsrc_);
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus == nullptr) {
            return false;
        }
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, kEosTimeoutNs, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        bool eos_ok = false;
        if (msg == nullptr) {
            spdlog::error("OverlayBurn: encode timed out waiting for EOS");
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            eos_ok = true;
        } else {
            GError* gerr = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &gerr, &dbg);
            spdlog::error("OverlayBurn: encode error: {}", gerr != nullptr ? gerr->message : "?");
            if (gerr != nullptr) {
                g_error_free(gerr);
            }
            g_free(dbg);
        }
        if (msg != nullptr) {
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
        return eos_ok;
    }

   private:
    void Teardown() {
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (appsrc_ != nullptr) {
            gst_object_unref(appsrc_);  // ref taken in Open via gst_bin_get_by_name
            appsrc_ = nullptr;
        }
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    GstElement* pipeline_ = nullptr;
    GstAppSrc* appsrc_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_n_ = static_cast<int>(kDefaultFps);
};

}  // namespace

auto OpenCvOverlayBurner::Burn(const fs::path& l1_path,
                               const sst::overlay::OverlayTimeline& timeline,
                               const fs::path& l2_path, const std::atomic<bool>& cancel) -> bool {
    try {
        // Lower this worker thread's scheduling priority so the burn yields to the
        // live pipeline + networking under contention (setpriority with who=0 nices
        // the calling thread on Linux; best-effort).
        if (setpriority(PRIO_PROCESS, 0, kBurnNiceness) != 0) {
            spdlog::debug("OverlayBurn: could not lower burn thread priority (continuing)");
        }
        cv::VideoCapture cap(l1_path.string());
        if (!cap.isOpened()) {
            spdlog::error("OverlayBurn: cannot open L1 {}", l1_path.string());
            return false;
        }
        double fps = cap.get(cv::CAP_PROP_FPS);
        if (fps <= 0.0) {
            fps = kDefaultFps;
        }
        // Overlay render canvas follows the container's reported geometry (the
        // resolution the layout was authored for); the encoder geometry follows
        // the actual decoded frame (set lazily on the first frame).
        const int render_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        const int render_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        if (render_width <= 0 || render_height <= 0) {
            spdlog::error("OverlayBurn: L1 {} has no video frames", l1_path.string());
            return false;
        }

        const auto fail = [&l2_path] {
            std::error_code remove_ec;
            fs::remove(l2_path, remove_ec);
            return false;
        };

        GstFileEncoder encoder;
        bool encoder_open = false;

        // Render the overlay only when the active scene changes (a step function —
        // it changes ~once per visible update, not per frame), then composite the
        // cached RGBA onto every frame until the next change.
        const sst::overlay::RenderScene* last_scene = nullptr;
        sst::overlay::RgbaImage rgba;
        bool have_rgba = false;
        cv::Mat frame;
        std::uint64_t index = 0;

        // Per-phase timers (#18): decode (cv::VideoCapture) / composite (Cairo
        // render + alpha blend) / encode (GStreamer x264 push, which blocks on the
        // encoder via block=true). Reported once at the end.
        using Clock = std::chrono::steady_clock;
        std::chrono::nanoseconds decode_ns{0};
        std::chrono::nanoseconds composite_ns{0};
        std::chrono::nanoseconds encode_ns{0};

        while (true) {
            const auto t_decode = Clock::now();
            const bool got = cap.read(frame) && !frame.empty();
            decode_ns += Clock::now() - t_decode;
            if (!got) {
                break;
            }
            if (cancel.load()) {
                spdlog::info("OverlayBurn: cancelled at frame {} for {}", index, l1_path.string());
                return fail();  // encoder dtor tears the pipeline down
            }
            if (!encoder_open) {
                if (!encoder.Open(frame.cols, frame.rows, fps, l2_path)) {
                    spdlog::error("OverlayBurn: cannot open L2 encoder {}", l2_path.string());
                    return fail();
                }
                encoder_open = true;
            }

            const auto pts_ms =
                static_cast<std::uint64_t>(static_cast<double>(index) * kMsPerSecond / fps);

            const auto t_comp = Clock::now();
            AdvanceOverlay(timeline, renderer_, cv::Size(render_width, render_height),
                           timeline.anchor_ms + pts_ms, last_scene, rgba, have_rgba);
            // `holder` keeps the composited buffer alive — `out` borrows it.
            std::optional<sst::capture::Frame> holder;
            const cv::Mat out = have_rgba ? ComposeOut(frame, rgba, holder) : frame;
            composite_ns += Clock::now() - t_comp;

            const auto t_enc = Clock::now();
            const bool pushed = encoder.Push(out, index);
            encode_ns += Clock::now() - t_enc;
            if (!pushed) {
                spdlog::error("OverlayBurn: encoder rejected frame {} for {}", index,
                              l1_path.string());
                return fail();
            }
            ++index;
        }

        cap.release();

        if (index == 0) {
            spdlog::error("OverlayBurn: decoded no frames from {}", l1_path.string());
            return fail();
        }

        // Flush the encoder + finalize the MP4 (counts toward encode time).
        const auto t_finish = Clock::now();
        const bool finished = encoder.Finish();
        encode_ns += Clock::now() - t_finish;
        if (!finished) {
            spdlog::error("OverlayBurn: encode finalize failed for {}", l2_path.string());
            return fail();
        }

        const auto to_ms = [](std::chrono::nanoseconds duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        };
        spdlog::info(
            "OverlayBurn: composited {} frame(s) -> {} | timing: decode={}ms composite={}ms "
            "encode={}ms",
            index, l2_path.string(), to_ms(decode_ns), to_ms(composite_ns), to_ms(encode_ns));
        return true;
    } catch (const cv::Exception& e) {
        // A corrupt/truncated L1 or an unexpected frame size makes an OpenCV call
        // throw. Fail this job instead of letting the exception escape the worker
        // thread, which would std::terminate() the whole firmware process.
        spdlog::error("OverlayBurn: OpenCV error on {}: {}", l1_path.string(), e.what());
        std::error_code remove_ec;
        fs::remove(l2_path, remove_ec);
        return false;
    }
}

}  // namespace sst::adapters::overlay
