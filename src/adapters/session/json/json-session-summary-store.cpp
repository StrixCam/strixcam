#include "adapters/session/json/json-session-summary-store.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace sst::adapters::session {

namespace {

using sst::session::LastSessionSummary;
using sst::session::SessionEndReason;

// Reasons are persisted as stable strings (not enum ints) so a firmware update
// that reorders the enum can still read an older file.
auto ReasonToString(SessionEndReason reason) -> std::string {
    switch (reason) {
        case SessionEndReason::kAppStop:
            return "app-stop";
        case SessionEndReason::kAutoStop:
            return "auto-stop";
        case SessionEndReason::kCameraFailure:
            return "camera-failure";
        case SessionEndReason::kReboot:
            return "reboot";
    }
    return "app-stop";
}

auto ReasonFromString(const std::string& text) -> std::optional<SessionEndReason> {
    if (text == "app-stop") {
        return SessionEndReason::kAppStop;
    }
    if (text == "auto-stop") {
        return SessionEndReason::kAutoStop;
    }
    if (text == "camera-failure") {
        return SessionEndReason::kCameraFailure;
    }
    if (text == "reboot") {
        return SessionEndReason::kReboot;
    }
    return std::nullopt;
}

}  // namespace

auto JsonSessionSummaryStore::Persist(const LastSessionSummary& summary) -> bool {
    const nlohmann::json doc = {{"match_uuid", summary.match_uuid},
                                {"end_reason", ReasonToString(summary.end_reason)},
                                {"end_clock_seconds", summary.end_clock_seconds},
                                {"file_valid", summary.file_valid}};
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("JsonSessionSummaryStore: could not open {} for write", path_);
        return false;
    }
    out << doc.dump(2) << '\n';
    if (!out) {
        spdlog::error("JsonSessionSummaryStore: write to {} failed", path_);
        return false;
    }
    return true;
}

auto JsonSessionSummaryStore::Load() -> std::optional<LastSessionSummary> {
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return std::nullopt;  // first boot / never persisted — not an error
    }
    try {
        const auto doc = nlohmann::json::parse(input);
        LastSessionSummary summary;
        summary.match_uuid = doc.at("match_uuid").get<std::string>();
        const auto reason = ReasonFromString(doc.at("end_reason").get<std::string>());
        if (!reason) {
            spdlog::warn("JsonSessionSummaryStore: unknown end_reason in {} — ignoring file",
                         path_);
            return std::nullopt;
        }
        summary.end_reason = *reason;
        summary.end_clock_seconds = doc.at("end_clock_seconds").get<std::uint32_t>();
        summary.file_valid = doc.at("file_valid").get<bool>();
        return summary;
    } catch (const std::exception& e) {
        spdlog::warn("JsonSessionSummaryStore: could not parse {}: {} — ignoring file", path_,
                     e.what());
        return std::nullopt;
    }
}

}  // namespace sst::adapters::session
