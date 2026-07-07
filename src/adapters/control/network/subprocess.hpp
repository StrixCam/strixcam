#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace sst::adapters::control {

// Shared bounded-subprocess helpers for the nmcli/ip network adapters. Every
// exec is run on the single BLE dispatcher thread, so an unbounded blocking
// wait (a hung nmcli mid-NM-restart, a saturated D-Bus) would stall ALL BLE
// commands. These wrap fork/exec with a wall-clock deadline: past it the child
// is SIGKILLed and reaped, and the call returns failure instead of hanging.

// Generous deadline for state-changing commands (`nmcli con up` waits on NM to
// bring the iface online + DHCP, which can legitimately take ~30s).
constexpr std::chrono::seconds kApplyTimeout{45};
// Short deadline for read-only discovery queries (device status, addresses).
constexpr std::chrono::seconds kQueryTimeout{5};

// Result of a bounded capture: the child's stdout (possibly partial on timeout)
// plus whether it exited cleanly within the deadline.
struct CaptureResult {
    std::string output;
    bool ok{false};  // exited 0 within the deadline (false on exec/timeout/non-zero)
};

// Run `argv` (argv[0] resolved via PATH) to completion, bounded by `timeout`.
// Returns true iff the child exits 0 within the deadline. On timeout the child
// is SIGKILLed and reaped, and false is returned. argv must be non-empty.
auto RunBounded(const std::vector<std::string>& argv, std::chrono::seconds timeout) -> bool;

// Run `argv` capturing its stdout, bounded by `timeout`. The child's output is
// read through a pipe with poll()-driven deadline enforcement; on timeout the
// child is SIGKILLed, reaped, and CaptureResult.ok is false.
auto CaptureBounded(const std::vector<std::string>& argv,
                    std::chrono::seconds timeout) -> CaptureResult;

}  // namespace sst::adapters::control
