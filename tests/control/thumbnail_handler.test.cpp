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

// JPEG SOI marker (0xFFD8) leads the canned encoder output below.
constexpr std::uint8_t kJpegMarkerFf = 0xFF;
constexpr std::uint8_t kJpegMarkerD8 = 0xD8;
// Square test-frame edge length (pixels).
constexpr std::uint32_t kFrameEdge = 8;
// Fixed capture timestamp injected into the happy-path handler.
constexpr std::uint64_t kCaptureStamp = 777;

// Requested thumbnail geometry + quality, grouped so the three same-typed
// std::uint32_t fields can't be transposed at a call site.
struct ThumbReq {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t quality;
};

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
    // Signature is fixed by the IJpegEncoder port (external header); the override
    // must mirror its three adjacent std::uint32_t params verbatim.
    auto Encode(const sst::capture::Frame& /*frame*/,
                std::uint32_t width,  // NOLINT(bugprone-easily-swappable-parameters)
                std::uint32_t height, std::uint32_t quality)
        -> std::optional<std::vector<std::uint8_t>> override {
        last_w = width;
        last_h = height;
        last_q = quality;
        if (fail) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>{kJpegMarkerFf, kJpegMarkerD8, 0x01, 0x02};
    }
};

auto ThumbnailCmd(const ThumbReq& req) -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("th-1");
    auto* thumb = cmd.mutable_thumbnail();
    thumb->set_width(req.width);
    thumb->set_height(req.height);
    thumb->set_quality(req.quality);
    return cmd;
}

auto DummyFrame() -> sst::capture::Frame {
    sst::capture::Frame frame;
    frame.geometry = {.width = kFrameEdge, .height = kFrameEdge};
    frame.format = sst::common::PixelFormat::BGR8;
    return frame;
}

// Requested thumbnail geometry used by the happy path.
constexpr ThumbReq kHappyReq{.width = 160, .height = 120, .quality = 85};

// Asserts the request geometry/quality reached the encoder verbatim. Extracted
// so the per-field EXPECTs stay out of the test's cognitive-complexity budget.
void ExpectForwardedToEncoder(const FakeEncoder& enc, const ThumbReq& req) {
    EXPECT_EQ(enc.last_w, req.width);
    EXPECT_EQ(enc.last_h, req.height);
    EXPECT_EQ(enc.last_q, req.quality);
}

// Happy path: a frame available -> OK with non-empty JPEG bytes; the request's
// width/height/quality are forwarded to the encoder.
TEST(ThumbnailHandlerTest, ReturnsJpegBytesFromLatestFrame) {
    FakeSnapshot snap;
    snap.frame = DummyFrame();
    FakeEncoder enc;
    ThumbnailHandler handler(snap, enc, [] { return kCaptureStamp; });

    auto resp = handler.Handle(ThumbnailCmd(kHappyReq));

    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
    ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kThumbnail);
    EXPECT_FALSE(resp.thumbnail().jpeg_bytes().empty());
    EXPECT_EQ(resp.thumbnail().capture_timestamp(), kCaptureStamp);
    ExpectForwardedToEncoder(enc, kHappyReq);
}

// Error path: no frame available -> ResponseStatus::ERROR with a message, not a
// crash and not an empty-OK.
TEST(ThumbnailHandlerTest, NoFrameAvailableReturnsError) {
    FakeSnapshot snap;  // frame is nullopt
    FakeEncoder enc;
    ThumbnailHandler handler(snap, enc, [] { return std::uint64_t{0}; });

    auto resp = handler.Handle(ThumbnailCmd({.width = 0, .height = 0, .quality = 0}));

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

    auto resp = handler.Handle(ThumbnailCmd({.width = 0, .height = 0, .quality = 0}));
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

    auto resp = dispatcher.Dispatch(ThumbnailCmd({.width = 0, .height = 0, .quality = 0}));
    EXPECT_NE(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
    EXPECT_EQ(resp.correlation_id(), "th-1");
    EXPECT_EQ(resp.payload_case(), sst_cam::CommandResponse::kThumbnail);
}

}  // namespace
