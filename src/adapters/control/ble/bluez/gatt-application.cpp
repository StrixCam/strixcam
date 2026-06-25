#include "adapters/control/ble/bluez/gatt-application.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace sst::adapters::control {

namespace {

constexpr const char* kIfaceProperties = "org.freedesktop.DBus.Properties";
constexpr const char* kIfaceGattService = "org.bluez.GattService1";
constexpr const char* kIfaceGattChar = "org.bluez.GattCharacteristic1";

}  // namespace

GattApplication::GattApplication(sdbus::IConnection& connection, std::string root_path,
                                 std::string service_uuid, std::string command_char_uuid,
                                 std::string response_char_uuid, OnCommandBytes on_command)
    : connection_(connection),
      root_path_(std::move(root_path)),
      service_path_(root_path_ + "/service0"),
      command_char_path_(service_path_ + "/char0"),
      response_char_path_(service_path_ + "/char1"),
      service_uuid_(std::move(service_uuid)),
      command_char_uuid_(std::move(command_char_uuid)),
      response_char_uuid_(std::move(response_char_uuid)),
      on_command_(std::move(on_command)) {
    BuildService();
    BuildCommandChar();
    BuildResponseChar();
    BuildRoot();
}

GattApplication::~GattApplication() = default;

auto GattApplication::BuildRoot() -> void {
    root_obj_ = sdbus::createObject(connection_, root_path_);

    // BlueZ's GATT manager calls org.freedesktop.DBus.ObjectManager.GetManagedObjects
    // on the application root to discover the service + characteristics. That
    // interface is RESERVED by sd-bus: sd_bus_add_object_vtable() rejects the four
    // standard org.freedesktop.DBus.* interfaces (Properties, Introspectable, Peer,
    // ObjectManager) with -EINVAL, which surfaces as
    // "Failed to register object vtable (Invalid argument)" and aborts BLE start.
    // Use sdbus-c++'s built-in ObjectManager instead: it auto-answers
    // GetManagedObjects by enumerating the child service/characteristic objects
    // under this path and the properties each already registers (UUID, Primary,
    // Service, Flags, …), and emits InterfacesAdded/Removed. No hand-rolled vtable.
    root_obj_->addObjectManager();

    root_obj_->finishRegistration();
}

auto GattApplication::BuildService() -> void {
    service_obj_ = sdbus::createObject(connection_, service_path_);

    service_obj_->registerProperty("UUID").onInterface(kIfaceGattService).withGetter([this] {
        return service_uuid_;
    });
    service_obj_->registerProperty("Primary").onInterface(kIfaceGattService).withGetter([] {
        return true;
    });

    service_obj_->finishRegistration();
}

auto GattApplication::BuildCommandChar() -> void {
    command_obj_ = sdbus::createObject(connection_, command_char_path_);

    command_obj_->registerProperty("UUID").onInterface(kIfaceGattChar).withGetter([this] {
        return command_char_uuid_;
    });
    command_obj_->registerProperty("Service").onInterface(kIfaceGattChar).withGetter([this] {
        return sdbus::ObjectPath{service_path_};
    });
    command_obj_->registerProperty("Flags").onInterface(kIfaceGattChar).withGetter([] {
        return std::vector<std::string>{"write-without-response"};
    });

    // BlueZ calls WriteValue(ay value, a{sv} options) on the GATT characteristic.
    command_obj_->registerMethod("WriteValue")
        .onInterface(kIfaceGattChar)
        .implementedAs([this](std::vector<std::uint8_t> value,
                              const std::map<std::string, sdbus::Variant>& /*options*/) {
            if (on_command_) {
                on_command_(std::move(value));
            }
        });

    // ReadValue is required by BlueZ to exist on a characteristic — for a pure
    // write characteristic it can return empty.
    command_obj_->registerMethod("ReadValue")
        .onInterface(kIfaceGattChar)
        .implementedAs([](const std::map<std::string, sdbus::Variant>& /*options*/) {
            return std::vector<std::uint8_t>{};
        });

    command_obj_->finishRegistration();
}

auto GattApplication::BuildResponseChar() -> void {
    response_obj_ = sdbus::createObject(connection_, response_char_path_);

    response_obj_->registerProperty("UUID").onInterface(kIfaceGattChar).withGetter([this] {
        return response_char_uuid_;
    });
    response_obj_->registerProperty("Service").onInterface(kIfaceGattChar).withGetter([this] {
        return sdbus::ObjectPath{service_path_};
    });
    response_obj_->registerProperty("Flags").onInterface(kIfaceGattChar).withGetter([] {
        return std::vector<std::string>{"notify"};
    });
    response_obj_->registerProperty("Value").onInterface(kIfaceGattChar).withGetter([this] {
        std::lock_guard lock(mtx_);
        return response_value_;
    });
    response_obj_->registerProperty("Notifying").onInterface(kIfaceGattChar).withGetter([this] {
        std::lock_guard lock(mtx_);
        return notifying_;
    });

    response_obj_->registerMethod("ReadValue")
        .onInterface(kIfaceGattChar)
        .implementedAs([this](const std::map<std::string, sdbus::Variant>& /*options*/) {
            std::lock_guard lock(mtx_);
            return response_value_;
        });

    response_obj_->registerMethod("StartNotify").onInterface(kIfaceGattChar).implementedAs([this] {
        std::lock_guard lock(mtx_);
        notifying_ = true;
        spdlog::info("GattApplication: StartNotify on response char");
    });

    response_obj_->registerMethod("StopNotify").onInterface(kIfaceGattChar).implementedAs([this] {
        std::lock_guard lock(mtx_);
        notifying_ = false;
        spdlog::info("GattApplication: StopNotify on response char");
    });

    response_obj_->finishRegistration();
}

auto GattApplication::SendNotification(const std::vector<std::uint8_t>& bytes) -> void {
    std::map<std::string, sdbus::Variant> changed;
    bool notifying_now = false;
    {
        std::lock_guard lock(mtx_);
        response_value_ = bytes;
        notifying_now = notifying_;
        changed.emplace("Value", sdbus::Variant{response_value_});
    }

    response_obj_->emitSignal("PropertiesChanged")
        .onInterface(kIfaceProperties)
        .withArguments(std::string{kIfaceGattChar}, changed, std::vector<std::string>{});

    if (!notifying_now) {
        spdlog::debug(
            "GattApplication: SendNotification while no central is subscribed; "
            "value cached, will be served on next ReadValue");
    }
}

}  // namespace sst::adapters::control
