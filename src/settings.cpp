#include "proto/config.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace proto {
namespace {

class File {
  public:
    explicit File(int fd) : fd_(fd) {}
    ~File() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const {
        return fd_;
    }

  private:
    int fd_;
};

void fail() {
    throw std::runtime_error("Cannot safely read/write proto settings; use an owned private regular file");
}

bool owned_directory(int fd) {
    struct stat info{};
    return ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::getuid();
}

} // namespace

nlohmann::json Config::json() const {
    return {{"version", 1}, {"api_base", api_base}, {"model", model},
            {"ai_enabled", ai_enabled}, {"context", context_name(context_scope)},
            {"suggestions", suggestion_count}, {"max_entries", max_entries},
            {"debounce_ms", debounce.count()}, {"timeout_ms", timeout.count()},
            {"persist_history", persist_history}, {"send_history", send_history}, {"theme", theme}};
}

void Config::load_file() {
    if (config_file.empty()) {
        return;
    }
    const File parent(::open(config_file.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (parent.get() < 0 && errno == ENOENT) {
        return;
    }
    if (parent.get() < 0 || !owned_directory(parent.get())) {
        fail();
    }
    const File file(::openat(parent.get(), config_file.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0 && errno == ENOENT) {
        return;
    }
    struct stat info{};
    if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != ::getuid() || info.st_nlink != 1 || (info.st_mode & 0077) != 0 ||
        info.st_size < 0 || info.st_size > 65536) {
        fail();
    }
    std::string contents;
    std::array<char, 4096> bytes{};
    for (;;) {
        const auto count = ::read(file.get(), bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            fail();
        }
        if (count == 0) {
            break;
        }
        contents.append(bytes.data(), static_cast<std::size_t>(count));
        if (contents.size() > 65536) {
            fail();
        }
    }
    try {
        const auto data = nlohmann::json::parse(contents);
        if (!data.is_object() || data.value("version", 1) != 1 || data.contains("api_key")) {
            throw std::runtime_error("Invalid config schema");
        }
        api_base = data.value("api_base", api_base);
        model = data.value("model", model);
        ai_enabled = data.value("ai_enabled", ai_enabled);
        suggestion_count = data.value("suggestions", suggestion_count);
        max_entries = data.value("max_entries", max_entries);
        debounce = std::chrono::milliseconds(data.value("debounce_ms", debounce.count()));
        timeout = std::chrono::milliseconds(data.value("timeout_ms", timeout.count()));
        persist_history = data.value("persist_history", persist_history);
        send_history = data.value("send_history", send_history);
        theme = data.value("theme", theme);
        const auto scope = data.value("context", std::string(context_name(context_scope)));
        if (scope != "none" && scope != "cwd" && scope != "home") {
            throw std::runtime_error("Invalid context scope");
        }
        context_scope = scope == "none" ? ContextScope::None :
                        scope == "cwd" ? ContextScope::CurrentDirectory : ContextScope::Home;
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid proto config.json; API keys belong in the environment, not this file");
    }
}

void Config::save() const {
    auto checked = *this;
    checked.ai_enabled = true;
    checked.validate();
    if (!config_file.is_absolute()) {
        fail();
    }
    std::error_code error;
    std::filesystem::create_directories(config_file.parent_path(), error);
    if (error) {
        fail();
    }
    const File parent(::open(config_file.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (parent.get() < 0 || !owned_directory(parent.get()) || ::fchmod(parent.get(), 0700) != 0) {
        fail();
    }
    struct stat existing{};
    const auto name = config_file.filename().string();
    if (::fstatat(parent.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_uid != ::getuid() || existing.st_nlink != 1) {
            fail();
        }
    } else if (errno != ENOENT) {
        fail();
    }
    static std::atomic<unsigned> sequence{0};
    const auto temporary = ".config-" + std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1));
    const File file(::openat(parent.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0) {
        fail();
    }
    try {
        const auto contents = json().dump(2) + '\n';
        std::size_t written = 0;
        while (written < contents.size()) {
            const auto count = ::write(file.get(), contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                fail();
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(file.get()) != 0 || ::renameat(parent.get(), temporary.c_str(), parent.get(), name.c_str()) != 0) {
            fail();
        }
        (void)::fsync(parent.get());
    } catch (...) {
        ::unlinkat(parent.get(), temporary.c_str(), 0);
        throw;
    }
}

} // namespace proto
