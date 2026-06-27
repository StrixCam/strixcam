// Unit tests for the set-preview-layout handler (#6 F6d): flips the shared
// PreviewLayoutState and replies with the preview stream geometry.
#include <gtest/gtest.h>

#include <cstdint>

#include "app/control/services/handlers/preview-layout.handler.hpp"
#include "bluetooth.pb.h"
#include "domain/streaming/models/preview-layout.hpp"

namespace {

constexpr std::uint32_t kStreamW = 1280;
constexpr std::uint32_t kStreamH = 720;

auto LayoutCmd(sst_cam::PreviewLayout layout) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.mutable_set_preview_layout()->set_layout(layout);
    return cmd;
}

}  // namespace

TEST(PreviewLayoutHandlerTest, HandlesSetPreviewLayoutCase) {
    sst::streaming::PreviewLayoutState state;
    sst::control::PreviewLayoutHandler handler(state, kStreamW, kStreamH);
    const auto cases = handler.HandledCases();
    ASSERT_EQ(cases.size(), 1U);
    EXPECT_EQ(cases[0], sst_cam::Command::kSetPreviewLayout);
}

TEST(PreviewLayoutHandlerTest, SideBySideSetsStateAndReportsGeometry) {
    sst::streaming::PreviewLayoutState state;
    sst::control::PreviewLayoutHandler handler(state, kStreamW, kStreamH);

    const auto resp =
        handler.Handle(LayoutCmd(sst_cam::PreviewLayout::PREVIEW_LAYOUT_SIDE_BY_SIDE));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_TRUE(resp.has_preview_layout());
    EXPECT_EQ(resp.preview_layout().layout(), sst_cam::PreviewLayout::PREVIEW_LAYOUT_SIDE_BY_SIDE);
    EXPECT_EQ(resp.preview_layout().width(), kStreamW);
    EXPECT_EQ(resp.preview_layout().height(), kStreamH);
    EXPECT_EQ(state.Get(), sst::streaming::PreviewLayout::kSideBySide);
}

TEST(PreviewLayoutHandlerTest, SingleResetsStateBackToSingle) {
    sst::streaming::PreviewLayoutState state;
    sst::control::PreviewLayoutHandler handler(state, kStreamW, kStreamH);

    handler.Handle(LayoutCmd(sst_cam::PreviewLayout::PREVIEW_LAYOUT_SIDE_BY_SIDE));
    const auto resp = handler.Handle(LayoutCmd(sst_cam::PreviewLayout::PREVIEW_LAYOUT_SINGLE));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    EXPECT_EQ(resp.preview_layout().layout(), sst_cam::PreviewLayout::PREVIEW_LAYOUT_SINGLE);
    EXPECT_EQ(state.Get(), sst::streaming::PreviewLayout::kSingle);
}
