#pragma once

#include "domain/config/models/calibration.hpp"
#include "domain/config/models/device.hpp"
#include "domain/config/models/storage.hpp"
#include "domain/config/models/uplink-config.hpp"
#include "domain/config/models/wifi-direct.hpp"

namespace sst::config {
struct ConfigData {
    sst::config::CalibrationData calibration;
    sst::config::DeviceData device;
    sst::config::StorageData storage;
    sst::config::WifiDirectData wifi_direct;
    sst::config::UplinkData uplink;
};
}  // namespace sst::config
