#include "proto/shell.hpp"

#include "proto/context.hpp"
#include "proto/signals.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <wordexp.h>

extern char **environ;

namespace proto {
namespace {

class IgnoredSignals {
  public:
    IgnoredSignals() {
        struct sigaction action{};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        for (std::size_t index = 0; index < signals_.size(); ++index) {
            if (::sigaction(signals_[index], &action, &previous_[index]) != 0) {
                for (std::size_t previous = 0; previous < installed_; ++previous) {
                    ::sigaction(signals_[previous], &previous_[previous], nullptr);
                }
                throw std::runtime_error("Could not prepare command signal handling");
            }
            ++installed_;
        }
    }
    ~IgnoredSignals() {
        for (std::size_t index = 0; index < installed_; ++index) {
            ::sigaction(signals_[index], &previous_[index], nullptr);
        }
    }
    IgnoredSignals(const IgnoredSignals &) = delete;
    IgnoredSignals &operator=(const IgnoredSignals &) = delete;

  private:
    static constexpr std::array signals_{SIGINT, SIGQUIT, SIGTSTP, SIGTTOU};
    std::array<struct sigaction, signals_.size()> previous_{};
    std::size_t installed_ = 0;
};

std::optional<std::vector<std::string>> builtin_words(const std::string &command) {
    const auto first = command.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::vector<std::string>{};
    }
    const auto end = command.find_first_of(" \t", first);
    const auto name = std::string_view(command).substr(first, end - first);
    if (name != "cd" && name != "exit" && name != "jobs" && name != "fg") {
        return std::nullopt;
    }
    wordexp_t expanded{};
    const int result = ::wordexp(command.c_str(), &expanded, WRDE_NOCMD);
    if (result != 0) {
        if (result == WRDE_NOSPACE) {
            ::wordfree(&expanded);
        }
        // Operators, redirections, and command substitutions are delegated to the user's shell.
        return std::nullopt;
    }
    std::vector<std::string> words;
    for (std::size_t index = 0; index < expanded.we_wordc; ++index) {
        words.emplace_back(expanded.we_wordv[index]);
    }
    ::wordfree(&expanded);
    return words;
}

std::optional<int> integer(std::string_view value) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace

Shell::Shell(Config config) : config_(std::move(config)) {}

Shell::~Shell() {
    for (const auto &job : jobs_) {
        ::kill(-job.pid, SIGHUP);
        ::kill(-job.pid, SIGCONT);
        (void)::waitpid(job.pid, nullptr, WNOHANG);
    }
}

int Shell::last_status() const {
    return last_status_;
}

ShellResult Shell::execute(const std::string &command) {
    const auto parsed = builtin_words(command);
    if (parsed && parsed->empty()) {
        return {false, last_status_};
    }
    if (parsed) {
        const auto &words = *parsed;
        if (words[0] == "exit") {
            if (words.size() > 2 || (words.size() == 2 && !integer(words[1]))) {
                std::cerr << "proto: exit accepts one numeric status\n";
                last_status_ = 2;
                return {false, last_status_};
            }
            return {true, words.size() == 2 ? (*integer(words[1]) & 255) : last_status_};
        }
        if (words[0] == "cd") {
            std::size_t operand = words.size() > 1 && words[1] == "--" ? 2 : 1;
            if (words.size() > operand + 1) {
                std::cerr << "proto: cd: too many arguments\n";
                last_status_ = 1;
                return {false, last_status_};
            }
            const bool previous = words.size() > operand && words[operand] == "-";
            const std::filesystem::path target = previous ? previous_directory_
                                                 : words.size() > operand
                                                     ? std::filesystem::path(words[operand])
                                                     : config_.home;
            std::error_code error;
            const auto old = std::filesystem::current_path(error);
            if (!error && !target.empty()) {
                std::filesystem::current_path(target, error);
            } else if (target.empty()) {
                error = std::make_error_code(std::errc::no_such_file_or_directory);
            }
            if (error) {
                std::cerr << "proto: cd: " << error.message() << '\n';
                last_status_ = 1;
            } else {
                previous_directory_ = old;
                last_status_ = 0;
                if (previous) {
                    std::cout << display_path(std::filesystem::current_path(), config_.home)
                              << '\n';
                }
            }
            return {false, last_status_};
        }
        if (words[0] == "jobs") {
            for (const auto &job : jobs_) {
                std::cout << '[' << job.id << "] stopped " << job.command << '\n';
            }
            last_status_ = 0;
            return {false, last_status_};
        }
        if (words[0] == "fg") {
            const auto id = words.size() == 2 ? integer(words[1]) : std::optional<int>{};
            const auto job = words.size() == 1 && !jobs_.empty()
                                 ? std::prev(jobs_.end())
                                 : std::find_if(jobs_.begin(), jobs_.end(), [&](const Job &entry) {
                                       return id && entry.id == *id;
                                   });
            if (words.size() > 2 || job == jobs_.end()) {
                std::cerr << "proto: fg: no matching stopped job\n";
                last_status_ = 1;
            } else {
                auto running = *job;
                jobs_.erase(job);
                const IgnoredSignals ignored;
                last_status_ = wait(std::move(running), true);
            }
            return {false, last_status_};
        }
    }
    last_status_ = launch(command);
    return {false, last_status_};
}

int Shell::launch(const std::string &command) {
    const IgnoredSignals ignored;
    std::vector<char *> environment;
    for (char **entry = environ; *entry != nullptr; ++entry) {
        if (!std::string_view(*entry).starts_with("PROTO_API_KEY=")) {
            environment.push_back(*entry);
        }
    }
    environment.push_back(nullptr);
    char *arguments[]{config_.shell.data(), const_cast<char *>("-c"),
                      const_cast<char *>(command.c_str()), nullptr};
    posix_spawnattr_t attributes{};
    int result = ::posix_spawnattr_init(&attributes);
    if (result != 0) {
        throw std::runtime_error("Could not initialize command process attributes");
    }
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int signal : {SIGINT, SIGQUIT, SIGTSTP, SIGTTOU, SIGTERM, SIGHUP}) {
        sigaddset(&defaults, signal);
    }
    sigset_t mask;
    sigemptyset(&mask);
    result = ::posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (result == 0) {
        result = ::posix_spawnattr_setsigmask(&attributes, &mask);
    }
    if (result == 0) {
        result = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result == 0) {
        result = ::posix_spawnattr_setflags(
            &attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
    }
    pid_t child = -1;
    if (result == 0) {
        result = ::posix_spawn(&child, config_.shell.c_str(), nullptr, &attributes, arguments,
                               environment.data());
    }
    ::posix_spawnattr_destroy(&attributes);
    if (result != 0) {
        std::cerr << "proto: could not start shell: " << std::strerror(result) << '\n';
        return 126;
    }
    return wait({next_job_id_++, child, command}, false);
}

int Shell::wait(Job job, bool resume) {
    const auto parent_group = ::tcgetpgrp(STDIN_FILENO);
    if (parent_group >= 0) {
        (void)::tcsetpgrp(STDIN_FILENO, job.pid);
    }
    if (resume) {
        ::kill(-job.pid, SIGCONT);
    }
    int status = 0;
    int termination = 0;
    bool initial_terminal_stop = true;
    for (;;) {
        const int signal = take_signal();
        if (signal != 0) {
            ::kill(-job.pid, signal);
            if (signal != SIGINT) {
                termination = signal;
            }
        }
        const auto result = ::waitpid(job.pid, &status, WUNTRACED);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result > 0 && parent_group >= 0 && initial_terminal_stop && WIFSTOPPED(status) &&
            (WSTOPSIG(status) == SIGTTIN || WSTOPSIG(status) == SIGTTOU)) {
            // posix_spawn may run a fast reader before the parent hands over the terminal.
            initial_terminal_stop = false;
            (void)::tcsetpgrp(STDIN_FILENO, job.pid);
            ::kill(-job.pid, SIGCONT);
            continue;
        }
        if (parent_group >= 0) {
            (void)::tcsetpgrp(STDIN_FILENO, parent_group);
        }
        if (result < 0) {
            std::cerr << "proto: could not wait for shell command\n";
            return 1;
        }
        break;
    }
    if (termination != 0) {
        // Re-deliver after ignored dispositions are restored, so the next editor exits cleanly.
        ::kill(::getpid(), termination);
    }
    if (WIFSTOPPED(status)) {
        std::cout << '[' << job.id << "] stopped " << job.command << " (use fg to resume)\n";
        jobs_.push_back(std::move(job));
        return 128 + WSTOPSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

} // namespace proto
