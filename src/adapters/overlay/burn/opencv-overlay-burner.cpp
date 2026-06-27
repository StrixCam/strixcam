#include "adapters/overlay/burn/opencv-overlay-burner.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <system_error>
#include <vector>

#include "domain/capture/models/frame.hpp"
#include "domain/common/models/pixel-format.hpp"
#include "domain/overlay/services/frame-compositor.hpp"

namespace sst::adapters::overlay {

namespace {

namespace fs = std::filesystem;

constexpr double kDefaultFps = 30.0;
constexpr double kMsPerSecond = 1000.0;
// H.264 in an MP4 container — matches the L1 codec so the L2 plays the same way.
const int kFourccAvc1 = cv::VideoWriter::fourcc('a', 'v', 'c', '1');

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

}  // namespace

auto OpenCvOverlayBurner::Burn(const fs::path& l1_path,
                               const sst::overlay::OverlayTimeline& timeline,
                               const fs::path& l2_path) -> bool {
    cv::VideoCapture cap(l1_path.string());
    if (!cap.isOpened()) {
        spdlog::error("OverlayBurn: cannot open L1 {}", l1_path.string());
        return false;
    }
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (fps <= 0.0) {
        fps = kDefaultFps;
    }
    const int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (width <= 0 || height <= 0) {
        spdlog::error("OverlayBurn: L1 {} has no video frames", l1_path.string());
        return false;
    }

    cv::VideoWriter writer(l2_path.string(), kFourccAvc1, fps, cv::Size(width, height));
    if (!writer.isOpened()) {
        spdlog::error("OverlayBurn: cannot open L2 writer {} (H.264 encoder missing?)",
                      l2_path.string());
        return false;
    }

    // Render the overlay only when the active scene changes (a step function —
    // it changes ~once per visible update, not per frame), then composite the
    // cached RGBA onto every frame until the next change.
    const sst::overlay::RenderScene* last_scene = nullptr;
    sst::overlay::RgbaImage rgba;
    bool have_rgba = false;
    cv::Mat frame;
    std::uint64_t index = 0;

    while (cap.read(frame) && !frame.empty()) {
        const auto pts_ms =
            static_cast<std::uint64_t>(static_cast<double>(index) * kMsPerSecond / fps);
        const auto* scene = sst::overlay::SceneAtOverlayTime(timeline, timeline.anchor_ms + pts_ms);
        if (scene != last_scene) {
            last_scene = scene;
            have_rgba = false;
            if (scene != nullptr) {
                rgba = renderer_.Render(*scene, static_cast<std::uint32_t>(width),
                                        static_cast<std::uint32_t>(height));
                have_rgba = !rgba.pixels.empty();
            }
        }

        if (have_rgba) {
            const auto composited = sst::overlay::CompositeOverlay(AsFrame(frame), rgba);
            if (composited && !composited->planes.empty()) {
                const cv::Mat out(height, width, CV_8UC3,
                                  const_cast<std::uint8_t*>(composited->planes[0].data),
                                  composited->planes[0].stride);
                writer.write(out);
            } else {
                writer.write(frame);  // composite rejected the frame — pass through
            }
        } else {
            writer.write(frame);  // no overlay active at this time
        }
        ++index;
    }

    writer.release();
    cap.release();

    if (index == 0) {
        spdlog::error("OverlayBurn: decoded no frames from {}", l1_path.string());
        std::error_code remove_ec;
        fs::remove(l2_path, remove_ec);
        return false;
    }
    spdlog::info("OverlayBurn: composited {} frame(s) -> {}", index, l2_path.string());
    return true;
}

}  // namespace sst::adapters::overlay
