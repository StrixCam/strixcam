#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adapters/control/ble/bluez/chunk-assembler.hpp"
#include "adapters/control/ble/bluez/connection-supervisor.hpp"
#include "adapters/control/ble/bluez/gatt-application.hpp"
#include "app/control/ports/ble-transport.hpp"

namespace sst::adapters::control {

// BlueZ-via-D-Bus BLE peripheral transport. On Start():
//   1. Connect to the system bus.
//   2. Build a GattApplication exposing one service + command/response chars.
//   3. Subscribe to Device1 PropertiesChanged (Connected=false = abrupt
//      central loss — a force-killed app sends no StopNotify).
//   4. Register an LEAdvertisement1 object so BlueZ broadcasts the device name.
//   5. Call RegisterAdvertisement and RegisterApplication on org.bluez/<adapter>.
//   6. Start the connection's async event loop on an internal thread.
//
// On Stop(): unregister advertisement + application, leave the event loop, and
// drop all D-Bus objects.
class BluezBleTransport final : public sst::control::IBleTransport {
   public:
    // `advertised_name` is the contract `sst-cam-NNNN` name (computed from the
    // device identity in config). It is used both as the LEAdvertisement
    // LocalName and as the adapter Alias.
    explicit BluezBleTransport(std::string advertised_name,
                               std::string adapter_path = "/org/bluez/hci0");
    ~BluezBleTransport() override;

    BluezBleTransport(const BluezBleTransport&) = delete;
    auto operator=(const BluezBleTransport&) -> BluezBleTransport& = delete;
    BluezBleTransport(BluezBleTransport&&) = delete;
    auto operator=(BluezBleTransport&&) -> BluezBleTransport& = delete;

    auto Start() -> void override;
    auto Stop() -> void override;
    [[nodiscard]] auto IsRunning() const -> bool override;

    auto SetOnCommand(CommandHandler handler) -> void override;
    auto SetOnConnect(ConnectionHandler handler) -> void override;
    auto SetOnDisconnect(ConnectionHandler handler) -> void override;

   private:
    auto BuildAdvertisement() -> void;
    // The central is gone — either it unsubscribed (StopNotify, the graceful
    // path) or its link dropped without one (Device1 Connected=false via
    // OnDeviceProperties; a force-killed app sends no StopNotify). Fire
    // on_disconnect_ (session cleanup) and resume advertising so the camera is
    // discoverable again. Both signals funnel here and are idempotent through
    // the supervisor's generation guard. Invoked on the D-Bus event-loop
    // thread, so it hands the (blocking) work to the owned disconnect worker —
    // running it inline would deadlock the event loop the re-advertise futures
    // depend on.
    auto HandleCentralGone() -> void;
    // Bus-wide PropertiesChanged handler (match rule arg0=org.bluez.Device1):
    // classifies Connected=false on a device node of our adapter as a central
    // link drop and feeds HandleCentralGone. Runs on the event-loop thread.
    auto OnDeviceProperties(sdbus::Message& msg) -> void;
    // Body of the owned disconnect worker thread: waits for tickets queued by
    // HandleCentralGone, delivers each (generation-validated) disconnect and
    // re-advertises. Doubles as the advertising watchdog: while no central is
    // present it re-asserts the advertisement every kAdvRefreshInterval,
    // because the controller can silently terminate the advertising set
    // (kernel: "Unexpected advertising set terminated event") while BlueZ
    // still reports it active — there is no D-Bus signal to react to, so the
    // advertisement is periodically re-registered rather than trusted.
    // Started by Start(), joined by Stop() BEFORE the event loop goes away
    // (its re-advertise futures are serviced by that loop).
    auto DisconnectWorkerLoop() -> void;
    // Joins the worker; returns a still-undelivered ticket (queued disconnect
    // the worker never got to) so Stop() can deliver it synchronously.
    auto StopDisconnectWorker() -> std::optional<std::uint64_t>;
    // Re-register the LE advertisement so BlueZ resumes advertising (it pauses
    // our connectable advert on connect and does not auto-resume on disconnect).
    // `from_watchdog` only tunes the success log level (periodic re-asserts log
    // at debug, event-driven re-advertises at info). MUST run off the
    // event-loop thread.
    auto ReAdvertise(bool from_watchdog) -> void;
    // Demux a Command-Write characteristic write: either an inbound command
    // ChunkedPayload (total_chunks >= 1) or a ChunkAck (total_chunks == 0,
    // wire-compatible on fields 1/2) acking an outbound response chunk.
    auto OnRawCommand(std::vector<std::uint8_t> bytes) -> void;
    // Serialize + chunk a CommandResponse out over the response characteristic,
    // gated by ChunkAck (R3). Sends chunk 0 immediately.
    auto SendResponse(const sst_cam::CommandResponse& response) -> void;
    // Emit a ChunkAck for one inbound (app->camera) command chunk so the app
    // can release its next chunk. The firmware is a GATT peripheral and cannot
    // write to the central, so the ack rides the response characteristic as a
    // notification; it is wire-compatible with ChunkedPayload on fields 1/2 and
    // carries no total_chunks (==0), which the app demuxes as an ack rather than
    // a response chunk (mirroring our own inbound-ack disambiguation).
    auto SendInboundAck(const std::string& correlation_id, std::uint32_t chunk_index) -> void;
    // Invoke a BlueZ manager method (RegisterAdvertisement / RegisterApplication)
    // ASYNCHRONOUSLY and wait on a std::future. These calls make BlueZ re-enter
    // our own D-Bus objects before they reply; a blocking call would monopolize
    // the connection and deadlock the event loop until the 25s D-Bus timeout.
    // See the comment in Start() for the full rationale.
    static auto CallManagerAsync(sdbus::IProxy& proxy, const char* iface, const char* method,
                                 const sdbus::ObjectPath& object_path) -> void;

    std::string advertised_name_;
    std::string adapter_path_;
    std::string app_root_path_;
    std::string adv_path_;

    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> adv_obj_;
    std::unique_ptr<GattApplication> gatt_app_;
    // Owned match-rule slot for the Device1 PropertiesChanged subscription
    // (abrupt central loss). Released in Stop() before the connection drops.
    sdbus::Slot device_match_slot_;

    mutable std::mutex mtx_;
    CommandHandler on_command_;
    ConnectionHandler on_connect_;
    ConnectionHandler on_disconnect_;
    // Connect/disconnect ordering: generation-validated so the disconnect
    // worker never delivers OnDisconnect after a newer OnConnect (fast
    // unsubscribe+resubscribe would otherwise re-arm the session's auto-stop
    // right after the reconnect cancelled it).
    ConnectionSupervisor supervisor_;
    ChunkAssembler assembler_;
    std::atomic<bool> running_{false};
    // Atomic: ReAdvertise() re-arms the advertisement flag from the disconnect
    // worker while Stop()'s early-exit check may read it from another thread.
    std::atomic<bool> advertisement_registered_{false};
    bool application_registered_{false};

    // Owned disconnect worker (replaces the old detached thread, so shutdown
    // joins every thread). HandleCentralGone (event-loop thread) queues the
    // latest ticket; the worker delivers it off-loop. Guarded by worker_mtx_.
    std::thread disconnect_worker_;
    std::mutex worker_mtx_;
    std::condition_variable worker_cv_;
    bool worker_stop_{false};
    std::optional<std::uint64_t> pending_ticket_;
    // A central-gone signal arrived that needs a re-advertise even when it
    // carries no ticket (e.g. the link dropped before the central ever wrote —
    // the connectable advertisement was still consumed by the connection).
    bool readvertise_requested_{false};
};

}  // namespace sst::adapters::control
