#pragma once

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace sst::common::utils {

// RFC 4122 v4 (random) UUID. Used for match / recording / event-clip ids that
// double as filesystem path components — keep the canonical 8-4-4-4-12 hex form
// so directory listings remain human-scannable.
inline auto MakeUuidV4() -> std::string {
    constexpr std::size_t kByteCount = 16;  // 128-bit UUID
    constexpr std::size_t kHalfByteCount = kByteCount / 2;
    constexpr unsigned kBitsPerByte = 8;
    constexpr std::size_t kVersionByte = 6;      // RFC 4122 §4.1.3: version nibble lives here
    constexpr std::size_t kVariantByte = 8;      // RFC 4122 §4.1.1: variant bits live here
    constexpr std::uint8_t kVersionMask = 0x0F;  // clear the high nibble…
    constexpr std::uint8_t kVersionV4 = 0x40;    // …then stamp version 4
    constexpr std::uint8_t kVariantMask = 0x3F;  // clear the top two bits…
    constexpr std::uint8_t kVariantRfc = 0x80;   // …then stamp the RFC 4122 variant

    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::array<std::uint8_t, kByteCount> bytes{};
    const std::uint64_t high = rng();
    const std::uint64_t low = rng();
    for (std::size_t i = 0; i < kHalfByteCount; ++i) {
        bytes[i] = static_cast<std::uint8_t>(high >> (i * kBitsPerByte));
        bytes[i + kHalfByteCount] = static_cast<std::uint8_t>(low >> (i * kBitsPerByte));
    }
    bytes[kVersionByte] =
        static_cast<std::uint8_t>((bytes[kVersionByte] & kVersionMask) | kVersionV4);
    bytes[kVariantByte] =
        static_cast<std::uint8_t>((bytes[kVariantByte] & kVariantMask) | kVariantRfc);

    // Canonical 8-4-4-4-12 hex form. The byte indices below are a self-evident
    // 0..15 sequence into `bytes` — named constants would obscure, not clarify.
    // NOLINTBEGIN(readability-magic-numbers)
    return fmt::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
        bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    // NOLINTEND(readability-magic-numbers)
}

// Cryptographically-strong 128-bit random token (32 hex chars). Unlike
// MakeUuidV4 — which seeds an mt19937 PRNG once and is fine for human-scannable
// ids — every word here is pulled directly from std::random_device (/dev/urandom
// on Linux), so download bearer tokens are not predictable from observed ids.
inline auto MakeSecureToken() -> std::string {
    std::random_device rng_device;
    const std::array<std::uint32_t, 4> words{rng_device(), rng_device(), rng_device(),
                                             rng_device()};
    return fmt::format("{:08x}{:08x}{:08x}{:08x}", words[0], words[1], words[2], words[3]);
}

}  // namespace sst::common::utils
