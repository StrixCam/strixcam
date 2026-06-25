// ChunkAssembler framing + ChunkAck flow control (U4, R1/R3).
//
// Pure — no D-Bus. The assembler is the transport-free core of the BLE framing;
// the BlueZ wiring around it is exercised by a hardware-bound test.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adapters/control/ble/bluez/chunk-assembler.hpp"
#include "app/control/services/chunk-ack/chunk-ack.hpp"
#include "bluetooth.pb.h"

namespace {

using sst::adapters::control::ChunkAssembler;
using sst::adapters::control::ChunkAssemblerConfig;
using sst::control::BuildInboundAck;
using sst::control::kChunkAckTotalChunks;

// Layout dimensions + element count used to build a multi-chunk Command.
constexpr std::int32_t kCanvasWidth = 1920;
constexpr std::int32_t kCanvasHeight = 1080;
constexpr int kLayoutElementCount = 40;
// Chunk payload size (bytes) the multi-chunk reassembly test splits at.
constexpr std::size_t kSplitChunkSize = 16;
// An out-of-range chunk index (>= total) used by the malformed-framing tests.
constexpr std::uint32_t kOutOfRangeIndex = 5;
// Far more partials than the in-flight cap, to exercise stale eviction.
constexpr int kOverflowPartials = 100;

// Chunk framing coordinates, grouped so the two same-typed std::uint32_t fields
// (index, total) can't be transposed at a MakeChunk call site.
struct ChunkSpan {
    std::uint32_t index;
    std::uint32_t total;
};

auto MakeChunk(const std::string& corr, ChunkSpan span,
               const std::string& data) -> sst_cam::ChunkedPayload {
    sst_cam::ChunkedPayload payload;
    payload.set_correlation_id(corr);
    payload.set_chunk_index(span.index);
    payload.set_total_chunks(span.total);
    payload.set_data(data);
    return payload;
}

// Serialize a Command and split it into N chunks of `chunk_size` bytes.
auto SplitCommand(const sst_cam::Command& cmd, const std::string& corr,
                  std::size_t chunk_size) -> std::vector<sst_cam::ChunkedPayload> {
    const std::string wire = cmd.SerializeAsString();
    std::vector<sst_cam::ChunkedPayload> out;
    const auto total = static_cast<std::uint32_t>((wire.size() + chunk_size - 1) / chunk_size);
    for (std::uint32_t i = 0; i < total; ++i) {
        out.push_back(MakeChunk(corr, {i, total}, wire.substr(i * chunk_size, chunk_size)));
    }
    return out;
}

// Offers every chunk but the last; each must be accepted as pending (no decode
// before the final chunk). Extracted so the per-chunk EXPECTs don't inflate the
// test's cognitive complexity.
void OfferAllButLast(ChunkAssembler& assembler,
                     const std::vector<sst_cam::ChunkedPayload>& chunks) {
    for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
        const auto result = assembler.OfferInbound(chunks[i]);
        EXPECT_TRUE(result.accepted) << "well-formed pending chunk should be accepted at " << i;
        EXPECT_FALSE(result.payload.has_value()) << "decoded before the final chunk at index " << i;
    }
}

// Builds a PushOverlayLayout Command large enough to span several chunks.
// Extracted from the test so its element-building loop doesn't inflate the
// test's cognitive complexity.
auto MakeMultiChunkLayoutCommand() -> sst_cam::Command {
    sst_cam::Command cmd;
    cmd.set_correlation_id("layout-1");
    auto* layout = cmd.mutable_push_overlay_layout()->mutable_layout();
    layout->set_canvas_width(kCanvasWidth);
    layout->set_canvas_height(kCanvasHeight);
    for (int i = 0; i < kLayoutElementCount; ++i) {
        auto* element = layout->add_elements();
        element->set_id("element-with-a-longish-id-" + std::to_string(i));
    }
    return cmd;
}

// R3: a multi-chunk PushOverlayLayout reassembles to a byte-identical Command,
// and no decode happens before the final chunk arrives.
TEST(ChunkAssemblerTest, MultiChunkInboundReassemblesExactly) {
    const sst_cam::Command cmd = MakeMultiChunkLayoutCommand();
    const std::string wire = cmd.SerializeAsString();

    ChunkAssembler assembler;
    auto chunks = SplitCommand(cmd, "layout-1", kSplitChunkSize);
    ASSERT_GT(chunks.size(), 1U);

    OfferAllButLast(assembler, chunks);
    const auto done = assembler.OfferInbound(chunks.back());
    EXPECT_TRUE(done.accepted);
    if (!done.payload) {
        FAIL() << "OfferInbound payload returned nullopt";
        return;
    }
    EXPECT_EQ(*done.payload, wire);

    sst_cam::Command parsed;
    ASSERT_TRUE(parsed.ParseFromString(*done.payload));
    EXPECT_EQ(parsed.push_overlay_layout().layout().elements_size(), kLayoutElementCount);
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);  // state freed on completion
}

// Single-chunk message decodes immediately, no reassembly buffer retained.
TEST(ChunkAssemblerTest, SingleChunkFastPath) {
    ChunkAssembler assembler;
    auto chunk = MakeChunk("c1", {0, 1}, "hello");
    auto done = assembler.OfferInbound(chunk);
    EXPECT_TRUE(done.accepted);
    if (!done.payload) {
        FAIL() << "OfferInbound payload returned nullopt";
        return;
    }
    EXPECT_EQ(*done.payload, "hello");
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
}

// Max outbound chunk payload (bytes) used by the gated-transfer tests.
constexpr std::size_t kMaxChunkPayloadBytes = 4;

// Acks `index` on transfer "r1" and asserts the transfer is still in flight
// (not the final chunk) and that `sent` now equals `expected`. Extracted so the
// repeated ack/observe sequence stays out of the test's complexity budget.
void ExpectAckReleasesNext(ChunkAssembler& assembler, std::uint32_t index,
                           const std::vector<std::uint32_t>& sent,
                           const std::vector<std::uint32_t>& expected) {
    EXPECT_FALSE(assembler.OnAck("r1", index));
    EXPECT_EQ(sent, expected);
}

// R3: outbound emits chunk 0, blocks until its ack, then emits chunk 1, etc.
TEST(ChunkAssemblerTest, OutboundIsGatedByChunkAck) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = kMaxChunkPayloadBytes;
    ChunkAssembler assembler(cfg);

    std::vector<std::uint32_t> sent;
    auto send = [&](const sst_cam::ChunkedPayload& chunk) { sent.push_back(chunk.chunk_index()); };

    const std::string data = "abcdefghij";  // 10 bytes -> 3 chunks of 4
    const std::uint32_t total =
        assembler.BeginOutbound(sst::control::CorrelationId{"r1"}, data, send);
    ASSERT_EQ(total, 3U);

    EXPECT_EQ(sent, (std::vector<std::uint32_t>{0}));      // only chunk 0 out so far
    ExpectAckReleasesNext(assembler, 1, sent, {0});        // wrong index releases nothing
    ExpectAckReleasesNext(assembler, 0, sent, {0, 1});     // chunk 0 ack releases chunk 1
    ExpectAckReleasesNext(assembler, 1, sent, {0, 1, 2});  // chunk 1 ack releases chunk 2

    // Acking the final chunk completes the transfer and frees state.
    EXPECT_TRUE(assembler.OnAck("r1", 2));
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);
}

// Single-chunk outbound is fire-and-forget: chunk 0 sent, no retained state.
TEST(ChunkAssemblerTest, SingleChunkOutboundNeedsNoAck) {
    ChunkAssembler assembler;
    std::vector<std::uint32_t> sent;
    auto send = [&](const sst_cam::ChunkedPayload& chunk) { sent.push_back(chunk.chunk_index()); };

    EXPECT_EQ(assembler.BeginOutbound(sst::control::CorrelationId{"r2"}, "tiny", send), 1U);
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);
    // A stray ack for an unknown transfer is harmless.
    EXPECT_FALSE(assembler.OnAck("r2", 0));
}

// Robustness: out-of-order arrival + a duplicate chunk still reassemble once.
TEST(ChunkAssemblerTest, OutOfOrderAndDuplicateInbound) {
    ChunkAssembler assembler;
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", {2, 3}, "ccc")).payload.has_value());
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", {0, 3}, "aaa")).payload.has_value());
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", {0, 3}, "aaa")).payload.has_value());  // dup
    auto done = assembler.OfferInbound(MakeChunk("x", {1, 3}, "bbb"));
    if (!done.payload) {
        FAIL() << "OfferInbound payload returned nullopt";
        return;
    }
    EXPECT_EQ(*done.payload, "aaabbbccc");
}

// #8 ack-after-validate: mirror the transport's policy transport-free. Offer the
// chunk to the assembler FIRST and ack only when it reports the chunk accepted
// (well-formed). A rejected chunk (malformed framing) is offered but NOT acked.
struct AckSink {
    std::vector<sst_cam::ChunkAck> acks;
    void OnInbound(const sst_cam::ChunkedPayload& chunk, ChunkAssembler& assembler) {
        const auto result = assembler.OfferInbound(chunk);
        if (result.accepted) {
            acks.push_back(BuildInboundAck(chunk.correlation_id(), chunk.chunk_index()));
        }
    }
};

// Verifies one ack: correct correlation/index, and that re-parsing it as a
// ChunkedPayload yields the ack sentinel total + the index (the app's
// ack-vs-response disambiguation). Extracted to keep the test under the
// cognitive-complexity cap.
void ExpectWellFormedAck(const sst_cam::ChunkAck& ack, std::uint32_t index) {
    EXPECT_EQ(ack.correlation_id(), "big");
    EXPECT_EQ(ack.chunk_index(), index);
    sst_cam::ChunkedPayload as_payload;
    ASSERT_TRUE(as_payload.ParseFromString(ack.SerializeAsString()));
    EXPECT_EQ(as_payload.total_chunks(), kChunkAckTotalChunks);
    EXPECT_EQ(as_payload.chunk_index(), index);
}

TEST(ChunkAssemblerTest, ThreeChunkInboundProducesThreeAcks) {
    ChunkAssembler assembler;
    AckSink sink;

    const std::vector<sst_cam::ChunkedPayload> chunks = {MakeChunk("big", {0, 3}, "aaa"),
                                                         MakeChunk("big", {1, 3}, "bbb"),
                                                         MakeChunk("big", {2, 3}, "ccc")};
    for (const auto& chunk : chunks) {
        sink.OnInbound(chunk, assembler);
    }

    ASSERT_EQ(sink.acks.size(), 3U);
    for (std::uint32_t i = 0; i < 3; ++i) {
        ExpectWellFormedAck(sink.acks[i], i);
    }
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);  // reassembled + freed
}

// Single-chunk inbound still acks exactly once and reassembles via the fast path.
TEST(ChunkAssemblerTest, SingleChunkInboundAcksOnce) {
    ChunkAssembler assembler;
    AckSink sink;
    auto chunk = MakeChunk("one", {0, 1}, "payload");
    sink.OnInbound(chunk, assembler);

    ASSERT_EQ(sink.acks.size(), 1U);
    EXPECT_EQ(sink.acks[0].correlation_id(), "one");
    EXPECT_EQ(sink.acks[0].chunk_index(), 0U);
}

// A duplicate inbound chunk is still acked (the app may have retransmitted after
// a lost ack), and the assembler dedups it so the message reassembles once.
TEST(ChunkAssemblerTest, DuplicateInboundIsStillAcked) {
    ChunkAssembler assembler;
    AckSink sink;

    sink.OnInbound(MakeChunk("d", {0, 2}, "aa"), assembler);
    sink.OnInbound(MakeChunk("d", {0, 2}, "aa"), assembler);  // duplicate index 0
    sink.OnInbound(MakeChunk("d", {1, 2}, "bb"), assembler);

    EXPECT_EQ(sink.acks.size(), 3U);            // every accepted write is acked, dup included
    EXPECT_EQ(sink.acks[1].chunk_index(), 0U);  // dup ack well-formed
}

// #8: a rejected chunk (malformed framing) is offered to the assembler but must
// NOT be acked — only chunks the assembler accepts get an ack. A valid chunk
// before/after the rejects is still acked exactly once each.
TEST(ChunkAssemblerTest, RejectedChunksAreNotAcked) {
    ChunkAssembler assembler;
    AckSink sink;

    // index >= total and total == 0 are both rejected -> no ack.
    sink.OnInbound(MakeChunk("r", {kOutOfRangeIndex, 3}, "x"), assembler);  // index>=total
    sink.OnInbound(MakeChunk("r", {0, 0}, "x"), assembler);                 // total 0
    EXPECT_EQ(sink.acks.size(), 0U) << "rejected chunks must not be acked";

    // A well-formed single-chunk message that follows is accepted and acked once.
    sink.OnInbound(MakeChunk("ok", {0, 1}, "payload"), assembler);
    ASSERT_EQ(sink.acks.size(), 1U);
    EXPECT_EQ(sink.acks[0].correlation_id(), "ok");
}

// Malformed framing is rejected without crashing or retaining state.
TEST(ChunkAssemblerTest, MalformedChunksRejected) {
    ChunkAssembler assembler;
    auto total0 = assembler.OfferInbound(MakeChunk("m", {0, 0}, "x"));  // total 0
    EXPECT_FALSE(total0.accepted);
    EXPECT_FALSE(total0.payload.has_value());
    auto over = assembler.OfferInbound(MakeChunk("m", {kOutOfRangeIndex, 3}, "x"));  // index>=total
    EXPECT_FALSE(over.accepted);
    EXPECT_FALSE(over.payload.has_value());
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
}

// #9: disconnect must drop all assembler state — in-flight inbound reassemblies
// AND pending outbound transfers — so a new central never inherits a previous
// session's half-assembled message or stale flow-control. Reset() is what the
// transport calls on its disconnect/Stop path.
// Asserts the assembler holds no inbound or outbound state. Extracted so the
// Reset test's pre/post assertions stay under the cognitive-complexity cap.
void ExpectNoRetainedState(const ChunkAssembler& assembler) {
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);
}

TEST(ChunkAssemblerTest, ResetClearsInboundAndOutboundState) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = kMaxChunkPayloadBytes;
    ChunkAssembler assembler(cfg);

    // A partial inbound reassembly (first of two chunks, never completes).
    EXPECT_TRUE(assembler.OfferInbound(MakeChunk("in", {0, 2}, "aa")).accepted);
    EXPECT_EQ(assembler.InflightInboundCount(), 1U);

    // A multi-chunk outbound transfer awaiting acks.
    auto send = [](const sst_cam::ChunkedPayload&) {};
    ASSERT_EQ(assembler.BeginOutbound(sst::control::CorrelationId{"out"}, "abcdefghij", send), 3U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 1U);

    assembler.Reset();

    ExpectNoRetainedState(assembler);

    // A late ack for the dropped transfer is a harmless no-op (no retained send).
    EXPECT_FALSE(assembler.OnAck("out", 0));
}

// Stand-in for the transport's GATT app: counts Notify() calls so a test can
// observe the send closure firing. Namespace-scoped (not a local struct) so the
// closure test body stays under the cognitive-complexity cap.
struct FakeGatt {
    int notifications{0};
    void Notify() { ++notifications; }
};

// Builds the transport's guarded send closure over a pointer-to-unique_ptr, so
// it observes a later reset() exactly like gatt_app_ after Stop(). The null
// guard lives here (not the test body) to keep the test under the cap.
auto MakeGuardedSend(std::unique_ptr<FakeGatt>* gatt_ptr) {
    return [gatt_ptr](const sst_cam::ChunkedPayload&) {
        if (*gatt_ptr) {
            (*gatt_ptr)->Notify();
        }
    };
}

// #9 null-guard: the outbound send closure the transport stores captures the
// GATT app and is invoked later by OnAck. After Stop() that app is null, so the
// real closure guards the deref. Mirror that transport-free: a closure whose
// backing target has been nulled must be safe to drive via OnAck (no crash).
TEST(ChunkAssemblerTest, OutboundSendClosureToleratesNulledTarget) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = kMaxChunkPayloadBytes;
    ChunkAssembler assembler(cfg);

    auto gatt = std::make_unique<FakeGatt>();
    auto send = MakeGuardedSend(&gatt);

    ASSERT_EQ(assembler.BeginOutbound(sst::control::CorrelationId{"g"}, "abcdefgh", send),
              2U);                      // 8 bytes / 4 -> 2 chunks
    EXPECT_EQ(gatt->notifications, 1);  // chunk 0 sent immediately

    // Simulate Stop(): the GATT app is destroyed. The retained closure must not
    // deref a dangling/null target when the next chunk is released.
    gatt.reset();
    EXPECT_NO_THROW({ (void)assembler.OnAck("g", 0); });  // releases chunk 1 -> guarded no-op
}

// Never-completing reassemblies are evicted past the in-flight cap — no leak.
TEST(ChunkAssemblerTest, InflightCapEvictsStalePartials) {
    ChunkAssemblerConfig cfg;
    cfg.max_inflight_inbound = 4;
    ChunkAssembler assembler(cfg);

    for (int i = 0; i < kOverflowPartials; ++i) {
        // Each is the first of two chunks and never completes.
        assembler.OfferInbound(MakeChunk("corr-" + std::to_string(i), {0, 2}, "partial"));
    }
    EXPECT_LE(assembler.InflightInboundCount(), cfg.max_inflight_inbound);
}

}  // namespace
