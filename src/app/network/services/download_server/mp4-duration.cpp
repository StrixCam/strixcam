#include "app/network/services/download_server/mp4-duration.hpp"

#include <array>
#include <fstream>
#include <optional>
#include <string>

namespace sst::network {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBoxHeaderLen = 8;  // 4-byte size + 4-byte type
constexpr std::size_t kLargeSizeLen = 8;  // extra 8 bytes when size == 1
constexpr std::uint32_t kBitsPerByte = 8U;
constexpr std::size_t kWordLen = 4;  // a 32-bit big-endian word

// Bytes read from the start of an mvhd body — enough to cover the v1 prefix
// (the longest), so both v0 and v1 fields are in `buf`.
constexpr std::size_t kMvhdReadLen = 32;
// Field byte offsets within that buffer (see ParseMvhdDuration).
constexpr std::size_t kMvhdV1TimescaleOff = 20;
constexpr std::size_t kMvhdV1DurationOff = 24;
constexpr std::size_t kMvhdV0TimescaleOff = 12;
constexpr std::size_t kMvhdV0DurationOff = 16;

auto ReadBe32(const std::uint8_t* bytes) -> std::uint32_t {
    return (static_cast<std::uint32_t>(bytes[0]) << (kBitsPerByte * 3)) |
           (static_cast<std::uint32_t>(bytes[1]) << (kBitsPerByte * 2)) |
           (static_cast<std::uint32_t>(bytes[2]) << kBitsPerByte) |
           static_cast<std::uint32_t>(bytes[3]);
}

auto ReadBe64(const std::uint8_t* bytes) -> std::uint64_t {
    const std::uint64_t high = ReadBe32(bytes);
    const std::uint64_t low = ReadBe32(bytes + kWordLen);
    return (high << (kBitsPerByte * kWordLen)) | low;
}

// One parsed box header: full box size (largesize / to-container-end resolved),
// the header length actually consumed (8 or 16), and the 4-char type. Shared by
// the top-level walk and the moov-child walk (previously duplicated).
struct BoxHeader {
    std::uint64_t size{0};
    std::uint64_t header_len{0};
    std::string type;
};

// Read the box header at `box_start` (file already positioned there). A
// box_size of 0 means "extends to `container_end`". Returns nullopt on a short
// read or an inconsistent size.
auto ReadBoxHeader(std::ifstream& file, std::uint64_t box_start,
                   std::uint64_t container_end) -> std::optional<BoxHeader> {
    std::array<std::uint8_t, kBoxHeaderLen> hdr{};
    file.read(reinterpret_cast<char*>(hdr.data()), kBoxHeaderLen);
    if (!file) {
        return std::nullopt;
    }
    BoxHeader box;
    box.size = ReadBe32(hdr.data());
    box.header_len = kBoxHeaderLen;
    if (box.size == 1) {
        std::array<std::uint8_t, kLargeSizeLen> ext{};
        file.read(reinterpret_cast<char*>(ext.data()), kLargeSizeLen);
        if (!file) {
            return std::nullopt;
        }
        box.size = ReadBe64(ext.data());
        box.header_len += kLargeSizeLen;
    } else if (box.size == 0) {
        box.size = container_end - box_start;
    }
    if (box.size < box.header_len) {
        return std::nullopt;
    }
    box.type.assign(reinterpret_cast<const char*>(hdr.data()) + kWordLen, kWordLen);
    return box;
}

// Read the mvhd box body (already positioned past its header) and return the
// duration in whole seconds, or 0 if the timescale is zero / body too short.
//
// mvhd layout: version(1) flags(3), then version-dependent fields. For v1 the
// creation/modification times are 8 bytes each and duration is 8 bytes; for v0
// they are 4 bytes each and duration is 4 bytes. timescale is always 4 bytes and
// sits right after the two time fields.
auto ParseMvhdDuration(std::ifstream& file, std::uint64_t body_len) -> std::uint64_t {
    std::array<std::uint8_t, kMvhdReadLen> buf{};
    const std::size_t want = buf.size();
    if (body_len < want) {
        return 0;
    }
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
    if (!file) {
        return 0;
    }
    const std::uint8_t version = buf[0];
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;
    if (version == 1) {
        // [4 ver+flags][8 ctime][8 mtime][4 timescale][8 duration]
        timescale = ReadBe32(buf.data() + kMvhdV1TimescaleOff);
        duration = ReadBe64(buf.data() + kMvhdV1DurationOff);
    } else {
        // [4 ver+flags][4 ctime][4 mtime][4 timescale][4 duration]
        timescale = ReadBe32(buf.data() + kMvhdV0TimescaleOff);
        duration = ReadBe32(buf.data() + kMvhdV0DurationOff);
    }
    if (timescale == 0) {
        return 0;
    }
    return duration / timescale;
}

// Walk the child boxes inside `moov` (file positioned at its first child) to find
// `mvhd` and parse its duration.
auto FindMvhdInMoov(std::ifstream& file, std::uint64_t moov_body_end) -> std::uint64_t {
    while (static_cast<std::uint64_t>(file.tellg()) + kBoxHeaderLen <= moov_body_end) {
        const auto box_start = static_cast<std::uint64_t>(file.tellg());
        const auto box = ReadBoxHeader(file, box_start, moov_body_end);
        if (!box || box_start + box->size > moov_body_end) {
            return 0;
        }
        if (box->type == "mvhd") {
            return ParseMvhdDuration(file, box->size - box->header_len);
        }
        file.seekg(static_cast<std::streamoff>(box_start + box->size));
    }
    return 0;
}

}  // namespace

auto ProbeMp4DurationSeconds(const fs::path& path) -> std::uint64_t {
    std::error_code err;
    const auto file_size = fs::file_size(path, err);
    if (err || file_size < kBoxHeaderLen) {
        return 0;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return 0;
    }
    std::uint64_t pos = 0;
    while (pos + kBoxHeaderLen <= file_size) {
        file.seekg(static_cast<std::streamoff>(pos));
        const auto box = ReadBoxHeader(file, pos, file_size);
        if (!box) {
            return 0;
        }
        if (box->type == "moov") {
            // file is positioned just past this moov header -> at its first child.
            return FindMvhdInMoov(file, pos + box->size);
        }
        pos += box->size;
    }
    return 0;
}

}  // namespace sst::network
