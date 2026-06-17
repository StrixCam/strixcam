#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bluetooth.pb.h"
#include "domain/control/models/correlation-id.hpp"

namespace sst::adapters::control {

struct ChunkAssemblerConfig {
    // Bytes of `data` per outbound chunk. Sized under the negotiated MTU (512
    // requested after connect) minus ChunkedPayload + ATT framing overhead.
    static constexpr std::size_t kDefaultMaxChunkPayloadBytes = 400;
    // Cap on concurrent inbound reassemblies (correlation_ids). A flood of
    // never-completing partial messages can't grow memory without bound — the
    // oldest incomplete reassembly is evicted past this cap.
    static constexpr std::size_t kDefaultMaxInflightInbound = 16;
    // Reject absurd total_chunks up front (malformed / hostile).
    static constexpr std::uint32_t kDefaultMaxTotalChunks = 8192;

    std::size_t max_chunk_payload_bytes = kDefaultMaxChunkPayloadBytes;
    std::size_t max_inflight_inbound = kDefaultMaxInflightInbound;
    std::uint32_t max_total_chunks = kDefaultMaxTotalChunks;
};

// Pure (no D-Bus) ChunkedPayload reassembly + ChunkAck-gated outbound chunking.
// The BlueZ transport feeds inbound chunks in and drives outbound chunks out
// through this; keeping it transport-free makes the framing logic unit-testable.
//
// Inbound:  OfferInbound() accumulates chunks keyed by correlation_id and
//           returns the reassembled inner payload (a serialized Command) once
//           the final chunk arrives. Single-chunk messages return immediately.
// Outbound: BeginOutbound() splits a serialized CommandResponse and emits chunk
//           0; each ChunkAck (OnAck) releases the next chunk — never sending a
//           chunk ahead of its predecessor's ack (R3 flow control).
class ChunkAssembler {
   public:
    using SendChunkFn = std::function<void(const sst_cam::ChunkedPayload&)>;

    // Outcome of offering one inbound chunk. `accepted` is true when the chunk
    // was well-formed and either buffered or completed a reassembly (so the
    // transport may ack it); false when rejected (malformed framing:
    // index>=total, total==0 / over-cap). A duplicate of an already-seen chunk
    // is `accepted` (well-formed; the app may have retransmitted after a lost
    // ack) but contributes nothing new. `payload` holds the reassembled inner
    // bytes only on the chunk that completes the message.
    struct OfferResult {
        bool accepted{false};
        std::optional<std::string> payload;
    };

    explicit ChunkAssembler(ChunkAssemblerConfig cfg = {});

    // Buffer / reassemble one inbound chunk. Returns an OfferResult: rejected
    // (malformed) chunks are not accepted; well-formed chunks are accepted, and
    // the one completing the message carries the reassembled inner payload.
    auto OfferInbound(const sst_cam::ChunkedPayload& chunk) -> OfferResult;

    // Split `data` for `correlation_id` and emit chunk 0 via `send`. Multi-chunk
    // transfers retain state until acked; a single-chunk transfer is fire-and-
    // forget. Returns the total chunk count.
    auto BeginOutbound(const sst::control::CorrelationId& correlation_id, const std::string& data,
                       const SendChunkFn& send) -> std::uint32_t;

    // Process a ChunkAck. If it acks the most-recently-sent chunk, the next
    // chunk is emitted via the transfer's stored send fn. Returns true once the
    // whole message has been delivered and acked (transfer complete).
    auto OnAck(const std::string& correlation_id, std::uint32_t chunk_index) -> bool;

    // Drop all in-flight inbound reassemblies and pending outbound transfers.
    // Called on disconnect so a new central never sees a previous session's
    // half-assembled message or a stale outbound flow-control state, and so the
    // stored outbound send closures (which capture the transport) are released.
    auto Reset() -> void;

    [[nodiscard]] auto InflightInboundCount() const -> std::size_t { return inbound_.size(); }
    [[nodiscard]] auto PendingOutboundCount() const -> std::size_t { return outbound_.size(); }

   private:
    struct InboundState {
        std::uint32_t total_chunks{0};
        std::uint32_t received{0};
        std::vector<std::string> parts;
        std::vector<bool> have;
    };

    struct OutboundState {
        std::vector<sst_cam::ChunkedPayload> chunks;
        std::uint32_t next_to_send{0};  // index of the next not-yet-sent chunk
        SendChunkFn send;
    };

    auto EvictOldestInboundIfFull() -> void;

    ChunkAssemblerConfig cfg_;
    std::unordered_map<std::string, InboundState> inbound_;
    std::list<std::string> inbound_order_;  // insertion order, for eviction
    std::unordered_map<std::string, OutboundState> outbound_;
};

}  // namespace sst::adapters::control
