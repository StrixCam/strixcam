#include <gtest/gtest.h>

#include "domain/storage/models/proxy-identity.hpp"
#include "domain/storage/services/proxy-naming.hpp"

namespace {

using sst::storage::ProxyIdentity;
namespace naming = sst::storage::proxy_naming;

TEST(ProxyNamingTest, FileNameRoundTrips) {
    const ProxyIdentity identity{.match_uuid = "abc-123-def", .camera_index = 1};
    const auto name = naming::FileName(identity);
    EXPECT_EQ(name, "proxy__abc-123-def__cam1.mp4");

    const auto parsed = naming::ParseFileName(name);
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->match_uuid, "abc-123-def");
    EXPECT_EQ(parsed->camera_index, 1U);
}

TEST(ProxyNamingTest, ParsesMultiDigitCameraIndex) {
    const auto parsed = naming::ParseFileName("proxy__match__cam12.mp4");
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->match_uuid, "match");
    EXPECT_EQ(parsed->camera_index, 12U);
}

// ParseFileName doubles as the enumeration EXCLUSION discriminator: everything
// it rejects is app-visible (final recordings, overlay exports), everything it
// accepts is firmware-internal proxy footage.
TEST(ProxyNamingTest, RejectsNonProxyNames) {
    EXPECT_FALSE(
        naming::ParseFileName("recording-2026.mp4").has_value());  // final: no proxy__ prefix
    EXPECT_FALSE(naming::ParseFileName("match-1-overlay.mp4").has_value());  // overlay export (L2)
    EXPECT_FALSE(naming::ParseFileName("proxy__m__cam0.nv12").has_value());  // wrong ext (proxy is
                                                                             // .mp4)
    EXPECT_FALSE(naming::ParseFileName("m__cam0.mp4").has_value());          // no prefix
    EXPECT_FALSE(naming::ParseFileName("proxy__m.mp4").has_value());         // no cam marker
    EXPECT_FALSE(naming::ParseFileName("proxy__m__camX.mp4").has_value());   // non-numeric index
    EXPECT_FALSE(naming::ParseFileName("proxy____cam0.mp4").has_value());    // empty match id
}

TEST(ProxyNamingTest, MatchIdMayContainSingleUnderscores) {
    const ProxyIdentity identity{.match_uuid = "a_b_c", .camera_index = 0};
    const auto parsed = naming::ParseFileName(naming::FileName(identity));
    if (!parsed) {
        FAIL() << "ParseFileName returned nullopt";
        return;
    }
    EXPECT_EQ(parsed->match_uuid, "a_b_c");
    EXPECT_EQ(parsed->camera_index, 0U);
}

}  // namespace
