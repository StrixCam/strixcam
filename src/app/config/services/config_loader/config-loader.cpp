#include "./config-loader.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "adapters/config/json/json.hpp"

namespace sst::config::app {

using sst::config::JsonReaderAdapter;

static auto MakePath(const std::string& root, const std::string& filename,
                     const std::string& extension) -> std::filesystem::path {
    return std::filesystem::path(root) / (filename + "." + extension);
}

template <typename R>
static auto fail(std::string_view name, const R& configurationReference) -> bool {
    if (!configurationReference.success) {
        spdlog::error("Config load failed: {}", name);
        return true;
    }
    return false;
}

ConfigLoader::ConfigLoader(std::string root_path, std::string file_type)
    : root_path_(std::move(root_path)), file_type_(std::move(file_type)) {
    const std::string ext = file_type_;
    if (file_type_ == "json") {
        deviceAdapter_ =
            std::make_unique<JsonReaderAdapter<DeviceData>>(MakePath(root_path_, "device", ext));
        calibrationAdapter_ = std::make_unique<JsonReaderAdapter<CalibrationData>>(
            MakePath(root_path_, "calibration", ext));
        storageAdapter_ =
            std::make_unique<JsonReaderAdapter<StorageData>>(MakePath(root_path_, "storage", ext));
        wifiDirectAdapter_ = std::make_unique<JsonReaderAdapter<WifiDirectData>>(
            MakePath(root_path_, "wifi-direct", ext));
    } else {
        throw std::runtime_error("Unsupported config file type: " + file_type_);
    }
}

auto ConfigLoader::get() -> ConfigData {
    if (!deviceAdapter_ || !calibrationAdapter_ || !storageAdapter_ || !wifiDirectAdapter_) {
        throw std::logic_error("ConfigLoader adapters not initialized");
    }

    const auto deviceConfig = deviceAdapter_->load();
    const auto calibrationConfig = calibrationAdapter_->load();
    const auto storageConfig = storageAdapter_->load();
    const auto wifiDirectConfig = wifiDirectAdapter_->load();

    if (fail("device", deviceConfig) || fail("calibration", calibrationConfig) ||
        fail("storage", storageConfig) || fail("wifi-direct", wifiDirectConfig)) {
        throw std::runtime_error("failed to load configuration files");
    }

    return ConfigData{
        .calibration = calibrationConfig.data,
        .device = deviceConfig.data,
        .storage = storageConfig.data,
        .wifi_direct = wifiDirectConfig.data,
    };
}

}  // namespace sst::config::app
