#include "adapters/control/ble/bluez/bluez-ble-transport.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app/control/services/chunk-ack/chunk-ack.hpp"
#include "bluetooth.pb.h"
#include "domain/control/models/bootstrap-config.hpp"

namespace sst::adapters::control {

namespace {

constexpr const char* kBluezBus = "org.bluez";
constexpr const char* kIfaceGattManager = "org.bluez.GattManager1";
constexpr const char* kIfaceLeAdvManager = "org.bluez.LEAdvertisingManager1";
constexpr const char* kIfaceLeAdv = "org.bluez.LEAdvertisement1";

auto ToBytes(const sst_cam::ChunkedPayload& chunk) -> std::vector<std::uint8_t> {
    const std::string wire = chunk.SerializeAsString();
    return {wire.begin(), wire.end()};
}

}  // namespace

BluezBleTransport::BluezBleTransport(std::string advertised_name, std::string adapter_path)
    : advertised_name_(std::move(advertised_name)),
      adapter_path_(std::move(adapter_path)),
      app_root_path_("/com/sst/cam/gatt"),
      adv_path_("/com/sst/cam/adv") {}

BluezBleTransport::~BluezBleTransport() {
    if (running_) {
        Stop();
    }
}

auto BluezBleTransport::IsRunning() const -> bool { return running_; }

auto BluezBleTransport::SetOnCommand(CommandHandler handler) -> void {
    std::lock_guard lock(mtx_);
    on_command_ = std::move(handler);
}

auto BluezBleTransport::SetOnConnect(ConnectionHandler handler) -> void {
    std::lock_guard lock(mtx_);
    on_connect_ = std::move(handler);
}

auto BluezBleTransport::SetOnDisconnect(ConnectionHandler handler) -> void {
    std::lock_guard lock(mtx_);
    on_disconnect_ = std::move(handler);
}

auto BluezBleTransport::OnRawCommand(std::vector<std::uint8_t> bytes) -> void {
    sst_cam::ChunkedPayload chunk;
    if (!chunk.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::warn("BluezBleTransport: undecodable {}-byte write on command char — dropping",
                     bytes.size());
        return;
    }

    // Reassemble + ack under the lock (the assembler and connection state it
    // guards are not thread-safe), but invoke the command handler OUTSIDE the
    // lock (#14): the thumbnail handler runs a full-res cv::imencode, and holding
    // mtx_ across it head-of-line-blocks the whole BLE control path, including
    // acks for the other direction. We extract everything the handler needs while
    // locked, drop the lock, then call it.
    sst_cam::Command command;
    bool have_command = false;
    CommandHandler handler;
    ConnectionHandler connect;
    {
        std::lock_guard lock(mtx_);

        // Any write means a central is connected — surface the connect once so
        // the session can leave Idle.
        if (!central_present_) {
            central_present_ = true;
            connect = on_connect_;  // invoked after the lock is released
        }

        // A ChunkAck is wire-compatible with ChunkedPayload on fields 1/2 but
        // carries no total_chunks (#3): total_chunks == kChunkAckTotalChunks
        // means this write acks one of OUR outbound response chunks rather than
        // delivering an inbound command chunk.
        if (chunk.total_chunks() == sst::control::kChunkAckTotalChunks) {
            assembler_.OnAck(chunk.correlation_id(), chunk.chunk_index());
        } else {
            // #8: validate BEFORE acking. Only ack a chunk the assembler actually
            // accepted (well-formed framing). A rejected chunk (index>=total,
            // over-cap, total==0) must NOT be acked. Duplicates are accepted
            // (the app may have retransmitted after a lost ack) and re-acked,
            // keeping the app's flow control unstalled.
            const ChunkAssembler::OfferResult result = assembler_.OfferInbound(chunk);
            if (result.accepted) {
                SendInboundAck(chunk.correlation_id(), chunk.chunk_index());
            }
            if (result.payload) {
                if (command.ParseFromString(*result.payload)) {
                    have_command = true;
                    handler = on_command_;
                } else {
                    spdlog::warn(
                        "BluezBleTransport: reassembled payload (corr={}) is not a valid Command",
                        chunk.correlation_id());
                }
            }
        }
    }

    if (connect) {
        connect();
    }

    if (!have_command) {
        return;  // ack write, more chunks pending, malformed/duplicate, or bad Command
    }
    if (!handler) {
        spdlog::warn("BluezBleTransport: no command handler set");
        return;
    }

    // Handler runs unlocked (#14) — may do heavy work (full-res imencode).
    sst_cam::CommandResponse response = handler(command);

    std::lock_guard lock(mtx_);
    SendResponse(response);
}

auto BluezBleTransport::BuildAdvertisement() -> void {
    adv_obj_ = sdbus::createObject(*connection_, adv_path_);

    adv_obj_->registerProperty("Type").onInterface(kIfaceLeAdv).withGetter([] {
        return std::string{"peripheral"};
    });
    adv_obj_->registerProperty("ServiceUUIDs").onInterface(kIfaceLeAdv).withGetter([] {
        return std::vector<std::string>{
            std::string{sst::control::BootstrapDefaults::kGattServiceUuid}};
    });
    adv_obj_->registerProperty("LocalName").onInterface(kIfaceLeAdv).withGetter([this] {
        return advertised_name_;
    });
    adv_obj_->registerProperty("Includes").onInterface(kIfaceLeAdv).withGetter([] {
        return std::vector<std::string>{"tx-power"};
    });

    adv_obj_->registerMethod("Release").onInterface(kIfaceLeAdv).implementedAs([] {
        spdlog::info("LEAdvertisement1.Release called");
    });

    adv_obj_->finishRegistration();
}

auto BluezBleTransport::Start() -> void {
    if (running_) {
        return;
    }

    try {
        connection_ = sdbus::createSystemBusConnection();
    } catch (const sdbus::Error& e) {
        spdlog::error("BluezBleTransport: createSystemBusConnection failed: {}", e.what());
        return;
    }

    try {
        gatt_app_ = std::make_unique<GattApplication>(
            *connection_, app_root_path_,
            std::string{sst::control::BootstrapDefaults::kGattServiceUuid},
            std::string{sst::control::BootstrapDefaults::kGattCommandCharUuid},
            std::string{sst::control::BootstrapDefaults::kGattResponseCharUuid},
            [this](std::vector<std::uint8_t> bytes) { OnRawCommand(std::move(bytes)); });

        BuildAdvertisement();

        // Set the adapter Alias so the classic device name also reads
        // sst-cam-NNNN (the app's secondary scan filter checks the name).
        try {
            auto adapter = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);
            adapter->setProperty("Alias")
                .onInterface("org.bluez.Adapter1")
                .toValue(advertised_name_);
        } catch (const sdbus::Error& e) {
            spdlog::warn("BluezBleTransport: could not set adapter Alias: {}", e.what());
        }

        auto adv_proxy = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);
        adv_proxy->callMethod("RegisterAdvertisement")
            .onInterface(kIfaceLeAdvManager)
            .withArguments(sdbus::ObjectPath{adv_path_}, std::map<std::string, sdbus::Variant>{});
        advertisement_registered_ = true;

        auto gatt_proxy = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);
        gatt_proxy->callMethod("RegisterApplication")
            .onInterface(kIfaceGattManager)
            .withArguments(sdbus::ObjectPath{app_root_path_},
                           std::map<std::string, sdbus::Variant>{});
        application_registered_ = true;

        connection_->enterEventLoopAsync();
        running_ = true;

        spdlog::info("BluezBleTransport: advertising as \"{}\" with service {} on adapter {}",
                     advertised_name_, sst::control::BootstrapDefaults::kGattServiceUuid,
                     adapter_path_);
    } catch (const sdbus::Error& e) {
        spdlog::error("BluezBleTransport::Start failed: {}: {}", e.getName(), e.getMessage());
        Stop();
    }
}

auto BluezBleTransport::Stop() -> void {
    if (!running_ && !advertisement_registered_ && !application_registered_ && !connection_) {
        return;
    }
    spdlog::info("BluezBleTransport::Stop");

    // Notify the session layer so it finalizes + cleans up (R15). On-device, a
    // central-initiated disconnect (org.bluez Device1 Connected=false) should
    // also invoke this; that D-Bus subscription is verified on hardware.
    ConnectionHandler on_disconnect;
    {
        std::lock_guard lock(mtx_);
        on_disconnect = central_present_ ? on_disconnect_ : nullptr;
        central_present_ = false;
    }
    if (on_disconnect) {
        on_disconnect();
    }

    if (connection_ && (advertisement_registered_ || application_registered_)) {
        try {
            auto proxy = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);
            if (advertisement_registered_) {
                proxy->callMethod("UnregisterAdvertisement")
                    .onInterface(kIfaceLeAdvManager)
                    .withArguments(sdbus::ObjectPath{adv_path_});
                advertisement_registered_ = false;
            }
            if (application_registered_) {
                proxy->callMethod("UnregisterApplication")
                    .onInterface(kIfaceGattManager)
                    .withArguments(sdbus::ObjectPath{app_root_path_});
                application_registered_ = false;
            }
        } catch (const sdbus::Error& e) {
            spdlog::warn("BluezBleTransport: unregister failed: {}", e.what());
        }
    }

    if (connection_) {
        try {
            connection_->leaveEventLoop();
        } catch (const sdbus::Error& e) {
            spdlog::warn("BluezBleTransport: leaveEventLoop failed: {}", e.what());
        }
    }

    {
        // Drop assembler state under the lock BEFORE clearing gatt_app_ so no
        // half-assembled inbound message or stale outbound flow-control survives
        // into a future session, and the retained outbound send closures (which
        // capture this transport + gatt_app_) are released (#9).
        std::lock_guard lock(mtx_);
        assembler_.Reset();
        gatt_app_.reset();
    }
    adv_obj_.reset();
    connection_.reset();
    running_ = false;
}

auto BluezBleTransport::SendResponse(const sst_cam::CommandResponse& response) -> void {
    if (!gatt_app_) {
        spdlog::warn("BluezBleTransport::SendResponse: GATT app not active");
        return;
    }
    const std::string wire = response.SerializeAsString();
    const std::uint32_t total =
        assembler_.BeginOutbound(sst::control::CorrelationId{response.correlation_id()}, wire,
                                 [this](const sst_cam::ChunkedPayload& chunk) {
                                     // This closure is retained inside the assembler and invoked
                                     // later by OnAck. By then Stop()/disconnect may have reset
                                     // gatt_app_; guard against the null deref (the disconnect path
                                     // also Reset()s the assembler to drop these closures, but a
                                     // race can still fire one).
                                     if (gatt_app_) {
                                         gatt_app_->SendNotification(ToBytes(chunk));
                                     }
                                 });
    spdlog::debug("BluezBleTransport: response corr={} status={} -> {} chunk(s)",
                  response.correlation_id(), static_cast<int>(response.status()), total);
}

auto BluezBleTransport::SendInboundAck(const std::string& correlation_id,
                                       std::uint32_t chunk_index) -> void {
    if (!gatt_app_) {
        spdlog::warn("BluezBleTransport::SendInboundAck: GATT app not active");
        return;
    }
    const std::string wire =
        sst::control::BuildInboundAck(correlation_id, chunk_index).SerializeAsString();
    gatt_app_->SendNotification({wire.begin(), wire.end()});
    spdlog::debug("BluezBleTransport: inbound ack corr={} index={}", correlation_id, chunk_index);
}

}  // namespace sst::adapters::control
