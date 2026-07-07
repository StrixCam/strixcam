#include "adapters/control/ble/bluez/chunk-assembler.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace sst::adapters::control {

ChunkAssembler::ChunkAssembler(ChunkAssemblerConfig cfg) : cfg_(cfg) {}

auto ChunkAssembler::EvictOldestInboundIfFull() -> void {
    while (inbound_.size() >= cfg_.max_inflight_inbound && !inbound_order_.empty()) {
        const std::string victim = inbound_order_.front();
        inbound_order_.pop_front();
        inbound_.erase(victim);
        spdlog::warn("ChunkAssembler: evicted incomplete reassembly corr={} (inflight cap {})",
                     victim, cfg_.max_inflight_inbound);
    }
}

auto ChunkAssembler::OfferInbound(const sst_cam::ChunkedPayload& chunk) -> OfferResult {
    const std::uint32_t total = chunk.total_chunks();
    const std::uint32_t index = chunk.chunk_index();
    const std::string& corr = chunk.correlation_id();

    if (total == 0 || total > cfg_.max_total_chunks) {
        spdlog::warn("ChunkAssembler: rejecting chunk corr={} total_chunks={}", corr, total);
        return {.accepted = false, .payload = std::nullopt};
    }
    if (index >= total) {
        spdlog::warn("ChunkAssembler: rejecting chunk corr={} index={} >= total={}", corr, index,
                     total);
        return {.accepted = false, .payload = std::nullopt};
    }

    // Single-chunk fast path — no reassembly state retained.
    if (total == 1) {
        return {.accepted = true, .payload = chunk.data()};
    }

    const auto fresh_state = [total] {
        InboundState state;
        state.total_chunks = total;
        state.parts.assign(total, std::string{});
        state.have.assign(total, false);
        return state;
    };
    auto entry = inbound_.find(corr);
    if (entry == inbound_.end()) {
        EvictOldestInboundIfFull();
        entry = inbound_.emplace(corr, fresh_state()).first;
        inbound_order_.push_back(corr);
    } else if (entry->second.total_chunks != total) {
        // total_chunks disagreed mid-stream — reset to the new framing.
        spdlog::warn("ChunkAssembler: corr={} total_chunks changed {}->{}, resetting", corr,
                     entry->second.total_chunks, total);
        entry->second = fresh_state();
    }

    InboundState& state = entry->second;
    if (state.have[index]) {
        // Duplicate of an already-seen chunk: well-formed, so still accepted (the
        // app may have retransmitted after a lost ack), but adds nothing.
        spdlog::debug("ChunkAssembler: duplicate chunk corr={} index={} ignored", corr, index);
        return {.accepted = true, .payload = std::nullopt};
    }

    state.parts[index] = chunk.data();
    state.have[index] = true;
    ++state.received;

    if (state.received < state.total_chunks) {
        return {.accepted = true, .payload = std::nullopt};
    }

    std::string out;
    std::size_t total_bytes = 0;
    for (const auto& part : state.parts) {
        total_bytes += part.size();
    }
    out.reserve(total_bytes);
    for (const auto& part : state.parts) {
        out += part;
    }

    inbound_.erase(entry);
    inbound_order_.remove(corr);
    return {.accepted = true, .payload = std::move(out)};
}

auto ChunkAssembler::BeginOutbound(const sst::control::CorrelationId& correlation_id,
                                   const std::string& data,
                                   const SendChunkFn& send) -> std::uint32_t {
    const std::size_t chunk_size = std::max<std::size_t>(cfg_.max_chunk_payload_bytes, 1);
    // At least one chunk, even for an empty payload.
    const std::uint32_t total =
        data.empty() ? 1U : static_cast<std::uint32_t>((data.size() + chunk_size - 1) / chunk_size);

    std::vector<sst_cam::ChunkedPayload> chunks;
    chunks.reserve(total);
    for (std::uint32_t i = 0; i < total; ++i) {
        sst_cam::ChunkedPayload chunk;
        chunk.set_correlation_id(correlation_id.value);
        chunk.set_chunk_index(i);
        chunk.set_total_chunks(total);
        const std::size_t off = static_cast<std::size_t>(i) * chunk_size;
        chunk.set_data(data.substr(off, chunk_size));
        chunks.push_back(std::move(chunk));
    }

    // Emit chunk 0 immediately.
    if (send) {
        send(chunks[0]);
    }

    // Single-chunk responses need no flow control — fire and forget.
    if (total > 1) {
        OutboundState state;
        state.chunks = std::move(chunks);
        state.next_to_send = 1;
        state.send = send;
        outbound_[correlation_id.value] = std::move(state);
    }

    return total;
}

auto ChunkAssembler::OnAck(const std::string& correlation_id, std::uint32_t chunk_index) -> bool {
    auto entry = outbound_.find(correlation_id);
    if (entry == outbound_.end()) {
        // Late / unknown ack (e.g. for a single-chunk response) — nothing to do.
        return false;
    }
    OutboundState& state = entry->second;

    const std::uint32_t last_sent = state.next_to_send - 1;
    if (chunk_index != last_sent) {
        spdlog::debug("ChunkAssembler: ignoring out-of-order ack corr={} index={} (expected {})",
                      correlation_id, chunk_index, last_sent);
        return false;
    }

    const auto total = static_cast<std::uint32_t>(state.chunks.size());
    if (state.next_to_send < total) {
        if (state.send) {
            state.send(state.chunks[state.next_to_send]);
        }
        ++state.next_to_send;
        return false;
    }

    // Last chunk was already sent and is now acked — transfer complete.
    outbound_.erase(entry);
    return true;
}

auto ChunkAssembler::Reset() -> void {
    inbound_.clear();
    inbound_order_.clear();
    outbound_.clear();
}

}  // namespace sst::adapters::control
