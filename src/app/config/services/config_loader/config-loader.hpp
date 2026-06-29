#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "app/config/ports/config-reader.hpp"
#include "domain/config/models/calibration.hpp"
#include "domain/config/models/config-data.hpp"
#include "domain/config/models/device.hpp"
#include "domain/config/models/storage.hpp"
#include "domain/config/models/uplink-config.hpp"
#include "domain/config/models/wifi-direct.hpp"

namespace sst::config::app {

using sst::config::CalibrationData;
using sst::config::ConfigData;
using sst::config::DeviceData;
using sst::config::IConfigFileReaderAdapter;
using sst::config::StorageData;
using sst::config::UplinkData;
using sst::config::WifiDirectData;

class ConfigLoader {
   public:
    ConfigLoader(std::string root_path, std::string file_type);

    auto get() -> ConfigData;

   private:
    // First-run self-provisioning: write `default_json` to <root>/<name>.<type>
    // when that file is missing, so a fresh device starts without hand-placed
    // config. Existing files are never overwritten.
    void EnsureDefault(const std::string& name, std::string_view default_json);

    std::string root_path_;
    std::string file_type_;
    std::unique_ptr<IConfigFileReaderAdapter<DeviceData>> deviceAdapter_;
    std::unique_ptr<IConfigFileReaderAdapter<CalibrationData>> calibrationAdapter_;
    std::unique_ptr<IConfigFileReaderAdapter<StorageData>> storageAdapter_;
    std::unique_ptr<IConfigFileReaderAdapter<WifiDirectData>> wifiDirectAdapter_;
    std::unique_ptr<IConfigFileReaderAdapter<UplinkData>> uplinkAdapter_;
};

}  // namespace sst::config::app
