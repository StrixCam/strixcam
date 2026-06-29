#include "adapters/control/network/subprocess.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using sst::adapters::control::CaptureBounded;
using sst::adapters::control::CaptureResult;
using sst::adapters::control::RunBounded;
using namespace std::chrono_literals;

// A command that finishes well within the deadline succeeds and returns its
// stdout. Drives the real fork/exec + poll-read path (no fixed sleeps in the
// test itself — `true`/`echo` exit immediately).
TEST(SubprocessTest, FastCommandSucceeds) {
    EXPECT_TRUE(RunBounded({"true"}, 5s));
    EXPECT_FALSE(RunBounded({"false"}, 5s));
}

TEST(SubprocessTest, CaptureReturnsStdout) {
    const CaptureResult result = CaptureBounded({"echo", "hello-uplink"}, 5s);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.output, "hello-uplink\n");
}

// A command that outlives the deadline is SIGKILLed and reported as failure,
// and the call returns within a bound close to the deadline (not the full
// sleep). `sleep 30` stands in for a hung nmcli/ip; the 1s deadline is the
// bound under test.
TEST(SubprocessTest, RunTimesOutAndReturnsFailureWithinBound) {
    const auto start = std::chrono::steady_clock::now();
    const bool succeeded = RunBounded({"sleep", "30"}, 1s);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(succeeded);
    // Returned on the deadline, not after the full 30s sleep (generous ceiling
    // to stay non-flaky on a loaded runner).
    EXPECT_LT(elapsed, 10s);
}

TEST(SubprocessTest, CaptureTimesOutAndReturnsFailureWithinBound) {
    const auto start = std::chrono::steady_clock::now();
    const CaptureResult result = CaptureBounded({"sleep", "30"}, 1s);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.ok);
    EXPECT_LT(elapsed, 10s);
}

TEST(SubprocessTest, EmptyArgvRejected) {
    EXPECT_FALSE(RunBounded({}, 5s));
    EXPECT_FALSE(CaptureBounded({}, 5s).ok);
}

}  // namespace
