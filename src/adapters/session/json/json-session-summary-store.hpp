#pragma once

#include <optional>
#include <string>
#include <utility>

#include "app/session/ports/session-summary-store.hpp"
#include "domain/session/models/session-summary.hpp"

namespace sst::adapters::session {

// JSON-file implementation of the ISessionSummaryStore port: one small file in
// the config dir (last-session.json) holding the most recent session's summary.
// Written on every session end AND write-ahead on recording start, so a crash
// mid-recording still leaves a reboot/file-invalid record for the next boot.
// Lives in the adapter layer so the session manager does no filesystem IO.
class JsonSessionSummaryStore final : public sst::session::ISessionSummaryStore {
   public:
    explicit JsonSessionSummaryStore(std::string path) : path_(std::move(path)) {}

    auto Persist(const sst::session::LastSessionSummary& summary) -> bool override;
    [[nodiscard]] auto Load() -> std::optional<sst::session::LastSessionSummary> override;

   private:
    std::string path_;
};

}  // namespace sst::adapters::session
