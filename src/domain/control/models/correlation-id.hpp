#pragma once

#include <string>

namespace sst::control {

// Wraps the per-request correlation id that keys an outbound BLE transfer. The
// outbound API also takes a `data` payload string; both being bare std::strings
// made them trivially transposable at the call site (high-impact: the wrong
// value silently binds). Wrapping the id in its own type makes a swap a compile
// error while `data` stays a plain string.
struct CorrelationId {
    std::string value;
};

}  // namespace sst::control
