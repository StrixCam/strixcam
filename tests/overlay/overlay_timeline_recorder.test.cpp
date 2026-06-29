// Unit tests for the overlay timeline recorder (#6 F6b) + its RenderScene serde.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "adapters/overlay/timeline/filesystem-overlay-timeline-recorder.hpp"
#include "adapters/overlay/timeline/serde/render-scene-serde.hpp"
#include "domain/overlay/models/overlay-enums.hpp"
#include "domain/overlay/models/render-scene.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kCanvasW = 1280;
constexpr std::uint32_t kCanvasH = 720;
constexpr std::uint64_t kAnchorMs = 100'000;
constexpr float kOpacity = 0.96F;
constexpr float kFontSize = 30.0F;
constexpr float kX2 = 464.0F;
constexpr float kY2 = 76.0F;
constexpr std::uint32_t kZIndex = 12;
constexpr std::uint64_t kTickMs = 1000;

// A small two-element scene: a background rect + a resolved score text.
auto MakeScene(const std::string& score_text) -> sst::overlay::RenderScene {
    sst::overlay::RenderScene scene;
    scene.canvas_width = kCanvasW;
    scene.canvas_height = kCanvasH;

    sst::overlay::RenderElement background;
    background.shape = sst::overlay::OverlayShape::kRect;
    background.bounds = {0.0F, 0.0F, kX2, kY2, 0};
    background.style.fill_color = "#14161A";
    background.style.opacity = kOpacity;
    scene.elements.push_back(background);

    sst::overlay::RenderElement score;
    score.shape = sst::overlay::OverlayShape::kText;
    score.bounds = {0.0F, 0.0F, kX2, kY2, kZIndex};
    score.style.text_color = "#FFFFFF";
    score.style.font_size = kFontSize;
    score.style.font_weight = sst::overlay::FontWeight::kBold;
    score.style.text_align = sst::overlay::TextAlign::kCenter;
    score.text = score_text;  // already resolved (binding evaluated)
    scene.elements.push_back(score);

    return scene;
}

}  // namespace

TEST(RenderSceneSerdeTest, RoundTripsAllFields) {
    const auto scene = MakeScene("1 - 0");
    const nlohmann::json encoded = scene;
    const auto decoded = encoded.get<sst::overlay::RenderScene>();

    EXPECT_EQ(decoded.canvas_width, kCanvasW);
    EXPECT_EQ(decoded.canvas_height, kCanvasH);
    ASSERT_EQ(decoded.elements.size(), scene.elements.size());

    const auto& text_el = decoded.elements.at(1);
    EXPECT_EQ(text_el.text, "1 - 0");
    EXPECT_EQ(text_el.shape, sst::overlay::OverlayShape::kText);
    EXPECT_EQ(text_el.style.text_color, "#FFFFFF");
    EXPECT_EQ(text_el.style.font_weight, sst::overlay::FontWeight::kBold);
    EXPECT_EQ(text_el.style.text_align, sst::overlay::TextAlign::kCenter);
    EXPECT_FLOAT_EQ(text_el.style.font_size, kFontSize);
    EXPECT_EQ(text_el.bounds.z, kZIndex);
}

class OverlayTimelineRecorderTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("sst-tl-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::create_directories(dir_);
        timeline_ = dir_ / (dir_.filename().string() + ".timeline.json");
        fs::remove(timeline_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
    fs::path timeline_;
};

TEST_F(OverlayTimelineRecorderTest, WritesAnchorAndScenesOnStop) {
    sst::adapters::overlay::FilesystemOverlayTimelineRecorder recorder;
    recorder.Start(dir_, kAnchorMs);
    recorder.OnScene(kAnchorMs, MakeScene("0 - 0"));
    recorder.OnScene(kAnchorMs + kTickMs, MakeScene("1 - 0"));
    recorder.Stop();

    ASSERT_TRUE(fs::exists(timeline_));
    std::ifstream file(timeline_);
    const auto doc = nlohmann::json::parse(file);

    EXPECT_EQ(doc.at("anchor_ms").get<std::uint64_t>(), kAnchorMs);
    ASSERT_EQ(doc.at("events").size(), 2U);
    EXPECT_EQ(doc.at("events").at(0).at("at_ms").get<std::uint64_t>(), kAnchorMs);
    const auto scene1 = doc.at("events").at(1).at("scene").get<sst::overlay::RenderScene>();
    EXPECT_EQ(scene1.elements.at(1).text, "1 - 0");
}

TEST_F(OverlayTimelineRecorderTest, OnSceneBeforeStartIsNoOp) {
    sst::adapters::overlay::FilesystemOverlayTimelineRecorder recorder;
    recorder.OnScene(kAnchorMs, MakeScene("9 - 9"));  // not started
    recorder.Stop();
    EXPECT_FALSE(fs::exists(timeline_));
}

TEST_F(OverlayTimelineRecorderTest, StopWithNoScenesWritesNothing) {
    sst::adapters::overlay::FilesystemOverlayTimelineRecorder recorder;
    recorder.Start(dir_, kAnchorMs);
    recorder.Stop();
    EXPECT_FALSE(fs::exists(timeline_));
}
