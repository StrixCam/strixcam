#pragma once

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

#include "adapters/config/json/serde/calibration.hpp"    // IWYU pragma: keep
#include "adapters/config/json/serde/device.hpp"         // IWYU pragma: keep
#include "adapters/config/json/serde/storage.hpp"        // IWYU pragma: keep
#include "adapters/config/json/serde/uplink-config.hpp"  // IWYU pragma: keep
#include "adapters/config/json/serde/wifi-direct.hpp"    // IWYU pragma: keep
#include "app/config/ports/config-reader.hpp"

namespace sst::config {

using nlohmann::json;
using sst::config::ConfigReturn;
using sst::config::IConfigFileReaderAdapter;

template <typename T>
class JsonReaderAdapter : public IConfigFileReaderAdapter<T> {
   public:
    explicit JsonReaderAdapter(std::filesystem::path full_path)
        : full_path_{std::move(full_path)} {}

    auto load() -> ConfigReturn<T> override {
        try {
            spdlog::debug("Loading JSON config from: {}", full_path_.string());

            std::ifstream inputFileStream(full_path_, std::ios::in);
            if (!inputFileStream.is_open()) {
                spdlog::error("Cannot open config file: {}", full_path_.string());
                return ConfigReturn<T>::fail();
            }

            json jsonData;
            inputFileStream >> jsonData;

            T loadedConfig = jsonData.get<T>();
            spdlog::debug("Loaded JSON config OK: {}", full_path_.string());

            return ConfigReturn<T>::ok(std::move(loadedConfig));

        } catch (const json::exception& ex) {
            spdlog::error("Failed to parse JSON config: {} (file: {})", ex.what(),
                          full_path_.string());
            return ConfigReturn<T>::fail();
        } catch (const std::exception& ex) {
            spdlog::error("Failed to load JSON config: {} (file: {})", ex.what(),
                          full_path_.string());
            return ConfigReturn<T>::fail();
        }
    }

   private:
    std::filesystem::path full_path_;
};

}  // namespace sst::config
