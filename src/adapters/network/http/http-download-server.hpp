#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace httplib {
class Server;
class DataSink;
}  // namespace httplib

namespace sst::adapters::network {

// Stream [offset, offset + length) of `file` into `sink` in bounded chunks.
// Returns true only when the full requested range was delivered; a short read
// (file truncated after its size was advertised) or a sink failure returns
// false so httplib aborts the response instead of re-invoking the provider at
// the same offset forever (busy-spin). Exposed for tests; used by the
// /recordings content provider.
auto StreamFileRange(const std::string& file, std::size_t offset, std::size_t length,
                     httplib::DataSink& sink) -> bool;

// cpp-httplib download server (KTD7). Serves recordings over HTTP with byte-
// range support, gated per-request by an Authorization: Bearer token, bound to
// a single address (the WiFi Direct GO IP) so downloads are reachable only over
// the phone link. The token validator maps a token to the file it authorizes.
class HttpDownloadServer {
   public:
    // Returns the file path a token authorizes, or nullopt to reject (401).
    using TokenValidator =
        std::function<std::optional<std::filesystem::path>(const std::string& token)>;

    // Returns the `<id>.jpg` thumbnail path for a recording id, or nullopt (404).
    // Thumbnails are served untokened — small, non-sensitive preview frames over
    // the P2P link the phone already had to join.
    using ThumbnailResolver =
        std::function<std::optional<std::filesystem::path>(const std::string& recording_id)>;

    HttpDownloadServer(std::string bind_address, std::uint16_t port, TokenValidator validator,
                       ThumbnailResolver thumbnail_resolver);
    ~HttpDownloadServer();

    HttpDownloadServer(const HttpDownloadServer&) = delete;
    auto operator=(const HttpDownloadServer&) -> HttpDownloadServer& = delete;
    HttpDownloadServer(HttpDownloadServer&&) = delete;
    auto operator=(HttpDownloadServer&&) -> HttpDownloadServer& = delete;

    // Bind + start serving on a background thread. Returns false if it can't
    // bind (e.g. address unavailable).
    auto Start() -> bool;
    auto Stop() -> void;
    [[nodiscard]] auto IsRunning() const -> bool { return running_; }
    // Actual bound port (useful when constructed with port 0 for an ephemeral
    // port, e.g. in tests).
    [[nodiscard]] auto BoundPort() const -> std::uint16_t { return bound_port_; }

   private:
    std::string bind_address_;
    std::uint16_t port_;
    std::uint16_t bound_port_{0};
    TokenValidator validator_;
    ThumbnailResolver thumbnail_resolver_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    bool running_{false};
};

}  // namespace sst::adapters::network
