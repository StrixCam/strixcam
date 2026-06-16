#include "adapters/network/http/http-download-server.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <thread>
#include <utility>

#include "httplib.h"

namespace sst::adapters::network {

namespace fs = std::filesystem;

namespace {

constexpr const char* kBearerPrefix = "Bearer ";

auto ExtractBearer(const httplib::Request& req) -> std::string {
    if (!req.has_header("Authorization")) {
        return {};
    }
    const std::string auth = req.get_header_value("Authorization");
    if (auth.rfind(kBearerPrefix, 0) != 0) {
        return {};
    }
    return auth.substr(std::char_traits<char>::length(kBearerPrefix));
}

}  // namespace

HttpDownloadServer::HttpDownloadServer(std::string bind_address, std::uint16_t port,
                                       TokenValidator validator)
    : bind_address_(std::move(bind_address)),
      port_(port),
      validator_(std::move(validator)),
      server_(std::make_unique<httplib::Server>()) {
    server_->Get("/recordings/(.*)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string token = ExtractBearer(req);
        std::optional<fs::path> path = token.empty() ? std::nullopt : validator_(token);
        if (!path) {
            res.status = 401;
            res.set_content("unauthorized", "text/plain");
            return;
        }
        std::error_code ec;
        const auto size = static_cast<std::size_t>(fs::file_size(*path, ec));
        if (ec) {
            res.status = 404;
            res.set_content("not found", "text/plain");
            return;
        }
        const std::string file = path->string();
        // Content provider with known length: httplib serves Range requests
        // (206 partial content) by invoking this for the requested slice.
        res.set_content_provider(
            size, "video/mp4",
            [file](std::size_t offset, std::size_t length, httplib::DataSink& sink) -> bool {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    return false;
                }
                in.seekg(static_cast<std::streamoff>(offset));
                std::string buf(length, '\0');
                in.read(buf.data(), static_cast<std::streamsize>(length));
                const auto got = static_cast<std::size_t>(in.gcount());
                return sink.write(buf.data(), got);
            });
    });
}

HttpDownloadServer::~HttpDownloadServer() { Stop(); }

auto HttpDownloadServer::Start() -> bool {
    if (running_) {
        return true;
    }
    // Bind synchronously so we can report bind failure to the caller.
    const int bound =
        static_cast<const int>(server_->bind_to_port(bind_address_, port_));
    if (bound <= 0) {
        spdlog::error("HttpDownloadServer: bind {}:{} failed", bind_address_, port_);
        return false;
    }
    bound_port_ = static_cast<std::uint16_t>(bound);
    running_ = true;
    thread_ = std::thread([this] { server_->listen_after_bind(); });

    // Wait until the accept loop is actually running so callers (and tests) can
    // connect immediately after Start() returns.
    for (int i = 0; i < 200 && !server_->is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
