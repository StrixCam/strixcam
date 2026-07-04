#pragma once

#include <cstdint>

namespace sst::focus {

// Max focus code the ArduCAM IMX477 VCM accepts (0 = near, 1000 = far).
inline constexpr std::uint32_t kMaxFocus = 1000;

// Drives the motorized VCM lens on the ArduCAM IMX477. `position` is 0..kMaxFocus.
// Implementations map camera_index to its per-camera I2C bus and write the VCM
// DAC. The VCM is write-only (invisible to a read-based i2cdetect), so presence
// is treated as "available if the bus opens".
class IFocuser {
   public:
    virtual ~IFocuser() = default;

    // Set the lens focus code for a camera. Returns false on a bad index or I2C
    // write error. Clamps position to [0, kMaxFocus].
    virtual auto SetFocus(std::uint32_t camera_index, std::uint32_t position) -> bool = 0;

    // True if the focuser's I2C buses opened at construction — the lens hardware
    // is presumed present (the VCM can't be probed by read).
    [[nodiscard]] virtual auto IsAvailable() const -> bool = 0;
};

}  // namespace sst::focus
