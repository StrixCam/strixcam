#include "adapters/control/network/subprocess.hpp"

#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

namespace sst::adapters::control {

namespace {
constexpr int kExecFailedExitCode = 127;
constexpr std::int64_t kPollSliceMs = 100;  // re-check the deadline ~10x/sec

// Build the NULL-terminated char* argv proto execvp wants from the string argv.
// The pointers alias `argv`'s storage, so `argv` must outlive the returned
// vector (it does — both are locals in the callers below).
auto ToCArgv(const std::vector<std::string>& argv) -> std::vector<char*> {
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        c_argv.push_back(
            const_cast<char*>(arg.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    c_argv.push_back(nullptr);
    return c_argv;
}

// Reap `pid`, bounded by `timeout`: poll waitpid(WNOHANG) on a short cadence
// until the child exits or the deadline passes; on timeout SIGKILL + blocking
// reap (the kill makes the final wait return promptly). Returns true iff the
// child exited 0 within the deadline.
auto ReapBounded(pid_t pid, std::chrono::seconds timeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (true) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0) {
            return false;  // ECHILD / EINTR-storm — treat as failure
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("subprocess: child {} timed out after {}s — SIGKILL", pid,
                          timeout.count());
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);  // reap the killed child (returns promptly)
            return false;
        }
        const timespec slice{.tv_sec = 0, .tv_nsec = kPollSliceMs * 1000 * 1000};
        ::nanosleep(&slice, nullptr);
    }
}
}  // namespace

auto RunBounded(const std::vector<std::string>& argv, std::chrono::seconds timeout) -> bool {
    if (argv.empty()) {
        return false;
    }
    std::vector<char*> c_argv = ToCArgv(argv);

    const pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("subprocess: fork failed: {}", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        ::execvp(c_argv[0], c_argv.data());
        ::_exit(kExecFailedExitCode);  // only reached if exec fails
    }
    return ReapBounded(pid, timeout);
}

auto CaptureBounded(const std::vector<std::string>& argv,
                    std::chrono::seconds timeout) -> CaptureResult {
    CaptureResult result;
    if (argv.empty()) {
        return result;
    }
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        spdlog::error("subprocess: pipe failed: {}", std::strerror(errno));
        return result;
    }

    std::vector<char*> c_argv = ToCArgv(argv);
    const pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("subprocess: fork failed: {}", std::strerror(errno));
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return result;
    }
    if (pid == 0) {
        ::close(pipe_fds[0]);  // child: read end unused
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[1]);
        ::execvp(c_argv[0], c_argv.data());
        ::_exit(kExecFailedExitCode);
    }

    ::close(pipe_fds[1]);  // parent: write end unused
    const int read_fd = pipe_fds[0];
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, kReadBufferSize> buffer{};
    bool timed_out = false;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{.fd = read_fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;  // poll error — stop reading
        }
        if (ready == 0) {
            timed_out = true;  // deadline hit with no data
            break;
        }
        const ssize_t bytes_read = ::read(read_fd, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (bytes_read == 0) {
            break;  // EOF — child closed stdout
        }
        result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    }
    ::close(read_fd);

    if (timed_out) {
        spdlog::error("subprocess: capture child {} timed out after {}s — SIGKILL", pid,
                      timeout.count());
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        result.ok = false;
        return result;
    }
    result.ok = ReapBounded(pid, timeout);
    return result;
}

}  // namespace sst::adapters::control
