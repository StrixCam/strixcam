// Proto codegen smoke + round-trip coverage (U1, R26).
//
// Pure protobuf — no hardware. Proves the sst-cam-proto schema was generated
// from proto/ and that serialize/parse preserves fields, oneof cases, and bytes.
// If this links and passes, the host-protoc / target-libprotobuf ABI matches.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bluetooth.pb.h"

namespace {

constexpr std::uint32_t kCanvasWidth = 1920;
constexpr std::uint32_t kCanvasHeight = 1080;

TEST(ProtoRoundtrip, CommandPushOverlayLayoutOneofSurvives) {
    sst_cam::Command cmd;
    cmd.set_correlation_id("550e8400-e29b-41d4-a716-446655440000");
    auto* layout = cmd.mutable_push_overlay_layout()->mutable_layout();
    layout->set_canvas_width(kCanvasWidth);
    layout->set_canvas_height(kCanvasHeight);

    std::string wire;
    ASSERT_TRUE(cmd.SerializeToString(&wire));

    sst_cam::Command parsed;
    ASSERT_TRUE(parsed.ParseFromString(wire));

    EXPECT_EQ(parsed.correlation_id(), cmd.correlation_id());
    EXPECT_EQ(parsed.payload_case(), sst_cam::Command::kPushOverlayLayout);
    EXPECT_EQ(parsed.push_overlay_layout().layout().canvas_width(), kCanvasWidth);
    EXPECT_EQ(parsed.push_overlay_layout().layout().canvas_height(), kCanvasHeight);
}

TEST(ProtoRoundtrip, ChunkedPayloadPreservesBytesAndIndices) {
    constexpr std::uint32_t kTotalChunks = 5;
    sst_cam::ChunkedPayload chunk;
    chunk.set_correlation_id("corr-123");
    chunk.set_chunk_index(2);
    chunk.set_total_chunks(kTotalChunks);
    // Non-trivial bytes including embedded NULs.
    const std::string blob("\x00\x01\x02\xFF\x00payload", 12);
    chunk.set_data(blob);

    std::string wire;
    ASSERT_TRUE(chunk.SerializeToString(&wire));

    sst_cam::ChunkedPayload parsed;
    ASSERT_TRUE(parsed.ParseFromString(wire));

    EXPECT_EQ(parsed.correlation_id(), "corr-123");
    EXPECT_EQ(parsed.chunk_index(), 2U);
    EXPECT_EQ(parsed.total_chunks(), kTotalChunks);
    EXPECT_EQ(parsed.data(), blob);
}

TEST(ProtoRoundtrip, CommandResponseUnsupportedHasNoPayloadCase) {
    sst_cam::CommandResponse resp;
    resp.set_correlation_id("corr-xyz");
    resp.set_status(sst_cam::ResponseStatus::UNSUPPORTED);
    resp.set_error_message("capability not wired");

    std::string wire;
    ASSERT_TRUE(resp.SerializeToString(&wire));

    sst_cam::CommandResponse parsed;
    ASSERT_TRUE(parsed.ParseFromString(wire));

    EXPECT_EQ(parsed.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(parsed.correlation_id(), "corr-xyz");
    EXPECT_EQ(parsed.error_message(), "capability not wired");
    EXPECT_EQ(parsed.payload_case(), sst_cam::CommandResponse::PAYLOAD_NOT_SET);
}

}  // namespace
