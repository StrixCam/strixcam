// Unit tests for the dependency-free mp4 moov/mvhd duration probe.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/network/services/download_server/mp4-duration.hpp"

namespace fs = std::filesystem;
using sst::network::ProbeMp4DurationSeconds;

namespace {

constexpr std::uint32_t kByteBits = 8;       // bits per byte
constexpr std::size_t kBoxHeaderLen = 8;     // 4-byte size + 4-byte type
constexpr std::size_t kMvhdBodyLen = 32;     // bytes the probe reads from an mvhd
constexpr std::size_t kMdatPayloadLen = 16;  // dummy mdat payload size
constexpr std::uint8_t kFillByte = 0xAB;     // arbitrary mdat fill

auto PutBe32(std::vector<std::uint8_t>& out, std::uint32_t value) -> void {
    out.push_back(static_cast<std::uint8_t>(value >> (kByteBits * 3)));
    out.push_back(static_cast<std::uint8_t>(value >> (kByteBits * 2)));
    out.push_back(static_cast<std::uint8_t>(value >> kByteBits));
    out.push_back(static_cast<std::uint8_t>(value));
}

auto PutType(std::vector<std::uint8_t>& out, const std::string& type) -> void {
    out.insert(out.end(), type.begin(), type.end());
}

// A version-0 mvhd box: header + version/flags + ctime + mtime + timescale +
// duration + padding (the probe reads the first kMvhdBodyLen body bytes).
auto MakeMvhd(std::uint32_t timescale, std::uint32_t duration) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> body;
    PutBe32(body, 0);              // version(0) + flags
    PutBe32(body, 0);              // creation time
    PutBe32(body, 0);              // modification time
    PutBe32(body, timescale);      // timescale
    PutBe32(body, duration);       // duration
    body.resize(kMvhdBodyLen, 0);  // pad to the bytes the probe reads
    std::vector<std::uint8_t> box;
    PutBe32(box, static_cast<std::uint32_t>(kBoxHeaderLen + body.size()));
    PutType(box, "mvhd");
    box.insert(box.end(), body.begin(), body.end());
    return box;
}

auto BoxAround(const std::string& type,
               const std::vector<std::uint8_t>& children) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> box;
    PutBe32(box, static_cast<std::uint32_t>(kBoxHeaderLen + children.size()));
    PutType(box, type);
    box.insert(box.end(), children.begin(), children.end());
    return box;
}

auto FtypBox() -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> body;
    PutType(body, "mp42");
    PutBe32(body, 0);
    return BoxAround("ftyp", body);
}

auto WriteTemp(const std::string& name, const std::vector<std::uint8_t>& bytes) -> fs::path {
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

}  // namespace

TEST(Mp4DurationTest, ParsesMvhdDuration) {
    // timescale 600, duration 2400 -> 4 seconds.
    std::vector<std::uint8_t> file = FtypBox();
    const auto moov = BoxAround("moov", MakeMvhd(/*timescale=*/600, /*duration=*/2400));
    file.insert(file.end(), moov.begin(), moov.end());
    const auto path = WriteTemp("sst-mvhd-front.mp4", file);
    EXPECT_EQ(ProbeMp4DurationSeconds(path), 4U);
    fs::remove(path);
}

TEST(Mp4DurationTest, FindsMoovAfterMdat) {
    // mp4mux (non-faststart) writes moov AFTER mdat — the probe must skip mdat.
    std::vector<std::uint8_t> file = FtypBox();
    std::vector<std::uint8_t> mdat;  // a dummy media-data box
    PutBe32(mdat, static_cast<std::uint32_t>(kBoxHeaderLen + kMdatPayloadLen));
    PutType(mdat, "mdat");
    mdat.resize(kBoxHeaderLen + kMdatPayloadLen, kFillByte);
    file.insert(file.end(), mdat.begin(), mdat.end());
    const auto moov = BoxAround("moov", MakeMvhd(/*timescale=*/1000, /*duration=*/9000));
    file.insert(file.end(), moov.begin(), moov.end());
    const auto path = WriteTemp("sst-mvhd-back.mp4", file);
    EXPECT_EQ(ProbeMp4DurationSeconds(path), 9U);
    fs::remove(path);
}

TEST(Mp4DurationTest, ReturnsZeroWhenNoMoov) {
    const auto path = WriteTemp("sst-no-moov.mp4", FtypBox());
    EXPECT_EQ(ProbeMp4DurationSeconds(path), 0U);
    fs::remove(path);
}

TEST(Mp4DurationTest, ReturnsZeroForMissingFile) {
    EXPECT_EQ(ProbeMp4DurationSeconds(fs::temp_directory_path() / "sst-does-not-exist.mp4"), 0U);
}
