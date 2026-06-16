// Thumbnail handler: latest frame -> JPEG bytes (U9, R6).
// Pure — fake snapshot source + fake encoder; no pipeline, no hardware. The
// real encode path is covered by JpegEncoderTest; here we assert the handler's
// wiring and the no-frame error path (CLAUDE.md: a real ERROR, not a skeleton).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/thumbnail.handler.hpp"
#include "app/pipeline/ports/frame-snapshot-source.hpp"
#include "app/storage/ports/jpeg-encoder.hpp"
#include "bluetooth.pb.h"
#include "domain/capture/models/frame.hpp"

namespace {

using sst::control::ThumbnailHandler;

// A snapshot source that hands back a preset frame (or nothing).
class FakeSnapshot final : public sst::pipeline::IFrameSnapshotSource {
   public:
    std::optional<sst::capture::Frame> frame;
    auto GrabLatest() -> std::optional<sst::capture::Frame> override { return frame; }
};

// An encoder that records the requested size and returns canned bytes (or fails).
class FakeEncoder final : public sst::storage::IJpegEncoder {
   public:
    bool fail{false};
    std::uint32_t last_w{0}, last_h{0}, last_q{0};
    auto Encode(const sst::capture::Frame& /*frame*/, std::uint32_t w,
                std::uint32_t h, std::uint32_t q)
        -> std::optional<std::vector<std::uint8_t>> override {
      last_w = w;
      last_h = h;
      last_q = q;
      if (fail) {
        return std::nullopt;
      }
      return std::vector<std::uint8_t>{0xFF, 0xD8, 0x01, 0x02};
    }
};

auto ThumbnailCmd(std::uint32_t w, std::uint32_t h, std::uint32_t q) -> sst_cam::Command {
    sst_cam::Command c;
    c.set_correlation_id("th-1");
    auto* t = c.mutable_thumbnail();
    t->set_width(w);
    t->set_height(h);
    t->set_quality(q);
    return c;
}

auto DummyFrame() -> sst::capture::Frame {
    sst::capture::Frame f;
    f.geometry = {.width = 8, .height = 8};
    f.format = sst::common::PixelFormat::BGR8;
    return f;
}

// Happy path: a frame available -> OK with non-empty JPEG bytes; the request's
// width/height/quality are forwarded to the encoder.
TEST(ThumbnailHandlerTest, ReturnsJpegBytesFromLatestFrame) {
    FakeSnapshot snap;
    snap.frame = DummyFrame();
    FakeEncoder enc;
    ThumbnailHandler handler(snap, enc, [] { return std::uint64_t{777}; });

    auto resp = handler.Handle(ThumbnailCmd(160, 120, 85));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kThumbnail);
    EXPECT_FALSE(resp.thumbnail().jpeg_bytes().empty());
    EXPECT_EQ(resp.thumbnail().capture_timestamp(), 777U);
    EXPECT_EQ(enc.last_w, 160U);
    EXPECT_EQ(enc.last_h, 120U);
    EXPECT_EQ(enc.last_q, 85U);
}

// Error path: no frame available -> ResponseStatus::ERROR with a message, not a
// crash and not an empty-OK.
TEST(ThumbnailHandlerTest, NoFrameAvailableReturnsError) {
    FakeSnapshot snap;  // frame is nullopt
    FakeEncoder enc;
    ThumbnailHandler handler(snap, enc, [] { return std::uint64_t{0}; });

    auto resp = handler.Handle(ThumbnailCmd(0, 0, 0));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(resp.error_message().empty());
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::PAYLOAD_NOT_SET);
}

// Error path: encoder failure -> ERROR (not an empty-OK).
TEST(ThumbnailHandlerTest, EncodeFailureReturnsError) {
    FakeSnapshot snap;
    snap.frame = DummyFrame();
    FakeEncoder enc;
    enc.fail = true;
    ThumbnailHandler handler(snap, enc, [] { return std::uint64_t{0}; });

    auto resp = handler.Handle(ThumbnailCmd(0, 0, 0));
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
}

// Dispatcher parity: thumbnail no longer falls through to UNSUPPORTED once the
// handler is registered.
TEST(ThumbnailHandlerTest, DispatcherRoutesThumbnail) {
    FakeSnapshot snap;
    snap.frame = DummyFrame();
    FakeEncoder enc;
    sst::control::CommandDispatcher dispatcher;
    dispatcher.Register(
        std::make_shared<ThumbnailHandler>(snap, enc, [] { return std::uint64_t{0}; }));

    auto resp = dispatcher.Dispatch(ThumbnailCmd(0, 0, 0));
    EXPECT_NE(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "th-1");
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::kThumbnail);
}

}  // namespace
