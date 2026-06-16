#pragma once

#include <functional>

#include "bluetooth.pb.h"

namespace sst::control {

// Peripheral-side BLE transport. The adapter advertises a GATT service, exposes
// a Command-Write characteristic the phone writes to, and a Command-Response
// characteristic that emits notifications back. Inbound writes are reassembled
// from ChunkedPayload framing into a Command; the transport invokes the handler
// and chunks the returned CommandResponse back out, gated by ChunkAck writes.
//
// The handler is synchronous and always returns exactly one CommandResponse
// (the dispatcher guarantees this, KTD3) — the transport owns the framing, not
// the response policy.
class IBleTransport {
   public:
    using CommandHandler = std::function<sst_cam::CommandResponse(const sst_cam::Command&)>;
    using ConnectionHandler = std::function<void()>;

    virtual ~IBleTransport() = default;

    virtual auto Start() -> void = 0;
    virtual auto Stop() -> void = 0;
    [[nodiscard]] virtual auto IsRunning() const -> bool = 0;

    virtual auto SetOnCommand(CommandHandler handler) -> void = 0;

    // Invoked when a central (the app) first connects — detected as the first
    // command write of a session — so the session lifecycle can leave Idle.
    virtual auto SetOnConnect(ConnectionHandler handler) -> void = 0;

    // Invoked when the central disconnects, so the session can be finalized +
    // cleaned up (R15). Also fires on transport Stop().
    virtual auto SetOnDisconnect(ConnectionHandler handler) -> void = 0;
};

}  // namespace sst::control
