#pragma once

#include <optional>

#include "domain/session/models/session-summary.hpp"

namespace sst::session {

// Persistence for the last-session summary (small JSON in the config dir).
// The session manager WRITE-AHEADS an end-reason=Reboot / file-valid=false
// summary when a recording starts and overwrites it with the real summary on
// every clean session end — so after a crash mid-recording, the next boot's
// Load() already describes the orphaned session without scanning MP4s.
class ISessionSummaryStore {
   public:
    virtual ~ISessionSummaryStore() = default;

    // Overwrite the stored summary. Returns false on I/O failure (logged by the
    // implementation; the in-memory summary is unaffected).
    virtual auto Persist(const LastSessionSummary& summary) -> bool = 0;

    // The stored summary, or nullopt when none was ever written (first boot) or
    // the file is unreadable/corrupt.
    [[nodiscard]] virtual auto Load() -> std::optional<LastSessionSummary> = 0;
};

}  // namespace sst::session
