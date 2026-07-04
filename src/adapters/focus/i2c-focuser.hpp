#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "app/focus/ports/focuser.hpp"

namespace sst::adapters::focus {

// Drives the ArduCAM IMX477 motorized VCM over Linux i2c-dev. Each camera's VCM
// (DAC at 0x0c) lives on the same I2C bus as its sensor: on the Orin Nano
// CAM0 = bus 10, CAM1 = bus 9 (ArduCAM's documented mapping). The DAC is
// write-only; the focus code (0..1000) is packed as ArduCAM's Focuser does:
//   v = (code << 4) & 0x3ff0 ; hi = (v >> 8) & 0x3f ; lo = v & 0xf0
// then written as the two-byte payload {hi, lo} to 0x0c. Writing 0x0c is
// independent of the streaming sensor at 0x1a (different address, kernel-
// serialized), so focus can move live during capture.
class I2cFocuser final : public sst::focus::IFocuser {
   public:
    // camera_index -> I2C bus number. Default is the Orin Nano mapping {10, 9}.
    // NOLINTNEXTLINE(readability-magic-numbers)
    explicit I2cFocuser(std::array<int, 2> camera_bus = {10, 9});

    auto SetFocus(std::uint32_t camera_index, std::uint32_t position) -> bool override;
    [[nodiscard]] auto IsAvailable() const -> bool override;

   private:
    static auto BusDevicePath(int bus) -> std::string;

    std::array<int, 2> camera_bus_;
    bool available_{false};
};

}  // namespace sst::adapters::focus
