#include "adapters/control/ble/bluez/bluez-ble-transport.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/control/services/chunk-ack/chunk-ack.hpp"
#include "bluetooth.pb.h"
#include "domain/control/models/bootstrap-config.hpp"

namespace sst::adapters::control {

namespace {

constexpr auto kCallTimeout = std::chrono::seconds{20};

constexpr const char* kBluezBus = "org.bluez";
constexpr const char* kIfaceGattManager = "org.bluez.GattManager1";
constexpr const char* kIfaceLeAdvManager = "org.bluez.LEAdvertisingManager1";
constexpr const char* kIfaceLeAdv = "org.bluez.LEAdvertisement1";

auto ToBytes(const sst_cam::ChunkedPayload& chunk) -> std::vector<std::uint8_t> {
    // Serialize straight into the byte vector (no intermediate std::string copy).
    std::vector<std::uint8_t> wire(chunk.ByteSizeLong());
    chunk.SerializeToArray(wire.data(), static_cast<int>(wire.size()));
    return wire;
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
    // Any write means a central is connected — surface the connect once (per
    // central) through the supervisor, which orders it against the detached
    // disconnect worker so a stale disconnect can never land after it.
    ConnectionHandler connect_handler;
    {
        std::lock_guard lock(mtx_);
        connect_handler = on_connect_;
    }
    supervisor_.OnWrite(connect_handler);

    sst_cam::Command command;
    bool have_command = false;
    CommandHandler handler;
    {
        std::lock_guard lock(mtx_);

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

auto BluezBleTransport::CallManagerAsync(sdbus::IProxy& proxy, const char* iface,
                                         const char* method,
                                         const sdbus::ObjectPath& object_path) -> void {
    // callMethodAsync registers the call on the connection and returns
    // immediately; the reply is delivered by the event-loop thread, which is
    // also free to dispatch BlueZ's reentrant callbacks into our objects. This
    // thread only blocks on a std::future (a plain condition variable — it does
    // not touch the bus), so there is no two-thread contention on the connection.
    auto future = proxy.callMethodAsync(method)
                      .onInterface(iface)
                      .withArguments(object_path, std::map<std::string, sdbus::Variant>{})
                      .getResultAsFuture<>();

    // Bound the wait so a wedged daemon can't hang Start() forever. 20s sits
    // under BlueZ's own 25s D-Bus timeout while leaving ample headroom for GATT
    // enumeration, which now completes in milliseconds.
    if (future.wait_for(kCallTimeout) != std::future_status::ready) {
        throw sdbus::Error("org.sst.cam.BleTimeout",
                           std::string{method} + " did not complete within 20s");
    }
    future.get();  // rethrows the sdbus::Error on a D-Bus-level failure
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

        // A central unsubscribing (StopNotify) is BlueZ's disconnect signal —
        // resume advertising so the camera is findable again for the next session.
        gatt_app_->SetOnUnsubscribe([this] { HandleCentralGone(); });

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

        // Start dispatching the connection BEFORE the Register* calls so the
        // event-loop thread can service the callbacks BlueZ makes back into us.
        connection_->enterEventLoopAsync();
        running_ = true;

        // Owned worker for off-event-loop disconnect handling (see
        // HandleCentralGone). Joined by Stop() while the event loop is still up.
        disconnect_worker_ = std::thread([this] { DisconnectWorkerLoop(); });

        auto bluez = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);

        // RegisterAdvertisement and RegisterApplication MUST be asynchronous.
        // BlueZ services each by re-entering our own objects on the same bus
        // before it replies: Properties.GetAll on /com/sst/cam/adv for the
        // advertisement, and ObjectManager.GetManagedObjects (plus each
        // characteristic's property reads) on /com/sst/cam/gatt for the
        // application. An sd_bus connection cannot be driven by two threads at
        // once, so a *blocking* callMethod() on this thread monopolizes the
        // connection and locks out enterEventLoopAsync()'s thread — the very
        // thread that has to answer those reentrant callbacks. BlueZ then waits
        // for a reply we cannot produce until the blocking call gives up at the
        // 25s D-Bus timeout ("org.freedesktop.DBus.Error.Timeout: Connection
        // timed out"), which is exactly what aborted BLE on-device (the
        // bluetoothd -d log shows it reading our advertisement props only at the
        // 25s mark, right as the call times out). Going async hands the
        // connection to the event loop: it answers BlueZ while this thread merely
        // waits on a std::future. Starting the loop first is NOT enough on its
        // own — the blocking call still monopolizes the connection.
        CallManagerAsync(*bluez, kIfaceLeAdvManager, "RegisterAdvertisement",
                         sdbus::ObjectPath{adv_path_});
        advertisement_registered_ = true;

        CallManagerAsync(*bluez, kIfaceGattManager, "RegisterApplication",
                         sdbus::ObjectPath{app_root_path_});
        application_registered_ = true;

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

    // Join the disconnect worker FIRST, while the event loop is still running
    // (its re-advertise futures are serviced by that loop), and before the
    // unregister calls below (so a late ReAdvertise can't re-register the
    // advertisement we are about to drop). A ticket it never delivered is
    // handed back and delivered synchronously here.
    const auto undelivered_ticket = StopDisconnectWorker();

    // Notify the session layer of the connection loss (it arms the auto-stop
    // safety net; it no longer tears the session down). On-device, a
    // central-initiated disconnect (org.bluez Device1 Connected=false) should
    // also invoke this; that D-Bus subscription is verified on hardware.
    ConnectionHandler on_disconnect;
    {
        std::lock_guard lock(mtx_);
        on_disconnect = on_disconnect_;
    }
    if (undelivered_ticket) {
        supervisor_.DeliverDisconnect(*undelivered_ticket, on_disconnect);
    }
    if (const auto ticket = supervisor_.OnCentralGone()) {
        supervisor_.DeliverDisconnect(*ticket, on_disconnect);
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

auto BluezBleTransport::HandleCentralGone() -> void {
    const auto ticket = supervisor_.OnCentralGone();
    if (!ticket) {
        return;  // never connected this session, or already handled
    }
    // This runs on the D-Bus event-loop thread (a GATT StopNotify callback).
    // Both the session notification and the re-advertise wait on work the event
    // loop itself must service, so they cannot run inline — queue the ticket
    // for the owned worker. Overwriting a pending ticket is safe: a second
    // ticket can only exist after an intervening connect, which makes the older
    // one stale (DeliverDisconnect would drop it by generation anyway).
    {
        std::lock_guard lock(worker_mtx_);
        pending_ticket_ = ticket;
    }
    worker_cv_.notify_one();
}

auto BluezBleTransport::DisconnectWorkerLoop() -> void {
    for (;;) {
        std::optional<std::uint64_t> ticket;
        {
            std::unique_lock lock(worker_mtx_);
            worker_cv_.wait(lock, [this] { return worker_stop_ || pending_ticket_.has_value(); });
            if (worker_stop_) {
                return;  // Stop() takes over delivery of any still-pending ticket
            }
            std::swap(ticket, pending_ticket_);
        }
        if (!ticket) {
            continue;
        }
        ConnectionHandler on_disc;
        {
            std::lock_guard lock(mtx_);
            on_disc = on_disconnect_;
        }
        // Re-validate at delivery time: if the central resubscribed+wrote before
        // this ran, the disconnect is stale and is dropped (never delivered
        // after the newer OnConnect).
        if (!supervisor_.DeliverDisconnect(*ticket, on_disc)) {
            spdlog::info(
                "BluezBleTransport: stale disconnect superseded by a newer connect — dropped");
        }
        // Skip the re-advertise churn when a central is already back (BlueZ
        // pauses a connectable advert while connected anyway).
        if (!supervisor_.CentralPresent()) {
            ReAdvertise();
        }
    }
}

auto BluezBleTransport::StopDisconnectWorker() -> std::optional<std::uint64_t> {
    std::optional<std::uint64_t> undelivered;
    {
        std::lock_guard lock(worker_mtx_);
        worker_stop_ = true;
        undelivered = pending_ticket_;
        pending_ticket_.reset();
    }
    worker_cv_.notify_one();
    if (disconnect_worker_.joinable()) {
        disconnect_worker_.join();
    }
    // Re-arm for a future Start() (the transport is Start/Stop cyclable).
    std::lock_guard lock(worker_mtx_);
    worker_stop_ = false;
    return undelivered;
}

auto BluezBleTransport::ReAdvertise() -> void {
    if (!running_ || !connection_) {
        return;
    }
    try {
        auto bluez = sdbus::createProxy(*connection_, kBluezBus, adapter_path_);
        // BlueZ pauses a connectable advertisement while a central is connected
        // and does not resume it on disconnect; re-register to start advertising
        // again. Drop the existing registration first (RegisterAdvertisement
        // rejects a still-registered object path). Async, like Start(), so the
        // event loop can answer BlueZ's reentrant property reads (see
        // CallManagerAsync) instead of deadlocking.
        try {
            auto future = bluez->callMethodAsync("UnregisterAdvertisement")
                              .onInterface(kIfaceLeAdvManager)
                              .withArguments(sdbus::ObjectPath{adv_path_})
                              .getResultAsFuture<>();
            if (future.wait_for(kCallTimeout) == std::future_status::ready) {
                future.get();
            }
        } catch (const sdbus::Error& e) {
            // Not currently registered (teardown race) — fine, just register.
            spdlog::debug("BluezBleTransport: UnregisterAdvertisement before re-advertise: {}",
                          e.what());
        }
        CallManagerAsync(*bluez, kIfaceLeAdvManager, "RegisterAdvertisement",
                         sdbus::ObjectPath{adv_path_});
        advertisement_registered_ = true;
        spdlog::info("BluezBleTransport: re-advertising as \"{}\" after central disconnect",
                     advertised_name_);
    } catch (const sdbus::Error& e) {
        spdlog::warn("BluezBleTransport: re-advertise failed: {}", e.what());
    }
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
