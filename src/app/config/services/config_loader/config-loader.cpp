#include "./config-loader.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "./config-defaults.hpp"
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
        uplinkAdapter_ =
            std::make_unique<JsonReaderAdapter<UplinkData>>(MakePath(root_path_, "uplink", ext));
    } else {
        throw std::runtime_error("Unsupported config file type: " + file_type_);
    }
}

void ConfigLoader::EnsureDefault(const std::string& name, std::string_view default_json) {
    const std::filesystem::path path = MakePath(root_path_, name, file_type_);
    std::error_code ecode;
    if (std::filesystem::exists(path, ecode)) {
        return;  // never clobber an existing (possibly operator-edited) file
    }
    std::filesystem::create_directories(path.parent_path(), ecode);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("Config: could not write default '{}' to {}", name, path.string());
        return;  // get() then fails the load and throws, as it did before
    }
    out << default_json;
    spdlog::info("Config: wrote default '{}' -> {}", name, path.string());
}

auto ConfigLoader::get() -> ConfigData {
    // First run: provision built-in defaults for any missing file so a fresh
    // device (empty /etc/sst/cam/config) starts out-of-box. json is the only
    // file_type with adapters + defaults built.
    if (file_type_ == "json") {
        EnsureDefault("device", defaults::kDeviceJson);
        EnsureDefault("calibration", defaults::kCalibrationJson);
        EnsureDefault("storage", defaults::kStorageJson);
        EnsureDefault("wifi-direct", defaults::kWifiDirectJson);
        EnsureDefault("uplink", defaults::kUplinkJson);
    }

    if (!deviceAdapter_ || !calibrationAdapter_ || !storageAdapter_ || !wifiDirectAdapter_ ||
        !uplinkAdapter_) {
        throw std::logic_error("ConfigLoader adapters not initialized");
    }

    const auto deviceConfig = deviceAdapter_->load();
    const auto calibrationConfig = calibrationAdapter_->load();
    const auto storageConfig = storageAdapter_->load();
    const auto wifiDirectConfig = wifiDirectAdapter_->load();
    const auto uplinkConfig = uplinkAdapter_->load();

    if (fail("device", deviceConfig) || fail("calibration", calibrationConfig) ||
        fail("storage", storageConfig) || fail("wifi-direct", wifiDirectConfig) ||
        fail("uplink", uplinkConfig)) {
        throw std::runtime_error("failed to load configuration files");
    }

    return ConfigData{
        .calibration = calibrationConfig.data,
        .device = deviceConfig.data,
        .storage = storageConfig.data,
        .wifi_direct = wifiDirectConfig.data,
        .uplink = uplinkConfig.data,
    };
}

}  // namespace sst::config::app
