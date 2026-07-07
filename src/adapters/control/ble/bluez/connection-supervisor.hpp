#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace sst::adapters::control {

// Orders BLE central connect/disconnect deliveries with a generation counter.
//
// The transport learns "connected" from the first GATT write and "disconnected"
// from StopNotify, whose (blocking) handling is bounced to a detached worker —
// so a fast unsubscribe+resubscribe can otherwise deliver a stale OnDisconnect
// AFTER the new OnConnect and re-arm the auto-stop the connect just cancelled.
// Every connect bumps the generation; a disconnect captured under the mutex is
// delivered ONLY while its generation is still current, and both callbacks run
// UNDER the same mutex so delivery order always matches event order.
class ConnectionSupervisor {
   public:
    using Callback = std::function<void()>;

    // A central wrote to the command characteristic. The FIRST write from a new
    // central bumps the generation and delivers `on_connect` under the mutex;
    // subsequent writes are no-ops. Returns whether a connect was delivered.
    auto OnWrite(const Callback& on_connect) -> bool;

    // The central went away (StopNotify or transport stop). Returns the ticket
    // to pass to DeliverDisconnect, or nullopt when no central was present
    // (never connected, or the disconnect was already claimed).
    auto OnCentralGone() -> std::optional<std::uint64_t>;

    // Deliver `on_disconnect` for `ticket` iff no newer connect superseded it,
    // under the mutex. Returns whether it was delivered.
    auto DeliverDisconnect(std::uint64_t ticket, const Callback& on_disconnect) -> bool;

    [[nodiscard]] auto CentralPresent() const -> bool;

   private:
    mutable std::mutex mtx_;
    bool central_present_{false};
    std::uint64_t generation_{0};
};

}  // namespace sst::adapters::control
