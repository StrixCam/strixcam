#include "app/session/services/session_cleanup/session-cleanup.hpp"

namespace sst::session {

SessionCleanup::SessionCleanup(sst::storage::IRecordingService& recording,
                               sst::streaming::IStreamingService& streaming,
                               sst::storage::ProxyLifecycle& proxy,
                               sst::overlay::IOverlayTimelineRecorder& timeline,
                               sst::control::IWifiManager& wifi, sst::control::IDhcpServer& dhcp,
                               sst::decision::ManualCameraState& camera_state,
                               sst::streaming::PreviewLayoutState& layout_state)
    : recording_(recording),
      streaming_(streaming),
      proxy_(proxy),
      timeline_(timeline),
      wifi_(wifi),
      dhcp_(dhcp),
      camera_state_(camera_state),
      layout_state_(layout_state) {}

auto SessionCleanup::FinalizeRecording() -> bool {
    // Idempotent: a no-op when idle, EOSes the MP4 + writes the thumbnail when
    // a recording is active. The result feeds the summary's file-valid bit for
    // a session ending mid-recording.
    const auto result = recording_.Stop();
    // Flush the overlay timeline beside the L1 (no-op when never started) so an
    // auto-stop/failure finalize still leaves a burnable timeline.
    timeline_.Stop();
    // Force-stop the training proxy AND reset both ref-count holds atomically
    // (U5): a session ending without commanded stops must not leave a stale
    // record/stream hold, or the next session's first start would miss the
    // 0->1 transition and the proxy would stay silently dead. A no-op when the
    // proxy was never started.
    proxy_.ForceStop();
    return result.success;
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

auto SessionCleanup::ResetSelections() -> void {
    // Session-scoped UI selections revert to their construction defaults at
    // SESSION END (not on disconnect — a rejoining app adopts the live values
    // from the snapshot), so the next session starts where the app's fresh UI
    // starts (Left / single view). Idempotent: a no-op when already at defaults.
    camera_state_.Set(0);
    layout_state_.Set(sst::streaming::PreviewLayout::kSingle);
}

}  // namespace sst::session
