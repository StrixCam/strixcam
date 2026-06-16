#pragma once

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "domain/config/models/calibration.hpp"
#include "fmt-helper.hpp"

template <>
struct fmt::formatter<sst::config::CalibrationCameraDeviceData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::CalibrationCameraDeviceData& data, FormatContext& ctx) const {
        using sst::config::ArrOptToStr;
        using sst::config::NumOptToStr;
        using sst::config::StrOptToStr;
        return fmt::format_to(ctx.out(),
                              "CalibrationCameraDeviceData{{\n"
                              "  id={},\n"
                              "  intrinsic_matrix={},\n"
                              "  distortion_coefficients={},\n"
                              "  last_calibration_date={}\n"
                              "}}",
                              NumOptToStr(data.id), ArrOptToStr(data.intrinsic_matrix),
                              ArrOptToStr(data.distortion_coefficients),
                              StrOptToStr(data.last_calibration_date));
    }
};

template <>
struct fmt::formatter<sst::config::CalibrationCamerasData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::CalibrationCamerasData& data, FormatContext& ctx) const {
        using sst::config::NumOptToStr;
        using sst::config::VecOptToStr;
        return fmt::format_to(
            ctx.out(),
            "CalibrationCamerasData{{\n"
            "  exposure={},\n"
            "  gain={},\n"
            "  white_balance={},\n"
            "  focus={},\n"
            "  width={},\n"
            "  height={},\n"
            "  format={},\n"
            "  fps={},\n"
            "  device={}\n"
            "}}",
            NumOptToStr(data.exposure), NumOptToStr(data.gain), NumOptToStr(data.white_balance),
            NumOptToStr(data.focus), NumOptToStr(data.width), NumOptToStr(data.height),
            NumOptToStr(data.format), NumOptToStr(data.fps), VecOptToStr(data.device));
    }
};

template <>
struct fmt::formatter<sst::config::CalibrationMicrophoneDeviceData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::CalibrationMicrophoneDeviceData& data,
                FormatContext& ctx) const {
        using sst::config::NumOptToStr;
        using sst::config::StrOptToStr;
        return fmt::format_to(ctx.out(),
                              "CalibrationMicrophoneDeviceData{{\n"
                              "  id={},\n"
                              "  sensitivity={},\n"
                              "  last_calibration_date={}\n"
                              "}}",
                              NumOptToStr(data.id), NumOptToStr(data.sensitivity),
                              StrOptToStr(data.last_calibration_date));
    }
};

template <>
struct fmt::formatter<sst::config::CalibrationMicrophonesData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::CalibrationMicrophonesData& data, FormatContext& ctx) const {
        using sst::config::BoolOptToStr;
        using sst::config::VecOptToStr;
        return fmt::format_to(ctx.out(),
                              "CalibrationMicrophonesData{{\n"
                              "  noise_reduction={},\n"
                              "  device={}\n"
                              "}}",
                              BoolOptToStr(data.noise_reduction), VecOptToStr(data.device));
    }
};

template <>
struct fmt::formatter<sst::config::CalibrationData> {
    static constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const sst::config::CalibrationData& data, FormatContext& ctx) const {
        using sst::config::ObjOptToStr;
        return fmt::format_to(ctx.out(),
                              "CalibrationData{{\n"
                              "  cameras={},\n"
                              "  microphones={}\n"
                              "}}",
                              ObjOptToStr(data.cameras), ObjOptToStr(data.microphones));
    }
};
