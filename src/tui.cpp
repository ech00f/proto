#include "proto/tui.hpp"

#include "proto/context.hpp"
#include "proto/editor.hpp"
#include "proto/signals.hpp"
#include "proto/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace proto {
namespace {

constexpr std::string_view background = "\x1b[48;2;12;16;28m";
constexpr std::string_view panel = "\x1b[48;2;19;25;41m";
constexpr std::string_view white = "\x1b[38;2;224;232;248m";
constexpr std::string_view muted = "\x1b[38;2;112;130;159m";
constexpr std::string_view green = "\x1b[38;2;98;228;184m";
constexpr std::string_view red = "\x1b[38;2;255;119;141m";
constexpr std::string_view amber = "\x1b[38;2;249;200;115m";

std::string repeated(std::string_view value, int count) {
    std::string result;
    for (int index = 0; index < count; ++index) {
        result += value;
    }
    return result;
}

void write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto count = ::write(fd, text.data(), text.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Cannot write to the TUI terminal");
        }
        text.remove_prefix(static_cast<std::size_t>(count));
    }
}

class Screen {
  public:
    Screen() {
        fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (fd_ < 0 || ::tcgetattr(fd_, &original_) != 0) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            throw std::runtime_error("TUI requires a controlling terminal");
        }
        auto raw = original_;
        ::cfmakeraw(&raw);
        raw.c_oflag |= OPOST;
        if (::tcsetattr(fd_, TCSANOW, &raw) != 0) {
            ::close(fd_);
            throw std::runtime_error("Cannot prepare TUI input");
        }
        try {
            write_all(fd_, "\x1b[?1049h\x1b[?25l\x1b[?2004h");
        } catch (...) {
            ::tcsetattr(fd_, TCSANOW, &original_);
            ::close(fd_);
            throw;
        }
    }
    ~Screen() {
        constexpr std::string_view restore = "\x1b[0m\x1b[?25h\x1b[?2004l\x1b[?1049l";
        (void)::write(fd_, restore.data(), restore.size());
        ::tcsetattr(fd_, TCSANOW, &original_);
        ::close(fd_);
    }
    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;
    std::pair<int, int> size() const {
        struct winsize size{};
        if (::ioctl(fd_, TIOCGWINSZ, &size) == 0 && size.ws_col != 0 && size.ws_row != 0) {
            return {size.ws_col, size.ws_row};
        }
        return {80, 24};
    }
    void draw(const std::string &frame) {
        if (frame != previous_) {
            write_all(fd_, frame);
            previous_ = frame;
        }
    }
    std::vector<KeyEvent> events() {
        const int signal = take_signal();
        if (signal != 0) {
            if (signal != SIGINT) {
                ::kill(::getpid(), signal);
            }
            return {{Key::Cancel, {}}};
        }
        struct pollfd input{fd_, POLLIN, 0};
        const int ready = ::poll(&input, 1, 40);
        if (ready < 0 && errno != EINTR) {
            throw std::runtime_error("Cannot read TUI input");
        }
        if (ready > 0 && (input.revents & POLLIN) != 0) {
            std::array<char, 512> bytes{};
            const auto count = ::read(fd_, bytes.data(), bytes.size());
            if (count <= 0) {
                return {{Key::Eof, {}}};
            }
            escape_time_ = std::chrono::steady_clock::now();
            return decoder_.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
        }
        if (ready > 0 && (input.revents & (POLLHUP | POLLERR)) != 0) {
            return {{Key::Eof, {}}};
        }
        if (decoder_.pending_escape() &&
            std::chrono::steady_clock::now() - escape_time_ > std::chrono::milliseconds{80}) {
            return decoder_.flush_escape();
        }
        return {};
    }

  private:
    int fd_ = -1;
    struct termios original_{};
    std::string previous_;
    InputDecoder decoder_;
    std::chrono::steady_clock::time_point escape_time_{};
};

class Canvas {
  public:
    Canvas(const Screen &screen, std::string_view title, std::string_view subtitle,
           std::string_view footer, std::string_view theme = "aurora") {
        const auto [columns, rows] = screen.size();
        width = std::max(1, std::min(110, columns - 4));
        height = std::max(1, std::min(34, rows - 2));
        left = std::max(1, (columns - width) / 2 + 1);
        top = std::max(1, (rows - height) / 2 + 1);
        accent = theme == "midnight" ? "\x1b[38;2;107;187;255m" : "\x1b[38;2;176;151;255m";
        buffer = std::string(background) + "\x1b[2J";
        compact = columns < 58 || rows < 14;
        if (compact) {
            text(1, 1, "proto · enlarge terminal (58×14) · Esc closes", white, columns - 1);
            return;
        }
        for (int row = 1; row < height - 1; ++row) {
            text(top + row, left, "│" + repeated(" ", width - 2) + "│", accent, width);
        }
        text(top, left, "╭" + repeated("─", width - 2) + "╮", accent, width);
        text(top + height - 1, left, "╰" + repeated("─", width - 2) + "╯", accent, width);
        text(top + 1, left + 3, "◈  P R O T O   /   " + std::string(title), white, width - 6);
        text(top + 2, left + 3, subtitle, muted, width - 6);
        text(top + 3, left + 1, repeated("─", width - 2), accent, width - 2);
        text(top + height - 3, left + 1, repeated("─", width - 2), accent, width - 2);
        text(top + height - 2, left + 3, footer, green, width - 6);
    }
    void text(int row, int column, std::string_view value, std::string_view color, int maximum) {
        if (maximum <= 0) {
            return;
        }
        const auto clipped = clip_columns(value, maximum);
        buffer += "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H" +
                  std::string(panel) + std::string(color) + clipped;
    }
    void body(int row, std::string_view value, std::string_view color = white) {
        if (row >= 0 && row < body_height()) {
            text(top + 5 + row, left + 3, value, color, width - 6);
        }
    }
    int body_height() const {
        return std::max(1, height - 9);
    }
    int left = 1;
    int top = 1;
    int width = 80;
    int height = 24;
    bool compact = false;
    std::string accent;
    std::string buffer;
};

bool close_key(Key key) {
    return key == Key::Escape || key == Key::Cancel || key == Key::Eof || key == Key::Menu;
}

void edit_line(LineBuffer &line, const KeyEvent &event) {
    switch (event.key) {
    case Key::Text:
        line.insert(event.text);
        break;
    case Key::Backspace:
        line.backspace();
        break;
    case Key::Delete:
        line.erase_next();
        break;
    case Key::KillStart:
        line.kill_start();
        break;
    case Key::KillEnd:
        line.kill_end();
        break;
    case Key::KillWord:
        line.kill_word();
        break;
    case Key::Left:
        line.left();
        break;
    case Key::Right:
        line.right();
        break;
    case Key::Home:
        line.home();
        break;
    case Key::End:
        line.end();
        break;
    default:
        break;
    }
}

std::vector<std::string> wrapped(std::string_view value, int columns) {
    std::vector<std::string> lines;
    while (!value.empty()) {
        const auto line = clip_columns(value, std::max(4, columns));
        if (line.empty()) {
            break;
        }
        lines.push_back(line);
        value.remove_prefix(line.size());
    }
    return lines;
}

struct DisplayLine {
    std::string text;
    std::string color;
};

bool approve(const std::vector<DisplayLine> &lines, std::string_view title,
             std::string_view subtitle, std::string_view theme) {
    Screen screen;
    LineBuffer confirmation;
    std::size_t offset = 0;
    bool bottom_seen = false;
    for (;;) {
        Canvas canvas(screen, title, subtitle,
                      "↑↓ scroll · type RUN + Enter to confirm · Esc cancel", theme);
        std::vector<DisplayLine> expanded;
        for (const auto &line : lines) {
            if (line.text.empty()) {
                expanded.push_back(line);
            } else {
                for (const auto &part : wrapped(line.text, canvas.width - 6)) {
                    expanded.push_back({part, line.color});
                }
            }
        }
        const auto available = static_cast<std::size_t>(std::max(1, canvas.body_height() - 2));
        const auto maximum = expanded.size() > available ? expanded.size() - available : 0;
        offset = std::min(offset, maximum);
        bottom_seen = bottom_seen || offset == maximum;
        if (!canvas.compact) {
            for (std::size_t row = 0; row < available && offset + row < expanded.size(); ++row) {
                canvas.body(static_cast<int>(row), expanded[offset + row].text,
                            expanded[offset + row].color);
            }
            canvas.body(canvas.body_height() - 1,
                        bottom_seen ? "CONFIRM  › " + confirmation.text()
                                    : "Scroll through all steps before confirming",
                        amber);
        }
        screen.draw(canvas.buffer);
        for (const auto &event : screen.events()) {
            if (close_key(event.key)) {
                return false;
            }
            if (event.key == Key::Down) {
                offset = std::min(maximum, offset + 1);
            } else if (event.key == Key::Up && offset > 0) {
                --offset;
            } else if (event.key == Key::End) {
                offset = maximum;
            } else if (event.key == Key::Home) {
                offset = 0;
            } else if (!canvas.compact && bottom_seen) {
                if (event.key == Key::Enter && confirmation.text() == "RUN") {
                    return true;
                }
                edit_line(confirmation, event);
            }
        }
    }
}

std::vector<std::pair<std::string, std::string>> settings(const Config &config) {
    return {{"AI", config.ai_enabled ? "enabled" : "disabled"},
            {"Endpoint", config.api_base},
            {"Model", config.model},
            {"Context", std::string(context_name(config.context_scope))},
            {"Alternatives", std::to_string(config.suggestion_count)},
            {"Debounce ms", std::to_string(config.debounce.count())},
            {"Timeout ms", std::to_string(config.timeout.count())},
            {"Entry limit", std::to_string(config.max_entries)},
            {"Disk history", config.persist_history ? "on" : "off"},
            {"Share history", config.send_history ? "on (opt-in)" : "off"},
            {"Theme", config.theme}};
}

void toggle(Config &config, std::size_t field, int direction) {
    switch (field) {
    case 0:
        config.ai_enabled = !config.ai_enabled;
        break;
    case 3:
        config.context_scope = static_cast<ContextScope>(
            (static_cast<int>(config.context_scope) + (direction < 0 ? 2 : 1)) % 3);
        break;
    case 4:
        config.suggestion_count = static_cast<std::size_t>(
            std::clamp(static_cast<int>(config.suggestion_count) + direction, 1, 5));
        break;
    case 8:
        config.persist_history = !config.persist_history;
        break;
    case 9:
        config.send_history = !config.send_history;
        break;
    case 10:
        config.theme = config.theme == "aurora" ? "midnight" : "aurora";
        break;
    default:
        break;
    }
}

void apply_edit(Config &config, std::size_t field, const std::string &value) {
    auto next = config;
    if (field == 1) {
        next.api_base = value;
        while (next.api_base.ends_with('/')) {
            next.api_base.pop_back();
        }
    } else if (field == 2) {
        next.model = value;
    } else {
        int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            throw std::runtime_error("Enter a number within the documented range");
        }
        if (field == 5) {
            next.debounce = std::chrono::milliseconds{number};
        } else if (field == 6) {
            next.timeout = std::chrono::milliseconds{number};
        } else if (field == 7) {
            next.max_entries = static_cast<std::size_t>(number);
        }
    }
    auto checked = next;
    checked.ai_enabled = true;
    checked.validate();
    config = std::move(next);
}

} // namespace

std::optional<std::string> show_menu(Config &config, const std::filesystem::path &cwd,
                                     const std::vector<std::string> &history) {
    Screen screen;
    Config working = config;
    constexpr std::array<std::string_view, 5> tabs{"Dashboard", "Settings", "History", "Context",
                                                   "Keys"};
    std::size_t page = 0;
    std::size_t selection = 0;
    bool editing = false;
    bool entering_task = false;
    bool searching = false;
    LineBuffer input;
    std::string query;
    std::string message = "Local-first · no file-content uploads · plans require approval";
    for (;;) {
        Canvas canvas(screen, tabs[page], display_path(cwd, working.home),
                      editing || entering_task || searching
                          ? "Enter apply · Esc cancel · Ctrl+U clear"
                          : "1–5 / Tab pages · ↑↓ select · Enter edit/recall · s save · Esc close",
                      working.theme);
        const int body_left = canvas.left + 20;
        const int body_width = canvas.width - 24;
        const auto values = settings(working);
        std::vector<std::string> matches;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->find(query) != it->npos) {
                matches.push_back(*it);
            }
        }
        if (page == 2 && selection >= matches.size()) {
            selection = matches.empty() ? 0 : matches.size() - 1;
        }
        const auto body = [&](int row, std::string_view text, std::string_view color = white) {
            if (row < canvas.body_height()) {
                canvas.text(canvas.top + 5 + row, body_left, text, color, body_width);
            }
        };
        if (!canvas.compact) {
            for (std::size_t index = 0; index < tabs.size(); ++index) {
                if (static_cast<int>(index) * 2 >= canvas.body_height()) {
                    break;
                }
                canvas.text(canvas.top + 5 + static_cast<int>(index) * 2, canvas.left + 3,
                            (index == page ? "▸ " : "  ") + std::string(tabs[index]),
                            index == page ? green : muted, 15);
            }
            if (entering_task) {
                body(0, "AI COMMAND PLAN", canvas.accent);
                body(2, "Describe a task. Review every step before running.", muted);
                body(4, "› " + input.text(), green);
            } else if (editing) {
                body(0, "EDIT  /  " + values[selection].first, canvas.accent);
                body(2, "› " + input.text(), green);
                body(4, message, muted);
            } else if (page == 0) {
                body(0, "YOUR TERMINAL, WITH A CO-PILOT", canvas.accent);
                body(2, "◉  " + working.model, green);
                body(3,
                     working.ai_enabled ? "AI enabled · asynchronous inline suggestions"
                                        : "Offline mode · paths and history only",
                     muted);
                body(5, "Context  " + std::string(context_name(working.context_scope)) +
                            "    Alternatives  " + std::to_string(working.suggestion_count));
                body(6, "History  " + std::to_string(history.size()) + " local commands");
                body(8, "p  New AI plan   /   ?? task at the prompt", green);
            } else if (page == 1) {
                const auto available =
                    static_cast<std::size_t>(std::max(1, canvas.body_height() - 1));
                const auto start = selection >= available ? selection - available + 1 : 0;
                for (std::size_t row = 0; row < available && start + row < values.size(); ++row) {
                    const auto index = start + row;
                    const auto label = values[index].first;
                    body(static_cast<int>(row),
                         (index == selection ? "▸ " : "  ") + label +
                             repeated(" ", std::max(1, 16 - display_width(label))) +
                             values[index].second,
                         index == selection ? green : white);
                }
            } else if (page == 2) {
                body(0,
                     searching ? "SEARCH › " + input.text()
                               : "/ search  ·  " + query + "  ·  Enter recalls (never runs)",
                     muted);
                const auto available =
                    static_cast<std::size_t>(std::max(1, canvas.body_height() - 3));
                const auto start = selection >= available ? selection - available + 1 : 0;
                for (std::size_t row = 0; row < available && start + row < matches.size(); ++row) {
                    const auto index = start + row;
                    body(static_cast<int>(row) + 2,
                         (index == selection ? "▸ " : "  ") + matches[index],
                         index == selection ? green : white);
                }
                if (matches.empty()) {
                    body(3, "No matching proto history", muted);
                }
            } else if (page == 3) {
                const auto context = build_context(working, "", cwd, history);
                const auto lines = wrapped(context.dump(), body_width);
                body(0, "OUTBOUND METADATA / no file contents", green);
                for (std::size_t row = selection;
                     row < lines.size() &&
                     row - selection < static_cast<std::size_t>(canvas.body_height() - 2);
                     ++row) {
                    body(static_cast<int>(row - selection) + 2, lines[row], muted);
                }
            } else {
                const std::array<std::string_view, 9> keys{
                    "F2 / Ctrl+Space    Control center",
                    "Tab / →            Accept suggestion",
                    "Alt+↑ / Alt+↓      Previous / next variant",
                    "Ctrl+→             Accept one word",
                    "Ctrl+G / ?? task    AI command plan",
                    "Enter              Execute typed input only",
                    "Esc                Dismiss suggestion",
                    "↑ / ↓              Browse proto history",
                    "Ctrl+C / Ctrl+D    Cancel / exit"};
                for (std::size_t row = 0;
                     row < keys.size() && row < static_cast<std::size_t>(canvas.body_height());
                     ++row) {
                    body(static_cast<int>(row), keys[row], row % 2 == 0 ? white : muted);
                }
            }
            canvas.text(canvas.top + canvas.height - 4, canvas.left + 3, message, muted,
                        canvas.width - 6);
        }
        screen.draw(canvas.buffer);
        for (const auto &event : screen.events()) {
            if (event.key == Key::Cancel || event.key == Key::Eof || event.key == Key::Menu) {
                return std::nullopt;
            }
            if (event.key == Key::Escape) {
                if (editing || entering_task || searching) {
                    editing = entering_task = searching = false;
                } else {
                    return std::nullopt;
                }
                continue;
            }
            if (canvas.compact) {
                continue;
            }
            if (editing || entering_task || searching) {
                if (event.key == Key::Enter) {
                    if (entering_task) {
                        if (!input.text().empty()) {
                            return "?? " + input.text();
                        }
                    } else if (searching) {
                        query = input.text();
                        selection = 0;
                        searching = false;
                    } else {
                        try {
                            apply_edit(working, selection, input.text());
                            editing = false;
                            message = "Changed locally. Press s to save and apply.";
                        } catch (const std::exception &error) {
                            message = error.what();
                        }
                    }
                } else {
                    edit_line(input, event);
                }
                continue;
            }
            if (event.key == Key::Text && event.text == "s") {
                try {
                    working.save();
                    config = working;
                    message = "Saved & applied. Environment overrides these values on restart.";
                } catch (const std::exception &error) {
                    message = error.what();
                }
            } else if (event.key == Key::Text && event.text == "p" && page == 0) {
                entering_task = true;
                input.replace("");
            } else if (event.key == Key::Text && event.text == "/" && page == 2) {
                searching = true;
                input.replace(query);
            } else if (event.key == Key::Text && event.text.size() == 1 && event.text[0] >= '1' &&
                       event.text[0] <= '5') {
                page = static_cast<std::size_t>(event.text[0] - '1');
                selection = 0;
            } else if (event.key == Key::Accept) {
                page = (page + 1) % tabs.size();
                selection = 0;
            } else if (event.key == Key::Up && selection > 0) {
                --selection;
            } else if (event.key == Key::Down) {
                const auto maximum = page == 1   ? values.size()
                                     : page == 2 ? matches.size()
                                     : page == 3 ? 1000
                                                 : 1;
                if (selection + 1 < maximum) {
                    ++selection;
                }
            } else if (page == 1 && (event.key == Key::Enter || event.key == Key::Left ||
                                     event.key == Key::Right)) {
                if (event.key == Key::Enter &&
                    (selection == 1 || selection == 2 || selection == 5 || selection == 6 ||
                     selection == 7)) {
                    editing = true;
                    input.replace(values[selection].second);
                } else {
                    toggle(working, selection, event.key == Key::Left ? -1 : 1);
                    message = "Changed locally. Press s to save and apply.";
                }
            } else if (page == 2 && event.key == Key::Enter && !matches.empty()) {
                return matches[selection];
            }
        }
    }
}

bool confirm_command(std::string_view command) {
    const auto risk = assess_command(command);
    if (risk.level != RiskLevel::Danger) {
        return true;
    }
    std::vector<DisplayLine> lines;
    const auto display =
        printable_utf8(command)
            ? std::string(command)
            : nlohmann::json(command).dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
    lines.push_back({display, std::string(white)});
    for (const auto &reason : risk.reasons) {
        lines.push_back({"! " + reason, std::string(red)});
    }
    return approve(lines, "DANGER / confirm command",
                   "Nothing has run. Review the exact command below.", "aurora");
}

std::optional<Plan> review_plan(const Config &config, std::string task,
                                const std::filesystem::path &cwd,
                                const std::vector<std::string> &history) {
    if (!can_send_prefix(task)) {
        throw std::runtime_error(
            "Task is empty, too long, non-printable, or contains recognizable credentials");
    }
    if (!config.ai_enabled) {
        throw std::runtime_error("Enable AI to generate command plans");
    }
    std::optional<Plan> result;
    std::string error;
    std::mutex mutex;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        try {
            auto plan = generate_plan(config, task, cwd, history, {&generation, 0});
            const std::lock_guard lock(mutex);
            result = std::move(plan);
        } catch (const RequestCancelled &) {
        } catch (const std::exception &failure) {
            const std::lock_guard lock(mutex);
            error = failure.what();
        }
        finished.store(true, std::memory_order_release);
    });
    bool cancelled = false;
    int columns = 80;
    try {
        Screen screen;
        std::size_t tick = 0;
        while (!finished.load(std::memory_order_acquire)) {
            Canvas canvas(screen, "AI PLAN",
                          "Preparing a reviewable sequence · nothing is executing",
                          "Esc / Ctrl+C cancels", config.theme);
            columns = screen.size().first;
            if (!canvas.compact) {
                constexpr std::array<std::string_view, 4> spinner{"◐", "◓", "◑", "◒"};
                canvas.body(1,
                            std::string(spinner[(tick++ / 4) % spinner.size()]) +
                                "  Planning your task…",
                            green);
                canvas.body(3, task, white);
                canvas.body(5, "Commands will wait for your explicit approval.", muted);
            }
            screen.draw(canvas.buffer);
            for (const auto &event : screen.events()) {
                if (close_key(event.key)) {
                    cancelled = true;
                    generation.fetch_add(1);
                }
            }
            if (cancelled) {
                break;
            }
        }
    } catch (...) {
        generation.fetch_add(1);
        worker.join();
        throw;
    }
    worker.join();
    if (cancelled) {
        return std::nullopt;
    }
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
    if (!result) {
        return std::nullopt;
    }
    std::vector<DisplayLine> lines;
    const int width = std::min(104, columns - 10);
    std::size_t index = 0;
    for (const auto &step : result->steps) {
        const auto color = step.risk.level == RiskLevel::Danger    ? red
                           : step.risk.level == RiskLevel::Caution ? amber
                                                                   : green;
        lines.push_back(
            {"[" + std::to_string(++index) + "] " + std::string(risk_name(step.risk.level)),
             std::string(color)});
        for (const auto &line : wrapped(step.command, width - 2)) {
            lines.push_back({"  " + line, std::string(white)});
        }
        for (const auto &line : wrapped(step.explanation, width - 2)) {
            lines.push_back({"  " + line, std::string(muted)});
        }
        for (const auto &reason : step.risk.reasons) {
            lines.push_back({"  ! " + reason, std::string(color)});
        }
        lines.push_back({"", std::string(muted)});
    }
    if (!approve(lines, result->title, "AI PLAN / one shell · shared state · stop on first failure",
                 config.theme)) {
        return std::nullopt;
    }
    return result;
}

} // namespace proto
