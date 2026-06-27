// Unit tests for the dependency-free mp4 moov/mvhd duration probe.
#include "app/network/services/download_server/mp4-duration.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using sst::network::ProbeMp4DurationSeconds;

namespace {

void PutBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void PutType(std::vector<std::uint8_t>& out, const std::string& t) {
    out.insert(out.end(), t.begin(), t.end());
}

// A version-0 mvhd box: header + version/flags + ctime + mtime + timescale +
// duration + 12 bytes padding (the probe reads the first 32 body bytes).
std::vector<std::uint8_t> MakeMvhd(std::uint32_t timescale, std::uint32_t duration) {
    std::vector<std::uint8_t> body;
    PutBe32(body, 0);          // version(0) + flags
    PutBe32(body, 0);          // creation time
    PutBe32(body, 0);          // modification time
    PutBe32(body, timescale);  // timescale
    PutBe32(body, duration);   // duration
    body.resize(32, 0);        // pad to the 32 bytes the probe reads
    std::vector<std::uint8_t> box;
    PutBe32(box, static_cast<std::uint32_t>(8 + body.size()));
    PutType(box, "mvhd");
    box.insert(box.end(), body.begin(), body.end());
    return box;
}

std::vector<std::uint8_t> BoxAround(const std::string& type,
                                    const std::vector<std::uint8_t>& children) {
    std::vector<std::uint8_t> box;
    PutBe32(box, static_cast<std::uint32_t>(8 + children.size()));
    PutType(box, type);
    box.insert(box.end(), children.begin(), children.end());
    return box;
}

std::vector<std::uint8_t> FtypBox() {
    std::vector<std::uint8_t> body;
    PutType(body, "mp42");
    PutBe32(body, 0);
    return BoxAround("ftyp", body);
}

fs::path WriteTemp(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
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
    std::vector<std::uint8_t> mdat;       // a dummy media-data box
    PutBe32(mdat, 8 + 16);
    PutType(mdat, "mdat");
    mdat.resize(8 + 16, 0xAB);
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
