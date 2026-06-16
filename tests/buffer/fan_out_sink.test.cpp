#include <gtest/gtest.h>

#include <vector>

#include "app/buffer/ports/frame-sink.hpp"
#include "app/buffer/services/fan_out_sink/fan-out-sink.hpp"
#include "domain/capture/models/frame.hpp"

namespace {

// Records every frame it receives — used to verify the FanOutSink forwards
// to every registered sink in order.
class RecordingSink final : public sst::buffer::IFrameSink {
   public:
    auto Push(const sst::capture::Frame& frame) -> void override {
        ++push_calls;
        last_frame_id = frame.frame_id;
    }

    int push_calls{0};
    std::uint64_t last_frame_id{0};
};

auto MakeFrame(std::uint64_t frame_id) -> sst::capture::Frame {
    constexpr std::uint32_t kWidth = 640;
    constexpr std::uint32_t kHeight = 360;
    sst::capture::Frame frame;
    frame.frame_id = frame_id;
    frame.format = sst::common::PixelFormat::BGR8;
    frame.geometry = {.width = kWidth, .height = kHeight};
    return frame;
}

TEST(FanOutSinkTest, PushesToEverySinkInOrder) {
    RecordingSink sink_a;
    RecordingSink sink_b;
    RecordingSink sink_c;
    sst::buffer::FanOutSink fanout(
        std::vector<sst::buffer::IFrameSink*>{&sink_a, &sink_b, &sink_c});

    fanout.Push(MakeFrame(1));
    fanout.Push(MakeFrame(2));

    EXPECT_EQ(sink_a.push_calls, 2);
    EXPECT_EQ(sink_b.push_calls, 2);
    EXPECT_EQ(sink_c.push_calls, 2);
    EXPECT_EQ(sink_a.last_frame_id, 2);
    EXPECT_EQ(sink_b.last_frame_id, 2);
    EXPECT_EQ(sink_c.last_frame_id, 2);
}

TEST(FanOutSinkTest, EmptySinkListIsHarmless) {
    sst::buffer::FanOutSink fanout(std::vector<sst::buffer::IFrameSink*>{});
    fanout.Push(MakeFrame(1));  // does not crash
    SUCCEED();
}

TEST(FanOutSinkTest, NullPointerEntriesAreSkipped) {
    RecordingSink sink_a;
    sst::buffer::FanOutSink fanout(
        std::vector<sst::buffer::IFrameSink*>{nullptr, &sink_a, nullptr});

    // 7 is a self-evident frame id used only to assert forwarding.
    fanout.Push(MakeFrame(7));  // NOLINT(readability-magic-numbers)

    EXPECT_EQ(sink_a.push_calls, 1);
    EXPECT_EQ(sink_a.last_frame_id, 7);  // NOLINT(readability-magic-numbers)
}

}  // namespace
