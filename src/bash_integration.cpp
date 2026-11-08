#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/editor.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/plan.hpp"
#include "proto/safety.hpp"
#include "proto/signals.hpp"
#include "proto/suggestions.hpp"
#include "proto/text.hpp"
#include "proto/tui.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <readline/readline.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" char **proto_bash_environment();
extern "C" const char *proto_bash_variable(const char *name);

namespace {

using Clock = std::chrono::steady_clock;

class BlockChildSignal {
  public:
    BlockChildSignal() {
        sigset_t mask;
        ::sigemptyset(&mask);
        ::sigaddset(&mask, SIGCHLD);
        if (::sigprocmask(SIG_BLOCK, &mask, &previous_) != 0) {
            throw std::runtime_error("Could not protect completion process ownership");
        }
    }
    ~BlockChildSignal() {
        ::sigprocmask(SIG_SETMASK, &previous_, nullptr);
    }
    BlockChildSignal(const BlockChildSignal &) = delete;
    BlockChildSignal &operator=(const BlockChildSignal &) = delete;

  private:
    sigset_t previous_{};
};

class ShellSignals {
  public:
    ShellSignals() : guard_(std::make_unique<proto::SignalGuard>()) {}
    ~ShellSignals() {
        guard_.reset();
        const int signal = proto::take_signal();
        if (signal != 0 && signal != SIGINT) {
            ::kill(::getpid(), signal);
        }
    }
    ShellSignals(const ShellSignals &) = delete;
    ShellSignals &operator=(const ShellSignals &) = delete;

  private:
    std::unique_ptr<proto::SignalGuard> guard_;
};

class CompletionProcess {
  public:
    ~CompletionProcess() {
        stop();
    }
    void stop() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            try {
                const BlockChildSignal blocked;
                const auto result = ::waitpid(pid_, nullptr, WNOHANG);
                if (result == 0) {
                    ::kill(pid_, SIGTERM);
                    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
                    }
                }
            } catch (...) {
            }
            pid_ = -1;
        }
        contents_.clear();
    }
    void start(const std::string &binary, const std::string &prefix, const proto::Config &config) {
        stop();
        std::vector<std::string> environment;
        for (char **item = proto_bash_environment(); item != nullptr && *item != nullptr; ++item) {
            environment.emplace_back(*item);
        }
        const auto put = [&](std::string_view name, std::string value) {
            const auto key = std::string(name) + '=';
            std::erase_if(environment, [&](const auto &entry) { return entry.starts_with(key); });
            environment.push_back(key + value);
        };
        const auto settings = config.shell_settings();
        for (const auto &[name, value] : settings.items()) {
            put(name, value.get<std::string>());
        }
        put("PROTO_API_KEY", config.api_key);
        put("PROTO_SHELL", config.shell);
        std::vector<char *> env;
        for (auto &entry : environment) {
            env.push_back(entry.data());
        }
        env.push_back(nullptr);
        int pipe[2];
        if (::pipe(pipe) != 0) {
            throw std::runtime_error("Could not open completion pipe");
        }
        for (const int fd : pipe) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        ::fcntl(pipe[0], F_SETFL, O_NONBLOCK);
        const BlockChildSignal blocked;
        posix_spawn_file_actions_t actions{};
        posix_spawnattr_t attributes{};
        int error = ::posix_spawn_file_actions_init(&actions);
        if (error != 0) {
            ::close(pipe[0]);
            ::close(pipe[1]);
            throw std::runtime_error("Could not prepare completion process");
        }
        error = ::posix_spawnattr_init(&attributes);
        const bool initialized = error == 0;
        const auto add = [&](int status) {
            if (error == 0) {
                error = status;
            }
        };
        add(::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        add(::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
        add(::posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO));
        add(::posix_spawn_file_actions_addclose(&actions, pipe[0]));
        add(::posix_spawn_file_actions_addclose(&actions, pipe[1]));
        if (initialized) {
            sigset_t mask;
            sigset_t defaults;
            ::sigemptyset(&mask);
            ::sigemptyset(&defaults);
            for (const int signal :
                 {SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) {
                ::sigaddset(&defaults, signal);
            }
            add(::posix_spawnattr_setsigmask(&attributes, &mask));
            add(::posix_spawnattr_setsigdefault(&attributes, &defaults));
            add(::posix_spawnattr_setpgroup(&attributes, 0));
            add(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK |
                                                            POSIX_SPAWN_SETSIGDEF |
                                                            POSIX_SPAWN_SETPGROUP));
        }
        char *arguments[]{const_cast<char *>(binary.c_str()), const_cast<char *>("--suggest-json"),
                          const_cast<char *>(prefix.c_str()), nullptr};
        if (error == 0) {
            error =
                ::posix_spawn(&pid_, binary.c_str(), &actions, &attributes, arguments, env.data());
        }
        if (initialized) {
            ::posix_spawnattr_destroy(&attributes);
        }
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(pipe[1]);
        if (error != 0) {
            ::close(pipe[0]);
            pid_ = -1;
            throw std::runtime_error("Could not start the proto completion helper");
        }
        fd_ = pipe[0];
    }
    std::optional<std::string> receive() {
        if (fd_ < 0) {
            return std::nullopt;
        }
        std::array<char, 4096> bytes{};
        for (;;) {
            const auto count = ::read(fd_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return std::nullopt;
            }
            if (count <= 0) {
                ::close(fd_);
                fd_ = -1;
                return std::exchange(contents_, {});
            }
            contents_.append(bytes.data(), static_cast<std::size_t>(count));
            if (contents_.size() > 128 * 1024) {
                stop();
                return std::string{};
            }
        }
    }

  private:
    int fd_ = -1;
    pid_t pid_ = -1;
    std::string contents_;
};

int accept(int count, int key);
int accept_word(int count, int key);
int next(int count, int key);
int previous(int count, int key);
int dismiss(int count, int key);
int menu(int count, int key);
int plan(int count, int key);
int submit(int count, int key);
int request(int count, int key);
void redisplay();
int idle();
int new_prompt();

struct Binding {
    Keymap map;
    std::string sequence;
    int type;
    rl_command_func_t *original;
    std::string macro;
    rl_command_func_t *replacement;
};

rl_command_func_t *lookup(const Binding &binding, int &type) {
    return rl_function_of_keyseq_len(binding.sequence.data(), binding.sequence.size(), binding.map,
                                     &type);
}

void bind(const Binding &binding, rl_command_func_t *function) {
    if (binding.sequence.size() == 1 && binding.sequence[0] == '\0') {
        rl_bind_key_in_map(0, function, binding.map);
    } else {
        rl_bind_keyseq_in_map(binding.sequence.c_str(), function, binding.map);
    }
}

int prompt_width() {
    const std::string_view prompt = rl_display_prompt != nullptr ? rl_display_prompt : "";
    std::string visible;
    bool hidden = false;
    for (const char value : prompt) {
        if (value == RL_PROMPT_START_IGNORE) {
            hidden = true;
        } else if (value == RL_PROMPT_END_IGNORE) {
            hidden = false;
        } else if (value == '\n' || value == '\r') {
            visible.clear();
        } else if (!hidden) {
            visible += value;
        }
    }
    return proto::printable_utf8(visible) ? proto::display_width(visible) : 100000;
}

struct State {
    explicit State(std::string executable)
        : config(proto::Config::from_environment()), history(config),
          binary(std::move(executable)) {
        if (const char *shell = proto_bash_variable("BASH")) {
            config.shell = shell;
        }
        config.validate();
    }
    void erase_ghost() {
        if (ghost_drawn) {
            std::fputs("\x1b[K", rl_outstream);
            std::fflush(rl_outstream);
            ghost_drawn = false;
        }
    }
    void observe(bool force = false) {
        const std::string line = rl_line_buffer != nullptr ? rl_line_buffer : "";
        const auto directory = std::filesystem::current_path();
        if (!force && line == prefix && rl_point == point && directory == cwd) {
            return;
        }
        process.stop();
        if (line != prefix || force) {
            suppressed = false;
            suggestions.filter(line);
        }
        prefix = line;
        point = rl_point;
        cwd = directory;
        pending = !suppressed && rl_point == rl_end && proto::can_send_prefix(line) &&
                  !line.starts_with("??") && (force || suggestions.size() == 0);
        due = Clock::now() + config.debounce;
    }
    void render() {
        if (suppressed || rl_point != rl_end || prefix.empty() ||
            RL_ISSTATE(RL_STATE_ISEARCH | RL_STATE_NSEARCH | RL_STATE_SEARCH | RL_STATE_COMPLETING |
                       RL_STATE_SIGHANDLER | RL_STATE_VIMOTION | RL_STATE_DONE)) {
            return;
        }
        int rows = 0;
        int columns = 0;
        rl_get_screen_size(&rows, &columns);
        const auto selected = suggestions.selected();
        const int available = columns - prompt_width() - proto::display_width(prefix) - 1;
        if (available <= 0 || selected.empty() || !selected.starts_with(prefix)) {
            return;
        }
        const auto risk = proto::assess_command(selected);
        const auto color = risk.level == proto::RiskLevel::Danger    ? "\x1b[91m"
                           : risk.level == proto::RiskLevel::Caution ? "\x1b[33m"
                                                                     : "\x1b[90m";
        std::string suffix(selected.substr(prefix.size()));
        if (suggestions.size() > 1 || risk.level != proto::RiskLevel::None) {
            suffix += " [" + std::to_string(suggestions.index() + 1) + "/" +
                      std::to_string(suggestions.size());
            if (risk.level != proto::RiskLevel::None) {
                suffix += " " + std::string(proto::risk_name(risk.level));
            }
            suffix += ']';
        }
        const auto text = proto::display_width(suffix) > available
                              ? proto::clip_columns(suffix, available - 1) + "…"
                              : suffix;
        const std::string frame = "\0337" + std::string(color) + text + "\x1b[0m\0338";
        std::fputs(frame.c_str(), rl_outstream);
        std::fflush(rl_outstream);
        ghost_drawn = true;
    }
    int fallback(rl_command_func_t *function, int count, int key) {
        const std::string_view sequence(
            rl_executing_keyseq != nullptr ? rl_executing_keyseq : "",
            static_cast<std::size_t>(std::max(0, rl_key_sequence_length)));
        for (const auto &binding : bindings) {
            if (binding.map == rl_get_keymap() && binding.replacement == function &&
                binding.sequence == sequence) {
                if (binding.type == ISMACR) {
                    rl_push_macro_input(::strdup(binding.macro.c_str()));
                    return 0;
                }
                return binding.original != nullptr ? binding.original(count, key) : rl_ding();
            }
        }
        return rl_ding();
    }
    void resume_display() {
        ghost_drawn = false;
        if (const char *paste = rl_variable_value("enable-bracketed-paste");
            paste != nullptr && std::string_view(paste) == "on") {
            std::fputs("\x1b[?2004h", rl_outstream);
        }
        observe(true);
        rl_forced_update_display();
    }
    proto::Config config;
    proto::History history;
    std::string binary;
    proto::Suggestions suggestions;
    CompletionProcess process;
    std::vector<Binding> bindings;
    rl_hook_func_t *old_idle = rl_event_hook;
    rl_hook_func_t *old_prompt = rl_pre_input_hook;
    rl_voidfunc_t *old_redisplay = rl_redisplay_function;
    int old_timeout = 100000;
    std::string prefix;
    std::filesystem::path cwd;
    int point = -1;
    bool pending = false;
    bool suppressed = false;
    bool ghost_drawn = false;
    bool busy = false;
    Clock::time_point due{};
};

std::unique_ptr<State> state;

template <typename Function> int guarded(Function action) noexcept {
    try {
        return state ? action() : 0;
    } catch (const std::exception &) {
        if (state) {
            state->process.stop();
            state->pending = false;
        }
        return rl_ding();
    }
}

void redisplay() {
    if (!state || state->busy) {
        return;
    }
    state->busy = true;
    guarded([] {
        state->erase_ghost();
        state->observe();
        if (state->old_redisplay != nullptr) {
            state->old_redisplay();
        } else {
            rl_redisplay();
        }
        state->render();
        return 0;
    });
    state->busy = false;
}

int new_prompt() {
    return guarded([] {
        state->process.stop();
        state->suggestions.clear();
        state->prefix.clear();
        state->point = -1;
        state->suppressed = false;
        state->pending = false;
        state->ghost_drawn = false;
        return state->old_prompt != nullptr ? state->old_prompt() : 0;
    });
}

int idle() {
    return guarded([] {
        const int result = state->old_idle != nullptr ? state->old_idle() : 0;
        state->observe();
        if (state->pending && Clock::now() >= state->due) {
            state->pending = false;
            state->suggestions.assign(
                proto::complete_locals(state->prefix, state->cwd, state->config.home,
                                       state->history.entries(), state->config.suggestion_count));
            redisplay();
            if (state->config.ai_enabled) {
                state->process.start(state->binary, state->prefix, state->config);
            }
        }
        if (const auto output = state->process.receive()) {
            const auto document = nlohmann::json::parse(*output, nullptr, false);
            if (document.is_array()) {
                std::vector<std::string> candidates;
                for (const auto &value : document) {
                    if (value.is_object() && value.contains("command") &&
                        value["command"].is_string()) {
                        const auto command = proto::normalize_completion(
                            state->prefix, value["command"].get_ref<const std::string &>());
                        if (command && candidates.size() < state->config.suggestion_count &&
                            std::find(candidates.begin(), candidates.end(), *command) ==
                                candidates.end()) {
                            candidates.push_back(*command);
                        }
                    }
                }
                state->suggestions.assign(std::move(candidates));
                redisplay();
            }
        }
        return result;
    });
}

int accept(int count, int key) {
    return guarded([&] {
        const auto selected = state->suggestions.selected();
        if (!state->suppressed && rl_point == rl_end && selected.starts_with(state->prefix) &&
            selected.size() > state->prefix.size()) {
            state->erase_ghost();
            const auto suffix = std::string(selected.substr(state->prefix.size()));
            rl_insert_text(suffix.c_str());
            state->suggestions.clear();
            state->observe();
            state->suppressed = true;
            state->pending = false;
            return 0;
        }
        return state->fallback(accept, count, key);
    });
}

int accept_word(int count, int key) {
    return guarded([&] {
        proto::LineBuffer line;
        line.replace(state->prefix);
        if (!state->suppressed && rl_point == rl_end &&
            line.accept(state->suggestions.selected(), true)) {
            state->erase_ghost();
            rl_insert_text(line.text().substr(state->prefix.size()).c_str());
            state->observe();
            return 0;
        }
        return state->fallback(accept_word, count, key);
    });
}

int cycle(int direction) {
    return guarded([&] {
        if (rl_point == rl_end) {
            state->suggestions.cycle(direction);
            state->suppressed = false;
            if (state->suggestions.size() == 0) {
                state->observe(true);
            }
        }
        return 0;
    });
}

int next(int, int) {
    return cycle(1);
}
int previous(int, int) {
    return cycle(-1);
}

int dismiss(int, int) {
    return guarded([] {
        state->erase_ghost();
        state->suggestions.clear();
        state->process.stop();
        state->pending = false;
        state->suppressed = true;
        return 0;
    });
}

int request(int, int) {
    return guarded([] {
        state->suggestions.clear();
        state->observe(true);
        state->due = Clock::now();
        return 0;
    });
}

int menu(int, int) {
    return guarded([] {
        state->erase_ghost();
        state->process.stop();
        {
            const ShellSignals signals;
            if (const auto recalled = proto::show_menu(
                    state->config, std::filesystem::current_path(), state->history.entries())) {
                rl_replace_line(recalled->c_str(), 0);
                rl_point = rl_end;
            }
        }
        state->history.configure(state->config);
        state->suggestions.clear();
        state->resume_display();
        return 0;
    });
}

int run_plan(std::string goal) {
    state->erase_ghost();
    state->process.stop();
    if (goal.starts_with("??")) {
        goal.erase(0, 2);
    }
    goal.erase(0, goal.find_first_not_of(' '));
    std::optional<proto::Plan> result;
    {
        const ShellSignals signals;
        result = proto::review_plan(state->config, goal, std::filesystem::current_path(),
                                    state->history.entries());
    }
    if (result) {
        state->history.add("?? " + goal);
        const auto script = "(\n" + proto::plan_script(*result) + ")";
        rl_replace_line(script.c_str(), 0);
        rl_point = rl_end;
        state->suggestions.clear();
        state->pending = false;
        return rl_newline(1, '\r');
    }
    state->resume_display();
    return 0;
}

int plan(int, int) {
    return guarded([] { return run_plan(rl_line_buffer != nullptr ? rl_line_buffer : ""); });
}

int submit(int count, int key) {
    return guarded([&] {
        const std::string command = rl_line_buffer != nullptr ? rl_line_buffer : "";
        if (command.starts_with("??")) {
            return run_plan(command);
        }
        state->erase_ghost();
        state->process.stop();
        if (proto::assess_command(command).level == proto::RiskLevel::Danger) {
            bool approved = false;
            {
                const ShellSignals signals;
                approved = proto::confirm_command(command);
            }
            state->resume_display();
            if (!approved) {
                return 0;
            }
        }
        state->history.add(command);
        state->suggestions.clear();
        state->pending = false;
        return state->fallback(submit, count, key);
    });
}

} // namespace

extern "C" int proto_bash_active() {
    return state != nullptr;
}

extern "C" void proto_bash_disable() {
    if (!state) {
        return;
    }
    state->erase_ghost();
    if (rl_event_hook == idle) {
        rl_event_hook = state->old_idle;
    }
    if (rl_pre_input_hook == new_prompt) {
        rl_pre_input_hook = state->old_prompt;
    }
    if (rl_redisplay_function == redisplay) {
        rl_redisplay_function = state->old_redisplay;
    }
    for (auto it = state->bindings.rbegin(); it != state->bindings.rend(); ++it) {
        int type = 0;
        if (lookup(*it, type) == it->replacement && type == ISFUNC) {
            if (it->type == ISMACR) {
                const auto sequence = it->sequence == std::string(1, '\0') ? "\\C-@" : it->sequence;
                rl_generic_bind(ISMACR, sequence.c_str(), const_cast<char *>(it->macro.c_str()),
                                it->map);
            } else {
                bind(*it, it->original);
            }
        }
    }
    const int timeout = rl_set_keyboard_input_timeout(state->old_timeout);
    if (timeout != 50000) {
        rl_set_keyboard_input_timeout(timeout);
    }
    state.reset();
}

extern "C" int proto_bash_enable(const char *binary) {
    if (state) {
        return 0;
    }
    try {
        if (binary == nullptr || !std::filesystem::path(binary).is_absolute() ||
            ::access(binary, X_OK) != 0 || !::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
            throw std::runtime_error(
                "Bash integration requires a terminal and an absolute proto executable path");
        }
        rl_initialize();
        state = std::make_unique<State>(binary);
        const std::pair<std::string, rl_command_func_t *> keys[]{{"\t", accept},
                                                                 {"\x1b[C", accept},
                                                                 {"\x1bOC", accept},
                                                                 {"\x1b[1;5C", accept_word},
                                                                 {"\x1b[1;3C", accept_word},
                                                                 {"\x1b[1;3A", previous},
                                                                 {"\x1b,", previous},
                                                                 {"\x1b[1;3B", next},
                                                                 {"\x1b.", next},
                                                                 {"\x1bOQ", menu},
                                                                 {"\x1b[12~", menu},
                                                                 {std::string(1, '\0'), menu},
                                                                 {"\x07", plan},
                                                                 {"\r", submit},
                                                                 {"\n", submit},
                                                                 {"\x1b\x1b", dismiss},
                                                                 {"\x1d", dismiss},
                                                                 {"\x18\x01", request}};
        for (const char *name : {"emacs-standard", "vi-insertion"}) {
            const auto map = rl_get_keymap_by_name(name);
            if (map == nullptr) {
                continue;
            }
            for (const auto &[sequence, replacement] : keys) {
                Binding binding{map, sequence, ISFUNC, nullptr, {}, replacement};
                binding.original = lookup(binding, binding.type);
                if (binding.type == ISKMAP) {
                    continue;
                }
                if (binding.type == ISMACR && binding.original != nullptr) {
                    binding.macro = reinterpret_cast<const char *>(binding.original);
                }
                state->bindings.push_back(std::move(binding));
                bind(state->bindings.back(), replacement);
            }
        }
        state->old_timeout = rl_set_keyboard_input_timeout(50000);
        rl_event_hook = idle;
        rl_pre_input_hook = new_prompt;
        rl_redisplay_function = redisplay;
        return 0;
    } catch (const std::exception &error) {
        proto_bash_disable();
        std::fprintf(stderr, "proto: %s\n", error.what());
        return 1;
    }
}
