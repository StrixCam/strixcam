#include "adapters/focus/i2c-focuser.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace sst::adapters::focus {

namespace {
constexpr int kVcmAddress = 0x0c;      // ArduCAM VCM DAC
constexpr std::uint32_t kShift = 4U;    // code is a 10-bit value in bits [4..13]
constexpr std::uint32_t kFocusMask = 0x3ff0U;
constexpr std::uint32_t kHiShift = 8U;
constexpr std::uint32_t kHiMask = 0x3fU;
constexpr std::uint32_t kLoMask = 0xf0U;
constexpr std::size_t kPayloadBytes = 2;
}  // namespace

auto I2cFocuser::BusDevicePath(int bus) -> std::string {
    return "/dev/i2c-" + std::to_string(bus);
}

I2cFocuser::I2cFocuser(std::array<int, 2> camera_bus) : camera_bus_(camera_bus) {
    // The VCM is write-only + unprobeable, so "available" just means both camera
    // I2C buses open. Open-and-close here; SetFocus reopens per write.
    bool ok = true;
    for (const int bus : camera_bus_) {
        const int fd = ::open(BusDevicePath(bus).c_str(), O_RDWR);
        if (fd < 0) {
            ok = false;
        } else {
            ::close(fd);
        }
    }
    available_ = ok;
    if (!available_) {
        spdlog::warn("I2cFocuser: a camera i2c bus failed to open — focus control unavailable");
    }
}

auto I2cFocuser::IsAvailable() const -> bool { return available_; }

auto I2cFocuser::SetFocus(std::uint32_t camera_index, std::uint32_t position) -> bool {
    if (camera_index >= camera_bus_.size()) {
        spdlog::warn("I2cFocuser: camera_index {} out of range", camera_index);
        return false;
    }
    const std::uint32_t code = std::min(position, sst::focus::kMaxFocus);
    const std::uint32_t packed = (code << kShift) & kFocusMask;
    const std::array<std::uint8_t, kPayloadBytes> payload{
        static_cast<std::uint8_t>((packed >> kHiShift) & kHiMask),
        static_cast<std::uint8_t>(packed & kLoMask)};

    const int bus = camera_bus_[camera_index];
    const int fd = ::open(BusDevicePath(bus).c_str(), O_RDWR);
    if (fd < 0) {
        spdlog::error("I2cFocuser: open {} failed", BusDevicePath(bus));
        return false;
    }
    bool ok = true;
    if (::ioctl(fd, I2C_SLAVE, kVcmAddress) < 0) {
        spdlog::error("I2cFocuser: I2C_SLAVE 0x0c on bus {} failed", bus);
        ok = false;
    } else if (::write(fd, payload.data(), payload.size()) !=
               static_cast<ssize_t>(payload.size())) {
        spdlog::error("I2cFocuser: write focus {} to bus {} failed", code, bus);
        ok = false;
    }
    ::close(fd);
    return ok;
}

}  // namespace sst::adapters::focus
