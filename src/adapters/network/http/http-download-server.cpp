#include "adapters/network/http/http-download-server.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "httplib.h"

namespace sst::adapters::network {

namespace fs = std::filesystem;

namespace {

constexpr const char* kBearerPrefix = "Bearer ";

// HTTP status codes used by the download endpoint.
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpNotFound = 404;

// Stream recordings to the Range sink in fixed slices instead of allocating the
// whole requested length up front — a `Range: bytes=0-` on a multi-GB recording
// would otherwise try to allocate gigabytes in one buffer and OOM the Jetson.
constexpr std::size_t kStreamChunkBytes = 64UL * 1024;

// Thumbnails are small preview JPEGs; refuse anything larger so a bad/huge file
// can't be slurped whole into memory per request.
constexpr std::uintmax_t kMaxThumbnailBytes = 16UL * 1024 * 1024;

// Longest accepted recording/thumbnail id (a uuid-ish stem); anything longer is
// rejected outright.
constexpr std::size_t kMaxIdLength = 256;

// A recording/thumbnail id addresses one file by stem. Reject anything with a
// path separator or parent ref before it reaches the resolver: it both blocks
// traversal attempts and stops obviously-bad ids from driving the resolver's
// recursive directory scan (a DoS lever over the P2P link).
auto IsSafeId(const std::string& recording_id) -> bool {
    if (recording_id.empty() || recording_id.size() > kMaxIdLength) {
        return false;
    }
    if (recording_id.find('/') != std::string::npos ||
        recording_id.find('\\') != std::string::npos ||
        recording_id.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(recording_id.begin(), recording_id.end(), [](unsigned char chr) {
        return std::isalnum(chr) != 0 || chr == '-' || chr == '_' || chr == '.';
    });
}

// How long to wait for the accept loop to come up after bind, in 5 ms slices.
constexpr int kStartupPollAttempts = 200;
constexpr auto kStartupPollInterval = std::chrono::milliseconds(5);

auto ExtractBearer(const httplib::Request& req) -> std::string {
    if (!req.has_header("Authorization")) {
        return {};
    }
    const std::string auth = req.get_header_value("Authorization");
    if (!auth.starts_with(kBearerPrefix)) {
        return {};
    }
    return auth.substr(std::char_traits<char>::length(kBearerPrefix));
}

// Untokened thumbnail GET: resolve <id> -> <id>.jpg and serve it whole. The id
// is charset-validated and the file size-capped before it is read into memory.
void ServeThumbnail(const HttpDownloadServer::ThumbnailResolver& resolve,
                    const httplib::Request& req, httplib::Response& res) {
    const std::string recording_id = req.matches.size() > 1 ? req.matches[1].str() : "";
    const std::optional<fs::path> path =
        IsSafeId(recording_id) ? resolve(recording_id) : std::nullopt;
    if (!path) {
        res.status = kHttpNotFound;
        res.set_content("not found", "text/plain");
        return;
    }
    std::error_code size_ec;
    const auto size = fs::file_size(*path, size_ec);
    if (size_ec || size > kMaxThumbnailBytes) {
        res.status = kHttpNotFound;
        res.set_content("not found", "text/plain");
        return;
    }
    std::ifstream stream(path->string(), std::ios::binary);
    if (!stream) {
        res.status = kHttpNotFound;
        res.set_content("not found", "text/plain");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    res.set_content(std::move(body), "image/jpeg");
}

// Stream the requested byte range of `file` to `sink` in bounded chunks, so a
// huge `length` can't trigger a multi-GB allocation on the Jetson.
// NOLINTBEGIN(bugprone-easily-swappable-parameters) floor-ok: httplib provider param order
auto StreamFileRange(const std::string& file, std::size_t offset, std::size_t length,
                     httplib::DataSink& sink) -> bool {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(offset));
    std::array<char, kStreamChunkBytes> chunk{};
    std::size_t remaining = length;
    while (remaining > 0 && stream) {
        const std::size_t want = std::min(remaining, chunk.size());
        stream.read(chunk.data(), static_cast<std::streamsize>(want));
        const auto got = static_cast<std::size_t>(stream.gcount());
        if (got == 0) {
            break;
        }
        if (!sink.write(chunk.data(), got)) {
            return false;
        }
        remaining -= got;
    }
    return true;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// Tokened recording GET with Range support: validate the bearer token, then hand
// httplib a known-length content provider that streams the requested slice.
void ServeRecording(const HttpDownloadServer::TokenValidator& validate, const httplib::Request& req,
                    httplib::Response& res) {
    const std::string token = ExtractBearer(req);
    const std::optional<fs::path> path = token.empty() ? std::nullopt : validate(token);
    if (!path) {
        res.status = kHttpUnauthorized;
        res.set_content("unauthorized", "text/plain");
        return;
    }
    std::error_code err;
    const auto size = static_cast<std::size_t>(fs::file_size(*path, err));
    if (err) {
        res.status = kHttpNotFound;
        res.set_content("not found", "text/plain");
        return;
    }
    const std::string file = path->string();
    res.set_content_provider(
        size, "video/mp4",
        [file](std::size_t offset,  // NOLINT(bugprone-easily-swappable-parameters) floor-ok:
                                    // (offset,length) order fixed by httplib ContentProvider
                                    // callback contract
               std::size_t length, httplib::DataSink& sink) -> bool {
            return StreamFileRange(file, offset, length, sink);
        });
}

}  // namespace

HttpDownloadServer::HttpDownloadServer(std::string bind_address, std::uint16_t port,
                                       TokenValidator validator,
                                       ThumbnailResolver thumbnail_resolver)
    : bind_address_(std::move(bind_address)),
      port_(port),
      validator_(std::move(validator)),
      thumbnail_resolver_(std::move(thumbnail_resolver)),
      server_(std::make_unique<httplib::Server>()) {
    // Thumbnails: untokened GET by recording id (small, non-sensitive preview
    // frames over the P2P link the phone already had to join).
    server_->Get("/thumbnails/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        ServeThumbnail(thumbnail_resolver_, req, res);
    });
    // Recordings: tokened GET with byte-range support.
    server_->Get("/recordings/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        ServeRecording(validator_, req, res);
    });
}

HttpDownloadServer::~HttpDownloadServer() { Stop(); }

auto HttpDownloadServer::Start() -> bool {
    if (running_) {
        return true;
    }
    // Bind synchronously so we can report bind failure to the caller. NOTE:
    // httplib's bind_to_port returns 1=success / 0=failure — NOT the bound port.
    // So for a fixed port we keep port_ (using the return value as the port is
    // the bug that made telemetry/logs report ":1"). Only an ephemeral request
    // (port 0, used by tests) needs bind_to_any_port, which DOES return the
    // actual assigned port.
    if (port_ == 0) {
        const int assigned = server_->bind_to_any_port(bind_address_);
        if (assigned <= 0) {
            spdlog::error("HttpDownloadServer: bind {}:0 (ephemeral) failed", bind_address_);
            return false;
        }
        bound_port_ = static_cast<std::uint16_t>(assigned);
    } else {
        if (!server_->bind_to_port(bind_address_, port_)) {
            spdlog::error("HttpDownloadServer: bind {}:{} failed", bind_address_, port_);
            return false;
        }
        bound_port_ = port_;
    }
    running_ = true;
    thread_ = std::thread([this] { server_->listen_after_bind(); });

    // Wait until the accept loop is actually running so callers (and tests) can
    // connect immediately after Start() returns.
    for (int i = 0; i < kStartupPollAttempts && !server_->is_running(); ++i) {
        std::this_thread::sleep_for(kStartupPollInterval);
    }
    spdlog::info("HttpDownloadServer: serving on http://{}:{}", bind_address_, bound_port_);
    return true;
}

auto HttpDownloadServer::Stop() -> void {
    if (!running_) {
        return;
    }
    server_->stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
    spdlog::info("HttpDownloadServer: stopped");
}

}  // namespace sst::adapters::network
