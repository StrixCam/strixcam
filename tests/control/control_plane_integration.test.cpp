// Assembled control-plane smoke test (U14): commands routed through the real
// CommandDispatcher + SessionManager + per-concern handlers advance the F1
// lifecycle and produce the right responses. Pure — hardware bits are faked.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/control/ports/dhcp-server.hpp"
#include "app/control/ports/wifi-manager.hpp"
#include "app/control/services/dispatcher/command-dispatcher.hpp"
#include "app/control/services/handlers/overlay.handler.hpp"
#include "app/control/services/handlers/session.handler.hpp"
#include "app/control/services/handlers/wifi-direct.handler.hpp"
#include "app/overlay/ports/overlay-renderer.hpp"
#include "app/overlay/ports/overlay-sink.hpp"
#include "app/overlay/services/overlay_controller/overlay-controller.hpp"
#include "app/session/ports/session-cleanup.hpp"
#include "app/session/services/session_manager/session-manager.hpp"
#include "app/streaming/ports/streaming-service.hpp"
#include "bluetooth.pb.h"

namespace {

using namespace sst;

class FakeCleanup final : public session::ISessionCleanup {
   public:
    auto FinalizeRecording() -> void override {}
    auto StopStreaming() -> void override {}
    auto TeardownWifiDirect() -> void override {}
};
class FakeWifi final : public control::IWifiManager {
   public:
    auto StartP2pGroupOwner() -> std::optional<network::WifiDirectGroup> override {
        network::WifiDirectGroup g;
        g.ssid = "DIRECT-z";
        g.psk = "pw";
        g.group_interface = "p2p-wlan0-0";
        g.group_owner_ip = "192.168.49.1";
        g.role = "GO";
        return g;
    }
    auto Stop() -> void override {}
    [[nodiscard]] auto State() const -> control::WifiState override { return {}; }
};
class FakeDhcp final : public control::IDhcpServer {
   public:
    auto Start(const std::string& /*group_interface*/,
               const std::string& /*go_ip*/) -> bool override {
      return true;
    }
    auto Stop() -> void override {}
};
class FakeRenderer final : public overlay::IOverlayRenderer {
   public:
    auto Render(const overlay::RenderScene& /*scene*/, std::uint32_t w,
                std::uint32_t h) -> overlay::RgbaImage override {
      overlay::RgbaImage img;
      img.width = w;
      img.height = h;
      img.stride = w * 4;
      img.pixels.assign(static_cast<std::size_t>(img.stride) * h, 0);
      return img;
    }
};
class FakeSink final : public overlay::IOverlaySink {
   public:
    auto PushFrame(const overlay::RgbaImage& /*frame*/) -> void override {}
};
class FakeStreaming final : public streaming::IStreamingService {
   public:
    auto StartAppStream(const streaming::AppStreamConfig& /*config*/)
        -> bool override {
      return true;
    }
    auto StopAppStream() -> bool override { return true; }
    [[nodiscard]] auto IsAppStreamRunning() const -> bool override { return false; }
    auto StartPlatformStream(const streaming::PlatformStreamConfig& /*config*/)
        -> bool override {
      return true;
    }
    auto StopPlatformStream(std::int64_t /*stream_id*/) -> bool override {
      return true;
    }
    [[nodiscard]] auto ListActivePlatformStreams() const
        -> std::vector<streaming::ActivePlatformStream> override {
        return {};
    }
};

auto Corr(const std::string& id) { return id; }

TEST(ControlPlaneIntegrationTest, RoutesFullLifecycleToReady) {
    FakeCleanup cleanup;
    session::SessionManager sm(cleanup);
    FakeWifi wifi;
    FakeDhcp dhcp;
    FakeRenderer renderer;
    FakeSink sink;
    FakeStreaming streaming;
    overlay::OverlayController controller(renderer, sink, 64, 64);

    control::CommandDispatcher dispatcher;
    dispatcher.Register(std::make_shared<control::SessionHandler>(sm));
    dispatcher.Register(
        std::make_shared<control::WifiDirectHandler>(sm, wifi, dhcp, streaming, 8554, 8080));
    dispatcher.Register(
        std::make_shared<control::OverlayHandler>(sm, controller, [] { return std::uint64_t{0}; }));

    // The transport signals connect on the first write; simulate it.
    sm.OnConnect();

    // StartWifiDirect -> WifiReady, with generated creds reported.
    {
        sst_cam::Command c;
        c.set_correlation_id(Corr("a"));
        c.mutable_start_wifi_direct();
        auto resp = dispatcher.Dispatch(c);
        EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
        EXPECT_EQ(resp.correlation_id(), "a");
        ASSERT_EQ(resp.payload_case(), sst_cam::CommandResponse::kWifiDirectGroup);
        EXPECT_EQ(resp.wifi_direct_group().ssid(), "DIRECT-z");
        EXPECT_EQ(sm.Phase(), session::SessionPhase::kWifiReady);
    }

    // PushSessionConfig -> Configured.
    {
        sst_cam::Command c;
        c.set_correlation_id(Corr("b"));
        auto* sc = c.mutable_push_session_config();
        sc->set_match_uuid("m");
        sc->set_user_uuid("u");
        // empty output paths -> no dir creation
        auto resp = dispatcher.Dispatch(c);
        EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
        EXPECT_EQ(sm.Phase(), session::SessionPhase::kConfigured);
    }

    // PushOverlayLayout -> Ready.
    {
        sst_cam::Command c;
        c.set_correlation_id(Corr("c"));
        c.mutable_push_overlay_layout()->mutable_layout()->set_canvas_width(1920);
        auto resp = dispatcher.Dispatch(c);
        EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::OK);
        EXPECT_EQ(sm.Phase(), session::SessionPhase::kReady);
    }

    // An unwired command still gets a defined UNSUPPORTED response.
    {
        sst_cam::Command c;
        c.set_correlation_id(Corr("d"));
        c.mutable_factory_reset();
        auto resp = dispatcher.Dispatch(c);
        EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::UNSUPPORTED);
        EXPECT_EQ(resp.correlation_id(), "d");
    }
}

// Out-of-order: PushSessionConfig before the WiFi group is up is rejected by the
// assembled plane (the SM gate surfaces as ERROR, never silence).
TEST(ControlPlaneIntegrationTest, OutOfOrderConfigRejected) {
    FakeCleanup cleanup;
    session::SessionManager sm(cleanup);
    control::CommandDispatcher dispatcher;
    dispatcher.Register(std::make_shared<control::SessionHandler>(sm));
    sm.OnConnect();

    sst_cam::Command c;
    c.set_correlation_id("x");
    c.mutable_push_session_config()->set_match_uuid("m");
    auto resp = dispatcher.Dispatch(c);
    EXPECT_EQ(resp.status(), sst_cam::ResponseStatus::ERROR);
    EXPECT_FALSE(resp.error_message().empty());
}

}  // namespace
