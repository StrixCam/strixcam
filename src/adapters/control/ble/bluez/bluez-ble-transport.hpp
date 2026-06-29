#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/control/ble/bluez/chunk-assembler.hpp"
#include "adapters/control/ble/bluez/gatt-application.hpp"
#include "app/control/ports/ble-transport.hpp"

namespace sst::adapters::control {

// BlueZ-via-D-Bus BLE peripheral transport. On Start():
//   1. Connect to the system bus.
//   2. Build a GattApplication exposing one service + command/response chars.
//   3. Register an LEAdvertisement1 object so BlueZ broadcasts the device name.
//   4. Call RegisterAdvertisement and RegisterApplication on org.bluez/<adapter>.
//   5. Start the connection's async event loop on an internal thread.
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
    // The central unsubscribed (StopNotify) — BlueZ fires this on disconnect.
    // Fire on_disconnect_ (session cleanup) and resume advertising so the camera
    // is discoverable again. Invoked on the D-Bus event-loop thread, so it hands
    // the (blocking) work to a detached worker — running it inline would deadlock
    // the event loop the re-advertise futures depend on.
    auto HandleCentralGone() -> void;
    // Re-register the LE advertisement so BlueZ resumes advertising (it pauses
    // our connectable advert on connect and does not auto-resume on disconnect).
    // MUST run off the event-loop thread.
    auto ReAdvertise() -> void;
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

    mutable std::mutex mtx_;
    CommandHandler on_command_;
    ConnectionHandler on_connect_;
    ConnectionHandler on_disconnect_;
    bool central_present_{false};
    ChunkAssembler assembler_;
    std::atomic<bool> running_{false};
    bool advertisement_registered_{false};
    bool application_registered_{false};
};

}  // namespace sst::adapters::control
