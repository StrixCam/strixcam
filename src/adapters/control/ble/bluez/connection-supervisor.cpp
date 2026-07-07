#include "adapters/control/ble/bluez/connection-supervisor.hpp"

namespace sst::adapters::control {

auto ConnectionSupervisor::OnWrite(const Callback& on_connect) -> bool {
    const std::lock_guard lock(mtx_);
    if (central_present_) {
        return false;
    }
    central_present_ = true;
    ++generation_;
    // Delivered under the mutex: a concurrently-claimed disconnect can never
    // slot in between the state flip and this callback.
    if (on_connect) {
        on_connect();
    }
    return true;
}

auto ConnectionSupervisor::OnCentralGone() -> std::optional<std::uint64_t> {
    const std::lock_guard lock(mtx_);
    if (!central_present_) {
        return std::nullopt;  // never connected, or already handled
    }
    central_present_ = false;
    return generation_;
}

auto ConnectionSupervisor::DeliverDisconnect(std::uint64_t ticket,
                                             const Callback& on_disconnect) -> bool {
    const std::lock_guard lock(mtx_);
    if (generation_ != ticket || central_present_) {
        return false;  // a newer connect superseded this disconnect — stale
    }
    if (on_disconnect) {
        on_disconnect();
    }
    return true;
}

auto ConnectionSupervisor::CentralPresent() const -> bool {
    const std::lock_guard lock(mtx_);
    return central_present_;
}

}  // namespace sst::adapters::control
