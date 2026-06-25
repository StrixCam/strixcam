#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <filesystem>

#include "app/config/services/config_loader/config-loader.hpp"
#include "domain/config/models/config-data.hpp"
#include "domain/config/models/formatter/_fmt.hpp"  // IWYU pragma: keep

namespace fs = std::filesystem;

namespace {
constexpr const char* kConfigDir = "tests/config/config_files";
}  // namespace

TEST(ConfigLoaderTest, LoadAndLog) {
    const fs::path root = fs::path{SST_REPO_ROOT_DIR} / kConfigDir;
    sst::config::app::ConfigLoader loader(root.string(), "json");
    sst::config::ConfigData cfg = loader.get();
    spdlog::info("ConfigData:\n{}", cfg);
}

// R10 / KTD8: after the app-as-source-of-truth refactor, config files are the
// ONLY persistent state. ConfigLoader must yield device identity + lens
// calibration with no database present — nothing seeds it, nothing reads SQLite.
TEST(ConfigLoaderTest, YieldsDeviceIdentityAndCalibrationWithoutDb) {
    const fs::path root = fs::path{SST_REPO_ROOT_DIR} / kConfigDir;
    sst::config::app::ConfigLoader loader(root.string(), "json");
    sst::config::ConfigData cfg = loader.get();

    // Device identity (drives the sst-cam-NNNN BLE name + DeviceInfoResponse).
    EXPECT_TRUE(cfg.device.name.has_value());
    EXPECT_TRUE(cfg.device.model.has_value());
    EXPECT_TRUE(cfg.device.serial_number.has_value());
    EXPECT_EQ(cfg.device.serial_number.value(), "00000001");

    // Lens calibration is present and config-sourced.
    ASSERT_TRUE(cfg.calibration.cameras.has_value());
}
