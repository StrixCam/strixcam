#include "app/network/services/download_server/mp4-duration.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string>

namespace sst::network {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBoxHeaderLen = 8;  // 4-byte size + 4-byte type
constexpr std::size_t kLargeSizeLen = 8;  // extra 8 bytes when size == 1
constexpr std::uint32_t kBitsPerByte = 8U;

auto ReadBe32(const std::uint8_t* p) -> std::uint32_t {
    return (static_cast<std::uint32_t>(p[0]) << (kBitsPerByte * 3)) |
           (static_cast<std::uint32_t>(p[1]) << (kBitsPerByte * 2)) |
           (static_cast<std::uint32_t>(p[2]) << kBitsPerByte) | static_cast<std::uint32_t>(p[3]);
}

auto ReadBe64(const std::uint8_t* p) -> std::uint64_t {
    std::uint64_t hi = ReadBe32(p);
    std::uint64_t lo = ReadBe32(p + 4);
    return (hi << (kBitsPerByte * 4)) | lo;
}

// Read the mvhd box body (already positioned past its header) and return the
// duration in whole seconds, or 0 if the timescale is zero / body too short.
//
// mvhd layout: version(1) flags(3), then version-dependent fields. For v1 the
// creation/modification times are 8 bytes each and duration is 8 bytes; for v0
// they are 4 bytes each and duration is 4 bytes. timescale is always 4 bytes and
// sits right after the two time fields.
auto ParseMvhdDuration(std::ifstream& file, std::uint64_t body_len) -> std::uint64_t {
    std::array<std::uint8_t, 32> buf{};
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
        timescale = ReadBe32(buf.data() + 20);
        duration = ReadBe64(buf.data() + 24);
    } else {
        // [4 ver+flags][4 ctime][4 mtime][4 timescale][4 duration]
        timescale = ReadBe32(buf.data() + 12);
        duration = ReadBe32(buf.data() + 16);
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
        std::array<std::uint8_t, kBoxHeaderLen> hdr{};
        file.read(reinterpret_cast<char*>(hdr.data()), kBoxHeaderLen);
        if (!file) {
            return 0;
        }
        std::uint64_t box_size = ReadBe32(hdr.data());
        std::uint64_t header_len = kBoxHeaderLen;
        if (box_size == 1) {
            std::array<std::uint8_t, kLargeSizeLen> ext{};
            file.read(reinterpret_cast<char*>(ext.data()), kLargeSizeLen);
            if (!file) {
                return 0;
            }
            box_size = ReadBe64(ext.data());
            header_len += kLargeSizeLen;
        } else if (box_size == 0) {
            box_size = moov_body_end - box_start;
        }
        if (box_size < header_len || box_start + box_size > moov_body_end) {
            return 0;
        }
        const std::string type(reinterpret_cast<const char*>(hdr.data()) + 4, 4);
        if (type == "mvhd") {
            return ParseMvhdDuration(file, box_size - header_len);
        }
        file.seekg(static_cast<std::streamoff>(box_start + box_size));
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
        std::array<std::uint8_t, kBoxHeaderLen> hdr{};
        file.read(reinterpret_cast<char*>(hdr.data()), kBoxHeaderLen);
        if (!file) {
            return 0;
        }
        std::uint64_t box_size = ReadBe32(hdr.data());
        std::uint64_t header_len = kBoxHeaderLen;
        if (box_size == 1) {
            std::array<std::uint8_t, kLargeSizeLen> ext{};
            file.read(reinterpret_cast<char*>(ext.data()), kLargeSizeLen);
            if (!file) {
                return 0;
            }
            box_size = ReadBe64(ext.data());
            header_len += kLargeSizeLen;
        } else if (box_size == 0) {
            box_size = file_size - pos;
        }
        if (box_size < header_len) {
            return 0;
        }
        const std::string type(reinterpret_cast<const char*>(hdr.data()) + 4, 4);
        if (type == "moov") {
            // file is positioned just past this moov header → at its first child.
            return FindMvhdInMoov(file, pos + box_size);
        }
        pos += box_size;
    }
    return 0;
}

}  // namespace sst::network
