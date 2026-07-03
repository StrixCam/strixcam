#include "app/session/services/session_cleanup/session-cleanup.hpp"

namespace sst::session {

SessionCleanup::SessionCleanup(sst::storage::IRecordingService& recording,
                               sst::streaming::IStreamingService& streaming,
                               sst::raw_capture::IRawCaptureSink& proxy,
                               sst::control::IWifiManager& wifi, sst::control::IDhcpServer& dhcp)
    : recording_(recording), streaming_(streaming), proxy_(proxy), wifi_(wifi), dhcp_(dhcp) {}

auto SessionCleanup::FinalizeRecording() -> void {
    // Idempotent: a no-op when idle, EOSes the MP4 + writes the thumbnail when
    // a recording is active.
    recording_.Stop();
    // Force-stop the coupled training proxy on disconnect (U5) so a mid-match BLE
    // drop can't leave the per-camera proxy pipelines running / files open. A
    // no-op if the proxy was never started.
    proxy_.Stop();
}

auto SessionCleanup::StopStreaming() -> void {
    streaming_.StopAppStream();
    for (const auto& active : streaming_.ListActivePlatformStreams()) {
        streaming_.StopPlatformStream(active.stream_id);
    }
}

auto SessionCleanup::TeardownWifiDirect() -> void {
    dhcp_.Stop();
    wifi_.Stop();
}

}  // namespace sst::session
