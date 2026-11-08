#include "proto/history.hpp"

#include "proto/text.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace proto {
namespace {

constexpr std::size_t max_history_bytes = 1024 * 1024;
constexpr std::size_t max_history_entries = 500;

class File {
  public:
    explicit File(int descriptor) : descriptor_(descriptor) {}
    ~File() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const {
        return descriptor_;
    }

  private:
    int descriptor_;
};

bool owned_regular_file(int descriptor, struct stat &status) {
    return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_uid == ::getuid() && status.st_size >= 0 &&
           static_cast<std::uintmax_t>(status.st_size) <= max_history_bytes;
}

} // namespace

History::History(const Config &config)
    : path_(config.state_directory / "proto/history"), enabled_(config.persist_history) {
    if (enabled_) {
        load();
    }
}

void History::load() {
    const File file(::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) {
        if (errno != ENOENT) {
            warning_ = "Could not safely open proto history";
        }
        return;
    }
    struct stat status {};
    if (!owned_regular_file(file.get(), status) || (status.st_mode & 0077) != 0) {
        warning_ = "Ignoring unsafe or oversized proto history (owner-only regular file required)";
        return;
    }
    if (::flock(file.get(), LOCK_SH) != 0) {
        warning_ = "Could not lock proto history";
        return;
    }
    std::string data;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > max_history_bytes) {
            warning_ = "Ignoring oversized proto history";
            return;
        }
    }
    std::size_t start = 0;
    while (start < data.size()) {
        const auto end = data.find('\n', start);
        if (end == data.npos) {
            break; // Ignore an incomplete final record.
        }
        const auto line = std::string_view(data).substr(start, end - start);
        if (can_send_prefix(line)) {
            entries_.emplace_back(line);
        }
        start = end + 1;
    }
    if (entries_.size() > max_history_entries) {
        entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(max_history_entries));
    }
}

void History::add(std::string_view command) {
    if (!can_send_prefix(command) || (!entries_.empty() && entries_.back() == command)) {
        return;
    }
    entries_.emplace_back(command);
    if (entries_.size() > max_history_entries) {
        entries_.erase(entries_.begin());
    }
    if (enabled_) {
        persist(command);
    }
}

void History::persist(std::string_view command) {
    std::error_code error;
    const auto directory = path_.parent_path();
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error))) {
        warning_ = "Refusing a symlinked proto history directory";
        return;
    }
    error.clear();
    std::filesystem::create_directories(directory, error);
    if (error || ::chmod(directory.c_str(), 0700) != 0) {
        warning_ = "Could not create private proto history directory";
        return;
    }
    const File file(::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW |
                                            O_NONBLOCK,
                           0600));
    struct stat status {};
    if (file.get() < 0 || !owned_regular_file(file.get(), status) || ::fchmod(file.get(), 0600) != 0 ||
        ::flock(file.get(), LOCK_EX) != 0) {
        warning_ = "Could not safely save proto history";
        return;
    }
    const std::string record = std::string(command) + '\n';
    if (static_cast<std::uintmax_t>(status.st_size) + record.size() > max_history_bytes) {
        warning_ = "Proto history reached 1 MiB; new entries remain in memory only";
        return;
    }
    std::size_t written = 0;
    while (written < record.size()) {
        const auto count = ::write(file.get(), record.data() + written, record.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            warning_ = "Could not write proto history";
            return;
        }
        written += static_cast<std::size_t>(count);
    }
}

const std::vector<std::string> &History::entries() const {
    return entries_;
}

const std::string &History::warning() const {
    return warning_;
}

} // namespace proto
