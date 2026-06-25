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
    EXPECT_EQ(cfg.device.serial_number.value_or(""), "00000001");

    // Lens calibration is present and config-sourced.
    ASSERT_TRUE(cfg.calibration.cameras.has_value());
}

// First-run self-provisioning: on a fresh device /etc/sst/cam/config is empty.
// ConfigLoader must write built-in default config files and return them rather
// than throwing, so the firmware (and its systemd service) starts out-of-box.
TEST(ConfigLoaderTest, WritesDefaultsWhenFilesMissing) {
    const fs::path root = fs::path{::testing::TempDir()} / "sst-cfg-defaults-first-run";
    fs::remove_all(root);  // start empty (isolated per run)
    fs::create_directories(root);

    sst::config::app::ConfigLoader loader(root.string(), "json");
    sst::config::ConfigData cfg;
    ASSERT_NO_THROW(cfg = loader.get());

    // Defaults were persisted to disk (editable state for later provisioning).
    EXPECT_TRUE(fs::exists(root / "device.json"));
    EXPECT_TRUE(fs::exists(root / "calibration.json"));
    EXPECT_TRUE(fs::exists(root / "storage.json"));
    EXPECT_TRUE(fs::exists(root / "wifi-direct.json"));

    // Defaults are sane + parse back through the loader.
    EXPECT_TRUE(cfg.device.name.has_value());
    ASSERT_TRUE(cfg.calibration.cameras.has_value());
    EXPECT_TRUE(cfg.wifi_direct.ssid.has_value());

    // Second load reads the written files unchanged — idempotent, no clobber.
    sst::config::app::ConfigLoader loader2(root.string(), "json");
    sst::config::ConfigData cfg2;
    ASSERT_NO_THROW(cfg2 = loader2.get());
    EXPECT_EQ(cfg2.device.name, cfg.device.name);

    fs::remove_all(root);
}
