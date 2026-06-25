#pragma once

#include <cstdint>
#include <string>

#include "bluetooth.pb.h"

namespace sst::control {

// ChunkAck is wire-compatible with ChunkedPayload on fields 1/2 (correlation_id,
// chunk_index) and deliberately carries no total_chunks (field 3). A receiver
// distinguishes an ack from a real ChunkedPayload by total_chunks == 0 — a real
// payload always carries total_chunks >= 1 (proto/bluetooth.proto documents this
// convention). This named sentinel is used at every comparison/construction site
// so the magic-zero meaning stays explicit.
inline constexpr std::uint32_t kChunkAckTotalChunks = 0;

// Build the ChunkAck the firmware emits for one inbound (app->camera) command
// chunk. Pure (no D-Bus) so the per-chunk ack policy is unit-testable
// transport-free; the BLE transport serializes the result and notifies it on the
// response characteristic. Lives in the app/control layer (not the bluez
// adapter) so a future app-layer ChunkAck builder doesn't import an adapter
// header.
inline auto BuildInboundAck(const std::string& correlation_id, std::uint32_t chunk_index)
    -> sst_cam::ChunkAck {
    sst_cam::ChunkAck ack;
    ack.set_correlation_id(correlation_id);
    ack.set_chunk_index(chunk_index);
    return ack;
}

}  // namespace sst::control
