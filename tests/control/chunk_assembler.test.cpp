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

auto MakeChunk(const std::string& corr, std::uint32_t index, std::uint32_t total,
               const std::string& data) -> sst_cam::ChunkedPayload {
    sst_cam::ChunkedPayload c;
    c.set_correlation_id(corr);
    c.set_chunk_index(index);
    c.set_total_chunks(total);
    c.set_data(data);
    return c;
}

// Serialize a Command and split it into N chunks of `chunk_size` bytes.
auto SplitCommand(const sst_cam::Command& cmd, const std::string& corr, std::size_t chunk_size)
    -> std::vector<sst_cam::ChunkedPayload> {
    const std::string wire = cmd.SerializeAsString();
    std::vector<sst_cam::ChunkedPayload> out;
    const auto total =
        static_cast<std::uint32_t>((wire.size() + chunk_size - 1) / chunk_size);
    for (std::uint32_t i = 0; i < total; ++i) {
        out.push_back(MakeChunk(corr, i, total, wire.substr(i * chunk_size, chunk_size)));
    }
    return out;
}

// R3: a multi-chunk PushOverlayLayout reassembles to a byte-identical Command,
// and no decode happens before the final chunk arrives.
TEST(ChunkAssemblerTest, MultiChunkInboundReassemblesExactly) {
    sst_cam::Command cmd;
    cmd.set_correlation_id("layout-1");
    auto* layout = cmd.mutable_push_overlay_layout()->mutable_layout();
    layout->set_canvas_width(1920);
    layout->set_canvas_height(1080);
    for (int i = 0; i < 40; ++i) {
        auto* el = layout->add_elements();
        el->set_id("element-with-a-longish-id-" + std::to_string(i));
    }
    const std::string wire = cmd.SerializeAsString();

    ChunkAssembler assembler;
    auto chunks = SplitCommand(cmd, "layout-1", 16);
    ASSERT_GT(chunks.size(), 1U);

    for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
        const auto r = assembler.OfferInbound(chunks[i]);
        EXPECT_TRUE(r.accepted) << "well-formed pending chunk should be accepted at " << i;
        EXPECT_FALSE(r.payload.has_value()) << "decoded before the final chunk at index " << i;
    }
    const auto done = assembler.OfferInbound(chunks.back());
    EXPECT_TRUE(done.accepted);
    ASSERT_TRUE(done.payload.has_value());
    EXPECT_EQ(*done.payload, wire);

    sst_cam::Command parsed;
    ASSERT_TRUE(parsed.ParseFromString(*done.payload));
    EXPECT_EQ(parsed.push_overlay_layout().layout().elements_size(), 40);
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);  // state freed on completion
}

// Single-chunk message decodes immediately, no reassembly buffer retained.
TEST(ChunkAssemblerTest, SingleChunkFastPath) {
    ChunkAssembler assembler;
    auto chunk = MakeChunk("c1", 0, 1, "hello");
    auto done = assembler.OfferInbound(chunk);
    EXPECT_TRUE(done.accepted);
    ASSERT_TRUE(done.payload.has_value());
    EXPECT_EQ(*done.payload, "hello");
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
}

// R3: outbound emits chunk 0, blocks until its ack, then emits chunk 1, etc.
TEST(ChunkAssemblerTest, OutboundIsGatedByChunkAck) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = 4;
    ChunkAssembler assembler(cfg);

    std::vector<std::uint32_t> sent;
    auto send = [&](const sst_cam::ChunkedPayload& c) { sent.push_back(c.chunk_index()); };

    const std::string data = "abcdefghij";  // 10 bytes -> 3 chunks of 4
    const std::uint32_t total = assembler.BeginOutbound("r1", data, send);
    ASSERT_EQ(total, 3U);

    // Only chunk 0 is out so far.
    EXPECT_EQ(sent, (std::vector<std::uint32_t>{0}));

    // Acking the wrong (not-yet-sent) index must not release anything.
    EXPECT_FALSE(assembler.OnAck("r1", 1));
    EXPECT_EQ(sent, (std::vector<std::uint32_t>{0}));

    EXPECT_FALSE(assembler.OnAck("r1", 0));
    EXPECT_EQ(sent, (std::vector<std::uint32_t>{0, 1}));

    EXPECT_FALSE(assembler.OnAck("r1", 1));
    EXPECT_EQ(sent, (std::vector<std::uint32_t>{0, 1, 2}));

    // Acking the final chunk completes the transfer and frees state.
    EXPECT_TRUE(assembler.OnAck("r1", 2));
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);
}

// Single-chunk outbound is fire-and-forget: chunk 0 sent, no retained state.
TEST(ChunkAssemblerTest, SingleChunkOutboundNeedsNoAck) {
    ChunkAssembler assembler;
    std::vector<std::uint32_t> sent;
    auto send = [&](const sst_cam::ChunkedPayload& c) { sent.push_back(c.chunk_index()); };

    EXPECT_EQ(assembler.BeginOutbound("r2", "tiny", send), 1U);
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);
    // A stray ack for an unknown transfer is harmless.
    EXPECT_FALSE(assembler.OnAck("r2", 0));
}

// Robustness: out-of-order arrival + a duplicate chunk still reassemble once.
TEST(ChunkAssemblerTest, OutOfOrderAndDuplicateInbound) {
    ChunkAssembler assembler;
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", 2, 3, "ccc")).payload.has_value());
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", 0, 3, "aaa")).payload.has_value());
    EXPECT_FALSE(assembler.OfferInbound(MakeChunk("x", 0, 3, "aaa")).payload.has_value());  // dup
    auto done = assembler.OfferInbound(MakeChunk("x", 1, 3, "bbb"));
    ASSERT_TRUE(done.payload.has_value());
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

TEST(ChunkAssemblerTest, ThreeChunkInboundProducesThreeAcks) {
    ChunkAssembler assembler;
    AckSink sink;

    const std::vector<sst_cam::ChunkedPayload> chunks = {MakeChunk("big", 0, 3, "aaa"),
                                                         MakeChunk("big", 1, 3, "bbb"),
                                                         MakeChunk("big", 2, 3, "ccc")};
    for (const auto& c : chunks) {
        sink.OnInbound(c, assembler);
    }

    ASSERT_EQ(sink.acks.size(), 3U);
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(sink.acks[i].correlation_id(), "big");
        EXPECT_EQ(sink.acks[i].chunk_index(), i);
        // Wire-compatible ack: re-parsed as a ChunkedPayload it has no
        // total_chunks (==0), the app's ack-vs-response disambiguation.
        sst_cam::ChunkedPayload as_payload;
        ASSERT_TRUE(as_payload.ParseFromString(sink.acks[i].SerializeAsString()));
        EXPECT_EQ(as_payload.total_chunks(), kChunkAckTotalChunks);
        EXPECT_EQ(as_payload.chunk_index(), i);
    }
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);  // reassembled + freed
}

// Single-chunk inbound still acks exactly once and reassembles via the fast path.
TEST(ChunkAssemblerTest, SingleChunkInboundAcksOnce) {
    ChunkAssembler assembler;
    AckSink sink;
    auto chunk = MakeChunk("one", 0, 1, "payload");
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

    sink.OnInbound(MakeChunk("d", 0, 2, "aa"), assembler);
    sink.OnInbound(MakeChunk("d", 0, 2, "aa"), assembler);  // duplicate index 0
    sink.OnInbound(MakeChunk("d", 1, 2, "bb"), assembler);

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
    sink.OnInbound(MakeChunk("r", 5, 3, "x"), assembler);  // index>=total
    sink.OnInbound(MakeChunk("r", 0, 0, "x"), assembler);  // total 0
    EXPECT_EQ(sink.acks.size(), 0U) << "rejected chunks must not be acked";

    // A well-formed single-chunk message that follows is accepted and acked once.
    sink.OnInbound(MakeChunk("ok", 0, 1, "payload"), assembler);
    ASSERT_EQ(sink.acks.size(), 1U);
    EXPECT_EQ(sink.acks[0].correlation_id(), "ok");
}

// Malformed framing is rejected without crashing or retaining state.
TEST(ChunkAssemblerTest, MalformedChunksRejected) {
    ChunkAssembler assembler;
    auto total0 = assembler.OfferInbound(MakeChunk("m", 0, 0, "x"));  // total 0
    EXPECT_FALSE(total0.accepted);
    EXPECT_FALSE(total0.payload.has_value());
    auto over = assembler.OfferInbound(MakeChunk("m", 5, 3, "x"));  // index>=total
    EXPECT_FALSE(over.accepted);
    EXPECT_FALSE(over.payload.has_value());
    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
}

// #9: disconnect must drop all assembler state — in-flight inbound reassemblies
// AND pending outbound transfers — so a new central never inherits a previous
// session's half-assembled message or stale flow-control. Reset() is what the
// transport calls on its disconnect/Stop path.
TEST(ChunkAssemblerTest, ResetClearsInboundAndOutboundState) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = 4;
    ChunkAssembler assembler(cfg);

    // A partial inbound reassembly (first of two chunks, never completes).
    EXPECT_TRUE(assembler.OfferInbound(MakeChunk("in", 0, 2, "aa")).accepted);
    EXPECT_EQ(assembler.InflightInboundCount(), 1U);

    // A multi-chunk outbound transfer awaiting acks.
    auto send = [](const sst_cam::ChunkedPayload&) {};
    ASSERT_EQ(assembler.BeginOutbound("out", "abcdefghij", send), 3U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 1U);

    assembler.Reset();

    EXPECT_EQ(assembler.InflightInboundCount(), 0U);
    EXPECT_EQ(assembler.PendingOutboundCount(), 0U);

    // A late ack for the dropped transfer is a harmless no-op (no retained send).
    EXPECT_FALSE(assembler.OnAck("out", 0));
}

// #9 null-guard: the outbound send closure the transport stores captures the
// GATT app and is invoked later by OnAck. After Stop() that app is null, so the
// real closure guards the deref. Mirror that transport-free: a closure whose
// backing target has been nulled must be safe to drive via OnAck (no crash).
TEST(ChunkAssemblerTest, OutboundSendClosureToleratesNulledTarget) {
    ChunkAssemblerConfig cfg;
    cfg.max_chunk_payload_bytes = 4;
    ChunkAssembler assembler(cfg);

    struct FakeGatt {
        int notifications{0};
        void Notify() { ++notifications; }
    };
    auto gatt = std::make_unique<FakeGatt>();

    // Capture a pointer-to-unique_ptr so the closure observes the later reset(),
    // exactly like the transport's gatt_app_ member after Stop().
    auto* gatt_ptr = &gatt;
    auto send = [gatt_ptr](const sst_cam::ChunkedPayload&) {
        if (*gatt_ptr) {
            (*gatt_ptr)->Notify();
        }
    };

    ASSERT_EQ(assembler.BeginOutbound("g", "abcdefgh", send), 2U);  // 8 bytes / 4 -> 2 chunks
    EXPECT_EQ(gatt->notifications, 1);                              // chunk 0 sent immediately

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

    for (int i = 0; i < 100; ++i) {
        // Each is the first of two chunks and never completes.
        assembler.OfferInbound(MakeChunk("corr-" + std::to_string(i), 0, 2, "partial"));
    }
    EXPECT_LE(assembler.InflightInboundCount(), cfg.max_inflight_inbound);
}

}  // namespace
