#pragma once

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace sst::common::utils {

// Cryptographically-strong 128-bit random token (32 hex chars). Every word is
// pulled directly from std::random_device (/dev/urandom on Linux), so download
// bearer tokens are not predictable from observed ids. (The old MakeUuidV4
// PRNG helper was removed unused — this is the only id-minting utility left.)
inline auto MakeSecureToken() -> std::string {
    std::random_device rng_device;
    const std::array<std::uint32_t, 4> words{rng_device(), rng_device(), rng_device(),
                                             rng_device()};
    return fmt::format("{:08x}{:08x}{:08x}{:08x}", words[0], words[1], words[2], words[3]);
}

}  // namespace sst::common::utils
