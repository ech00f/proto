#include "proto/daemon.hpp"

#include "proto/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace proto {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t request_limit = 256 * 1024;
constexpr std::size_t response_limit = 2 * 1024 * 1024;
constexpr std::size_t worker_limit = 8;
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);

class Descriptor {
  public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    Descriptor(Descriptor &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    int get() const {
        return fd_;
    }

  private:
    int fd_;
};

extern "C" void stop_daemon(int) {
    stopping.store(true, std::memory_order_relaxed);
}

class DaemonSignals {
  public:
    DaemonSignals() {
        stopping.store(false, std::memory_order_relaxed);
        struct sigaction action{};
        action.sa_handler = stop_daemon;
        sigemptyset(&action.sa_mask);
        for (const int signal : {SIGINT, SIGTERM, SIGHUP}) {
            struct sigaction previous{};
            if (::sigaction(signal, &action, &previous) != 0) {
                restore();
                throw std::runtime_error("Could not install daemon signal handlers");
            }
            previous_.emplace_back(signal, previous);
        }
    }
    ~DaemonSignals() {
        restore();
    }
    DaemonSignals(const DaemonSignals &) = delete;
    DaemonSignals &operator=(const DaemonSignals &) = delete;

  private:
    void restore() {
        for (const auto &[signal, previous] : previous_) {
            ::sigaction(signal, &previous, nullptr);
        }
    }
    std::vector<std::pair<int, struct sigaction>> previous_;
};

sockaddr_un address(const std::filesystem::path &path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    const auto name = path.string();
    if (!path.is_absolute() || !printable_utf8(name) || name.size() >= sizeof(result.sun_path)) {
        throw std::runtime_error("PROTO_DAEMON_SOCKET must be a short, printable absolute path");
    }
    std::memcpy(result.sun_path, name.c_str(), name.size() + 1);
    return result;
}

void private_directory(const std::filesystem::path &path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077) != 0) {
        throw std::runtime_error(
            "Daemon socket directory must be owned, private, and not a symlink");
    }
}

bool owned_socket(const struct stat &info) {
    return S_ISSOCK(info.st_mode) && info.st_uid == ::geteuid() && info.st_nlink == 1 &&
           (info.st_mode & 0077) == 0;
}

void private_peer(int fd) {
#if defined(__linux__)
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        credentials.uid != ::geteuid()) {
        throw std::runtime_error("Daemon connection requires the same user");
    }
#else
    uid_t user = 0;
    gid_t group = 0;
    if (::getpeereid(fd, &user, &group) != 0 || user != ::geteuid()) {
        throw std::runtime_error("Daemon connection requires the same user");
    }
#endif
}

void configure_socket(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        throw std::runtime_error("Could not configure daemon socket");
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw std::runtime_error("Could not configure daemon socket signals");
    }
#endif
}

void wait_socket(int fd, short events, Clock::time_point deadline, Cancellation cancellation) {
    for (;;) {
        if (cancellation.cancelled()) {
            throw RequestCancelled();
        }
        if (Clock::now() >= deadline) {
            throw std::runtime_error("Proto daemon request timed out");
        }
        struct pollfd descriptor{fd, events, 0};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        const int timeout = static_cast<int>(std::clamp<long long>(remaining.count(), 1, 50));
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready > 0) {
            if ((descriptor.revents & events) != 0) {
                return;
            }
            if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                throw std::runtime_error("Proto daemon connection closed");
            }
        } else if (ready < 0 && errno != EINTR) {
            throw std::runtime_error("Could not wait for proto daemon");
        }
    }
}

void transfer(int fd, char *bytes, std::size_t size, bool writing, Clock::time_point deadline,
              Cancellation cancellation) {
    std::size_t position = 0;
    while (position < size) {
        wait_socket(fd, writing ? POLLOUT : POLLIN, deadline, cancellation);
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto count = writing ? ::send(fd, bytes + position, size - position, flags)
                                   : ::recv(fd, bytes + position, size - position, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Proto daemon connection closed");
        }
        position += static_cast<std::size_t>(count);
    }
}

void send_frame(int fd, const nlohmann::json &data, std::size_t limit, Clock::time_point deadline,
                Cancellation cancellation = {}) {
    auto body = data.dump();
    if (body.size() > limit) {
        throw std::runtime_error("Proto daemon message exceeded its size limit");
    }
    std::array<char, 4> length{};
    for (std::size_t index = 0; index < length.size(); ++index) {
        length[index] = static_cast<char>((body.size() >> (8 * (3 - index))) & 255);
    }
    transfer(fd, length.data(), length.size(), true, deadline, cancellation);
    transfer(fd, body.data(), body.size(), true, deadline, cancellation);
}

nlohmann::json receive_frame(int fd, std::size_t limit, Clock::time_point deadline,
                             Cancellation cancellation = {}) {
    std::array<char, 4> length{};
    transfer(fd, length.data(), length.size(), false, deadline, cancellation);
    std::size_t size = 0;
    for (const char byte : length) {
        size = (size << 8) | static_cast<unsigned char>(byte);
    }
    if (size == 0 || size > limit) {
        throw std::runtime_error("Proto daemon message exceeded its size limit");
    }
    std::string body(size, '\0');
    transfer(fd, body.data(), body.size(), false, deadline, cancellation);
    const auto data = nlohmann::json::parse(body, nullptr, false);
    if (!data.is_object()) {
        throw std::runtime_error("Invalid proto daemon message");
    }
    return data;
}

Descriptor connect_daemon(const std::filesystem::path &path, Clock::time_point deadline,
                          Cancellation cancellation = {}) {
    if (path.empty()) {
        return Descriptor{};
    }
    if (!path.is_absolute() || !printable_utf8(path.string())) {
        throw std::runtime_error("PROTO_DAEMON_SOCKET must be a printable absolute path");
    }
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return Descriptor{};
        }
        throw std::runtime_error("Could not inspect proto daemon socket");
    }
    const auto location = address(path);
    private_directory(path.parent_path());
    if (!owned_socket(info)) {
        throw std::runtime_error("Proto daemon socket must be owned, private, and not a symlink");
    }
    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (socket.get() < 0) {
        throw std::runtime_error("Could not open proto daemon connection");
    }
    configure_socket(socket.get());
    if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&location), sizeof(location)) !=
        0) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            return Descriptor{};
        }
        if (errno != EINPROGRESS && errno != EAGAIN) {
            throw std::runtime_error("Could not connect to proto daemon");
        }
        wait_socket(socket.get(), POLLOUT, deadline, cancellation);
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            throw std::runtime_error("Could not connect to proto daemon");
        }
    }
    private_peer(socket.get());
    return socket;
}

class Listener {
  public:
    explicit Listener(std::filesystem::path path)
        : path_(std::move(path)), socket_(::socket(AF_UNIX, SOCK_STREAM, 0)) {
        const auto location = address(path_);
        if (socket_.get() < 0) {
            throw std::runtime_error("Could not create proto daemon socket");
        }
        if (::mkdir(path_.parent_path().c_str(), 0700) != 0 && errno != EEXIST) {
            throw std::runtime_error("Could not create proto daemon directory");
        }
        private_directory(path_.parent_path());
        lock_ = std::make_unique<Descriptor>(
            ::open((path_.string() + ".lock").c_str(),
                   O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
        struct stat lock_info{};
        if (lock_->get() < 0 || ::fstat(lock_->get(), &lock_info) != 0 ||
            !S_ISREG(lock_info.st_mode) || lock_info.st_uid != ::geteuid() ||
            lock_info.st_nlink != 1 || (lock_info.st_mode & 0077) != 0) {
            throw std::runtime_error("Could not safely lock proto daemon socket");
        }
        if (::flock(lock_->get(), LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Proto daemon is already running");
        }
        struct stat existing{};
        if (::lstat(path_.c_str(), &existing) == 0) {
            if (!owned_socket(existing)) {
                throw std::runtime_error("Refusing to replace an unsafe proto daemon socket");
            }
            const auto active =
                connect_daemon(path_, Clock::now() + std::chrono::milliseconds{250});
            if (active.get() >= 0) {
                throw std::runtime_error(
                    "Another service is already listening on the proto daemon socket");
            }
            if (::unlink(path_.c_str()) != 0) {
                throw std::runtime_error("Could not remove a stale proto daemon socket");
            }
        } else if (errno != ENOENT) {
            throw std::runtime_error("Could not inspect proto daemon socket");
        }
        configure_socket(socket_.get());
        const auto previous_mask = ::umask(0077);
        const int result =
            ::bind(socket_.get(), reinterpret_cast<const sockaddr *>(&location), sizeof(location));
        ::umask(previous_mask);
        if (result != 0) {
            throw std::runtime_error("Could not bind proto daemon socket");
        }
        if (::lstat(path_.c_str(), &info_) != 0 || !owned_socket(info_) ||
            ::chmod(path_.c_str(), 0600) != 0 || ::listen(socket_.get(), 16) != 0) {
            cleanup();
            throw std::runtime_error("Could not listen on proto daemon socket");
        }
    }
    ~Listener() {
        cleanup();
    }
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    int get() const {
        return socket_.get();
    }

  private:
    void cleanup() {
        struct stat current{};
        if (::lstat(path_.c_str(), &current) == 0 && current.st_dev == info_.st_dev &&
            current.st_ino == info_.st_ino && owned_socket(current)) {
            ::unlink(path_.c_str());
        }
    }
    std::filesystem::path path_;
    Descriptor socket_;
    std::unique_ptr<Descriptor> lock_;
    struct stat info_{};
};

struct Worker {
    explicit Worker(int fd) : socket(fd) {}
    ~Worker() {
        cancel();
        if (thread.joinable()) {
            thread.join();
        }
    }
    void cancel() {
        generation.store(1, std::memory_order_relaxed);
        ::shutdown(socket.get(), SHUT_RDWR);
    }
    Descriptor socket;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> received{false};
    std::atomic<bool> done{false};
    std::thread thread;
};

struct StopWorkers {
    const std::vector<std::unique_ptr<Worker>> &workers;
    ~StopWorkers() {
        for (const auto &worker : workers) {
            worker->cancel();
        }
    }
};

nlohmann::json generate_response(const nlohmann::json &request, Cancellation cancellation) {
    if (request.value("version", 0) != 1) {
        throw std::runtime_error("Unsupported proto daemon protocol");
    }
    const auto operation = request.at("op").get<std::string>();
    if (operation == "ping") {
        return {{"ok", true}, {"version", 1}, {"pid", ::getpid()}};
    }
    if (operation != "generate") {
        throw std::runtime_error("Unsupported proto daemon operation");
    }
    const auto &context = request.at("context");
    const auto prefix = context.at(context.contains("task") ? "task" : "prefix").get<std::string>();
    const auto instructions = request.at("instructions").get<std::string>();
    const auto tokens = request.at("max_tokens").get<int>();
    if (!context.is_object() || !can_send_prefix(prefix) || instructions.empty() ||
        instructions.size() > 8192 || !printable_utf8(instructions) || tokens < 1 ||
        tokens > 4096) {
        throw std::runtime_error("Invalid proto daemon generation request");
    }
    const auto &settings = request.at("config");
    Config config;
    config.home = "/";
    config.api_base = settings.at("api_base").get<std::string>();
    config.model = settings.at("model").get<std::string>();
    config.api_key = settings.at("api_key").get<std::string>();
    config.timeout = std::chrono::milliseconds(settings.at("timeout_ms").get<int>());
    config.use_daemon = false;
    if (config.api_key.size() > 8192) {
        throw std::runtime_error("Invalid proto daemon API key length");
    }
    return {{"ok", true},
            {"content", ApiClient(config).generate(context, instructions, tokens, cancellation)}};
}

void handle(Worker &worker) {
    const Cancellation cancellation{&worker.generation, 0};
    nlohmann::json response;
    try {
        const auto request = receive_frame(worker.socket.get(), request_limit,
                                           Clock::now() + std::chrono::seconds{2}, cancellation);
        worker.received.store(true, std::memory_order_release);
        response = generate_response(request, cancellation);
    } catch (const nlohmann::json::exception &) {
        response = {{"ok", false}, {"error", "Invalid proto daemon request"}};
    } catch (const std::exception &error) {
        response = {{"ok", false}, {"error", error.what()}};
    }
    send_frame(worker.socket.get(), response, response_limit,
               Clock::now() + std::chrono::seconds{2}, cancellation);
}

} // namespace

std::filesystem::path daemon_socket_path() {
    if (const char *path = std::getenv("PROTO_DAEMON_SOCKET")) {
        return path;
    }
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime != nullptr && std::filesystem::path(runtime).is_absolute()
               ? std::filesystem::path(runtime) / "proto/proto.sock"
               : std::filesystem::path{};
}

nlohmann::json daemon_status() {
    const auto path = daemon_socket_path();
    const auto deadline = Clock::now() + std::chrono::seconds{1};
    const auto socket = connect_daemon(path, deadline);
    if (socket.get() < 0) {
        return {{"running", false}, {"socket", path.string()}};
    }
    send_frame(socket.get(), {{"version", 1}, {"op", "ping"}}, request_limit, deadline);
    const auto response = receive_frame(socket.get(), response_limit, deadline);
    if (!response.value("ok", false) || response.value("version", 0) != 1 ||
        !response.contains("pid") || !response["pid"].is_number_integer()) {
        throw std::runtime_error("Invalid proto daemon status response");
    }
    return {{"running", true}, {"socket", path.string()}, {"pid", response["pid"]}};
}

std::optional<std::string> generate_via_daemon(const Config &config, const nlohmann::json &context,
                                               std::string_view instructions, int max_tokens,
                                               Cancellation cancellation) {
    const auto deadline = Clock::now() + config.timeout;
    const auto socket = connect_daemon(config.daemon_socket, deadline, cancellation);
    if (socket.get() < 0) {
        return std::nullopt;
    }
    const nlohmann::json request{{"version", 1},
                                 {"op", "generate"},
                                 {"context", context},
                                 {"instructions", instructions},
                                 {"max_tokens", max_tokens},
                                 {"config",
                                  {{"api_base", config.api_base},
                                   {"model", config.model},
                                   {"api_key", config.api_key},
                                   {"timeout_ms", config.timeout.count()}}}};
    send_frame(socket.get(), request, request_limit, deadline, cancellation);
    const auto response = receive_frame(socket.get(), response_limit, deadline, cancellation);
    if (!response.value("ok", false)) {
        const auto error = response.value("error", std::string{"Proto daemon request failed"});
        throw std::runtime_error(
            error.size() <= 256 && printable_utf8(error) ? error : "Proto daemon request failed");
    }
    if (!response.contains("content") || !response["content"].is_string()) {
        throw std::runtime_error("Invalid proto daemon generation response");
    }
    return response["content"].get<std::string>();
}

int serve_daemon() {
    const auto path = daemon_socket_path();
    if (path.empty()) {
        throw std::runtime_error("Set XDG_RUNTIME_DIR or PROTO_DAEMON_SOCKET to run the daemon");
    }
    const DaemonSignals signals;
    const Listener listener(path);
    initialize_http();
    std::vector<std::unique_ptr<Worker>> workers;
    const StopWorkers stop{workers};
    std::cout << "proto: daemon ready" << std::endl;
    while (!stopping.load(std::memory_order_relaxed)) {
        std::erase_if(workers, [](const auto &worker) {
            return worker->done.load(std::memory_order_acquire);
        });
        std::vector<struct pollfd> descriptors{{listener.get(), POLLIN, 0}};
        for (const auto &worker : workers) {
            descriptors.push_back({worker->socket.get(), POLLIN, 0});
        }
        const int ready = ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 50);
        if (ready < 0 && errno != EINTR) {
            throw std::runtime_error("Could not wait for daemon connections");
        }
        for (std::size_t index = 0; index < workers.size(); ++index) {
            const auto events = descriptors[index + 1].revents;
            bool closed = (events & (POLLHUP | POLLERR | POLLNVAL)) != 0;
            if ((events & POLLIN) != 0 &&
                workers[index]->received.load(std::memory_order_acquire)) {
                char byte = 0;
                const auto count =
                    ::recv(workers[index]->socket.get(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                closed = closed || count >= 0 ||
                         (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
            }
            if (closed) {
                workers[index]->cancel();
            }
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            const int fd = ::accept(listener.get(), nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            auto worker = std::make_unique<Worker>(fd);
            try {
                configure_socket(fd);
                private_peer(fd);
                if (workers.size() >= worker_limit) {
                    continue;
                }
                worker->thread = std::thread([current = worker.get()] {
                    try {
                        handle(*current);
                    } catch (...) {
                    }
                    current->done.store(true, std::memory_order_release);
                });
                workers.push_back(std::move(worker));
            } catch (const std::exception &) {
            }
        }
    }
    return 0;
}

} // namespace proto
