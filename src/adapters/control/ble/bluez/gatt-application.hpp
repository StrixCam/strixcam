#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sst::adapters::control {

// BLE GATT application registered with org.bluez. Exposes one primary service
// with two characteristics:
//   - command  (write):       phone → firmware (ChunkedPayload bytes; carries
//                             both inbound Command chunks and ChunkAck writes)
//   - response (read+notify): firmware → phone (ChunkedPayload bytes wrapping a
//                             CommandResponse)
//
// Each write/notification value is a serialized sst_cam::ChunkedPayload. The
// BluezBleTransport owns reassembly and ChunkAck-gated outbound chunking; this
// class only moves raw bytes over D-Bus.
//
// All D-Bus objects live under <root_path> and are returned via the
// ObjectManager interface so RegisterApplication() in BlueZ can introspect
// them. SendNotification updates the response value and emits
// PropertiesChanged on the response characteristic so connected centrals
// receive the new value.
class GattApplication {
   public:
    using OnCommandBytes = std::function<void(std::vector<std::uint8_t>)>;

    GattApplication(sdbus::IConnection& connection, std::string root_path, std::string service_uuid,
                    std::string command_char_uuid, std::string response_char_uuid,
                    OnCommandBytes on_command);
    ~GattApplication();

    GattApplication(const GattApplication&) = delete;
    auto operator=(const GattApplication&) -> GattApplication& = delete;
    GattApplication(GattApplication&&) = delete;
    auto operator=(GattApplication&&) -> GattApplication& = delete;

    [[nodiscard]] auto RootPath() const -> const std::string& { return root_path_; }

    // Update the response characteristic value and notify any subscribed
    // central via PropertiesChanged. Safe to call from any thread.
    auto SendNotification(const std::vector<std::uint8_t>& bytes) -> void;

    // Invoked when the central unsubscribes from the response characteristic
    // (StopNotify) — BlueZ fires this when a connected central disconnects, so
    // the transport treats it as a disconnect (resume advertising + session
    // cleanup). The callback runs ON the D-Bus event-loop thread; it must not
    // block (the transport hands off to a worker).
    auto SetOnUnsubscribe(std::function<void()> handler) -> void;

   private:
    auto BuildRoot() -> void;
    auto BuildService() -> void;
    auto BuildCommandChar() -> void;
    auto BuildResponseChar() -> void;

    sdbus::IConnection& connection_;
    std::string root_path_;
    std::string service_path_;
    std::string command_char_path_;
    std::string response_char_path_;
    std::string service_uuid_;
    std::string command_char_uuid_;
    std::string response_char_uuid_;

    OnCommandBytes on_command_;
    std::function<void()> on_unsubscribe_;

    std::unique_ptr<sdbus::IObject> root_obj_;
    std::unique_ptr<sdbus::IObject> service_obj_;
    std::unique_ptr<sdbus::IObject> command_obj_;
    std::unique_ptr<sdbus::IObject> response_obj_;

    mutable std::mutex mtx_;
    std::vector<std::uint8_t> response_value_;
    bool notifying_{false};
};

}  // namespace sst::adapters::control
