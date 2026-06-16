#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sst::config {

// Pinhole camera model: a 3x3 intrinsic matrix (fx, fy, cx, cy + skew/homog
// entries) and OpenCV's 5 radial/tangential distortion coefficients (k1, k2,
// p1, p2, k3).
inline constexpr std::size_t kIntrinsicMatrixSize = 9;
inline constexpr std::size_t kDistortionCoeffCount = 5;

struct CalibrationCameraDeviceData {
    std::optional<std::uint32_t> id{std::nullopt};
    std::optional<std::array<float, kIntrinsicMatrixSize>> intrinsic_matrix{std::nullopt};
    std::optional<std::array<float, kDistortionCoeffCount>> distortion_coefficients{std::nullopt};
    std::optional<std::string> last_calibration_date{std::nullopt};
};

struct CalibrationCamerasData {
    std::optional<std::uint32_t> exposure{std::nullopt};
    std::optional<float> gain{std::nullopt};
    std::optional<std::uint32_t> white_balance{std::nullopt};
    std::optional<std::uint32_t> focus{std::nullopt};
    std::optional<std::uint32_t> width{std::nullopt};
    std::optional<std::uint32_t> height{std::nullopt};
    std::optional<std::uint32_t> format{std::nullopt};
    std::optional<std::uint32_t> fps{std::nullopt};
    std::optional<std::vector<CalibrationCameraDeviceData>> device{std::nullopt};
};

struct CalibrationMicrophoneDeviceData {
    std::optional<std::uint32_t> id{std::nullopt};
    std::optional<float> sensitivity{std::nullopt};
    std::optional<std::string> last_calibration_date{std::nullopt};
};

struct CalibrationMicrophonesData {
    std::optional<bool> noise_reduction{std::nullopt};
    std::optional<std::vector<CalibrationMicrophoneDeviceData>> device{std::nullopt};
};

struct CalibrationData {
    std::optional<CalibrationCamerasData> cameras{std::nullopt};
    std::optional<CalibrationMicrophonesData> microphones{std::nullopt};
};

}  // namespace sst::config
