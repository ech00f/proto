#include "proto/editor.hpp"

#include "proto/context.hpp"
#include "proto/safety.hpp"
#include "proto/signals.hpp"
#include "proto/suggestions.hpp"
#include "proto/text.hpp"
#include "proto/tui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace proto {
namespace {

void write_terminal(std::string_view text) {
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(STDOUT_FILENO, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Could not write to terminal");
        }
        written += static_cast<std::size_t>(count);
    }
}

class Terminal {
  public:
    Terminal() {
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
            throw std::runtime_error("Interactive mode requires a TTY; use --complete in scripts");
        }
        if (::tcgetattr(STDIN_FILENO, &original_) != 0) {
            throw std::runtime_error("Could not read terminal settings");
        }
        auto raw = original_;
        ::cfmakeraw(&raw);
        raw.c_oflag |= OPOST;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            throw std::runtime_error("Could not enable terminal editing");
        }
        try {
            write_terminal("\x1b[?2004h");
        } catch (...) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &original_);
            throw;
        }
    }
    ~Terminal() {
        constexpr std::string_view reset = "\x1b[0m\x1b[?2004l";
        (void)::write(STDOUT_FILENO, reset.data(), reset.size());
        ::tcsetattr(STDIN_FILENO, TCSANOW, &original_);
    }
    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;

  private:
    struct termios original_{};
};

int terminal_columns() {
    struct winsize size{};
    return ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col != 0 ? size.ws_col : 80;
}

std::string render(const Frame &frame, RiskLevel risk) {
    const auto ghost_color = risk == RiskLevel::Danger    ? "\x1b[91m"
                             : risk == RiskLevel::Caution ? "\x1b[33m"
                                                          : "\x1b[90m";
    return "\r\x1b[2K\x1b[36m" + frame.prompt + "\x1b[0m" + frame.text + ghost_color + frame.ghost +
           "\x1b[0m\r\x1b[" + std::to_string(frame.cursor_column + 1) + "G";
}

bool space_at(std::string_view text, std::size_t position) {
    return position < text.size() && std::isspace(static_cast<unsigned char>(text[position]));
}

} // namespace

std::vector<KeyEvent> InputDecoder::feed(std::string_view bytes) {
    std::vector<KeyEvent> events;
    for (const char value : bytes) {
        byte(static_cast<unsigned char>(value), events);
    }
    return events;
}

bool InputDecoder::pending_escape() const {
    return !escape_.empty();
}

std::vector<KeyEvent> InputDecoder::flush_escape() {
    std::vector<KeyEvent> events;
    if (!escape_.empty() && !pasting_) {
        events.push_back({Key::Escape, {}});
    }
    escape_.clear();
    return events;
}

void InputDecoder::escape(std::vector<KeyEvent> &events) {
    if (escape_ == "\x1b[200~") {
        pasting_ = true;
        events.push_back({Key::PasteBegin, {}});
    } else if (escape_ == "\x1b[201~") {
        pasting_ = false;
        events.push_back({Key::PasteEnd, {}});
    } else if (!pasting_) {
        const std::pair<std::string_view, Key> sequences[]{{"\x1b[A", Key::Up},
                                                           {"\x1b[1;3A", Key::PreviousSuggestion},
                                                           {"\x1b[1;3B", Key::NextSuggestion},
                                                           {"\x1b,", Key::PreviousSuggestion},
                                                           {"\x1b.", Key::NextSuggestion},
                                                           {"\x1bOQ", Key::Menu},
                                                           {"\x1b[12~", Key::Menu},
                                                           {"\x1b[B", Key::Down},
                                                           {"\x1b[C", Key::Right},
                                                           {"\x1b[D", Key::Left},
                                                           {"\x1b[H", Key::Home},
                                                           {"\x1b[F", Key::End},
                                                           {"\x1bOH", Key::Home},
                                                           {"\x1bOF", Key::End},
                                                           {"\x1b[1~", Key::Home},
                                                           {"\x1b[4~", Key::End},
                                                           {"\x1b[7~", Key::Home},
                                                           {"\x1b[8~", Key::End},
                                                           {"\x1b[3~", Key::Delete},
                                                           {"\x1b[1;5C", Key::WordRight},
                                                           {"\x1b[1;3C", Key::WordRight},
                                                           {"\x1b[1;5D", Key::WordLeft},
                                                           {"\x1b[1;3D", Key::WordLeft},
                                                           {"\x1b"
                                                            "f",
                                                            Key::WordRight},
                                                           {"\x1b"
                                                            "b",
                                                            Key::WordLeft}};
        for (const auto &[sequence, key] : sequences) {
            if (escape_ == sequence) {
                events.push_back({key, {}});
                break;
            }
        }
    }
    escape_.clear();
}

void InputDecoder::byte(unsigned char value, std::vector<KeyEvent> &events) {
    if (!escape_.empty()) {
        escape_ += static_cast<char>(value);
        if (escape_.size() > 32) {
            escape_.clear();
        } else if ((escape_.size() == 2 && value != '[' && value != 'O') ||
                   (escape_.size() >= 3 && value >= 0x40 && value <= 0x7e)) {
            escape(events);
        }
        return;
    }
    if (value == 0x1b) {
        escape_ = '\x1b';
        utf8_.clear();
        return;
    }
    if (!utf8_.empty()) {
        if ((value & 0xc0U) == 0x80U) {
            utf8_ += static_cast<char>(value);
            if (utf8_.size() == utf8_size_) {
                if (printable_utf8(utf8_)) {
                    events.push_back({Key::Text, std::move(utf8_)});
                }
                utf8_.clear();
            }
            return;
        }
        utf8_.clear();
    }
    if (value >= 0xc2 && value <= 0xf4) {
        utf8_size_ = value < 0xe0 ? 2 : value < 0xf0 ? 3 : 4;
        utf8_ = static_cast<char>(value);
        return;
    }
    if (pasting_) {
        if (value == '\n' || value == '\r' || value == '\t') {
            events.push_back({Key::Text, " "});
        } else if (value >= 0x20 && value < 0x7f) {
            events.push_back({Key::Text, std::string(1, static_cast<char>(value))});
        }
        return;
    }
    const std::pair<unsigned char, Key> controls[]{
        {0, Key::Menu},       {1, Key::Home},   {2, Key::Left},       {3, Key::Cancel},
        {4, Key::Eof},        {5, Key::End},    {6, Key::Right},      {8, Key::Backspace},
        {9, Key::Accept},     {10, Key::Enter}, {11, Key::KillEnd},   {12, Key::Redraw},
        {13, Key::Enter},     {7, Key::Plan},   {21, Key::KillStart}, {23, Key::KillWord},
        {127, Key::Backspace}};
    for (const auto &[control, key] : controls) {
        if (value == control) {
            events.push_back({key, {}});
            return;
        }
    }
    if (value >= 0x20 && value < 0x7f) {
        events.push_back({Key::Text, std::string(1, static_cast<char>(value))});
    }
}

const std::string &LineBuffer::text() const {
    return text_;
}

std::size_t LineBuffer::cursor() const {
    return cursor_;
}

bool LineBuffer::at_end() const {
    return cursor_ == text_.size();
}

bool LineBuffer::insert(std::string_view text) {
    if (!printable_utf8(text) || text.size() > max_command_bytes - text_.size()) {
        return false;
    }
    text_.insert(cursor_, text);
    cursor_ += text.size();
    return true;
}

void LineBuffer::replace(std::string text) {
    if (text.size() <= max_command_bytes && printable_utf8(text)) {
        text_ = std::move(text);
        cursor_ = text_.size();
    }
}

void LineBuffer::left() {
    cursor_ = previous_character(text_, cursor_);
}

void LineBuffer::right() {
    cursor_ = next_character(text_, cursor_);
}

void LineBuffer::home() {
    cursor_ = 0;
}

void LineBuffer::end() {
    cursor_ = text_.size();
}

void LineBuffer::backspace() {
    const auto previous = previous_character(text_, cursor_);
    text_.erase(previous, cursor_ - previous);
    cursor_ = previous;
}

void LineBuffer::erase_next() {
    text_.erase(cursor_, next_character(text_, cursor_) - cursor_);
}

void LineBuffer::kill_start() {
    text_.erase(0, cursor_);
    cursor_ = 0;
}

void LineBuffer::kill_end() {
    text_.erase(cursor_);
}

void LineBuffer::word_left() {
    while (cursor_ > 0 && space_at(text_, previous_character(text_, cursor_))) {
        left();
    }
    while (cursor_ > 0 && !space_at(text_, previous_character(text_, cursor_))) {
        left();
    }
}

void LineBuffer::word_right() {
    while (cursor_ < text_.size() && !space_at(text_, cursor_)) {
        right();
    }
    while (cursor_ < text_.size() && space_at(text_, cursor_)) {
        right();
    }
}

void LineBuffer::kill_word() {
    const auto end = cursor_;
    word_left();
    text_.erase(cursor_, end - cursor_);
}

bool LineBuffer::accept(std::string_view command, bool word_only) {
    if (!at_end() || !command.starts_with(text_) || command.size() <= text_.size() ||
        !printable_utf8(command)) {
        return false;
    }
    auto suffix = command.substr(text_.size());
    if (word_only) {
        std::size_t end = 0;
        while (end < suffix.size() && space_at(suffix, end)) {
            end = next_character(suffix, end);
        }
        while (end < suffix.size() && !space_at(suffix, end)) {
            end = next_character(suffix, end);
        }
        if (end < suffix.size()) {
            end = next_character(suffix, end);
        }
        suffix = suffix.substr(0, end);
    }
    return insert(suffix);
}

Frame make_frame(std::string_view prompt, const LineBuffer &line, std::string_view completion,
                 int columns) {
    const int usable = std::max(0, columns - 1);
    Frame frame;
    frame.prompt = clip_columns(prompt, std::max(0, usable - 4));
    const int prompt_width = display_width(frame.prompt);
    const int available = usable - prompt_width;
    std::size_t start = 0;
    while (start < line.cursor() &&
           display_width(std::string_view(line.text()).substr(start, line.cursor() - start)) >
               std::max(0, available - 2)) {
        start = next_character(line.text(), start);
    }
    const int marker = start > 0 && available >= 2 ? 1 : 0;
    const auto visible = std::string_view(line.text()).substr(start);
    frame.text = (marker ? "<" : "") + clip_columns(visible, available - marker);
    frame.cursor_column =
        prompt_width + marker +
        display_width(std::string_view(line.text()).substr(start, line.cursor() - start));
    if (line.at_end() && completion.starts_with(line.text()) &&
        completion.size() > line.text().size()) {
        const auto suffix = completion.substr(line.text().size());
        const int ghost_space = available - display_width(frame.text);
        if (display_width(suffix) <= ghost_space) {
            frame.ghost = std::string(suffix);
        } else if (ghost_space > 0) {
            frame.ghost = clip_columns(suffix, ghost_space - 1) + "…";
        }
    }
    return frame;
}

Editor::Editor(Config config)
    : config_(std::move(config)), completions_(std::make_unique<CompletionService>(config_)) {}

const std::string &Editor::warning() const {
    return warning_;
}

const Config &Editor::config() const {
    return config_;
}

ReadResult Editor::read_line(const std::filesystem::path &cwd,
                             const std::vector<std::string> &history) {
    const Terminal terminal;
    LineBuffer line;
    InputDecoder decoder;
    Suggestions suggestions;
    std::uint64_t request = 0;
    bool suppressed = false;
    bool pasting = false;
    std::size_t history_index = history.size();
    std::string draft;
    std::string last_frame;
    auto escape_time = std::chrono::steady_clock::now();

    const auto prompt = [&] {
        const auto path = display_path(cwd, config_.home);
        const auto short_path = display_width(path) > 28 ? clip_columns(path, 27) + "…" : path;
        std::string status;
        if (suggestions.size() > 1 && !suppressed && line.at_end()) {
            status += " [" + std::to_string(suggestions.index() + 1) + "/" +
                      std::to_string(suggestions.size()) + "]";
        }
        const auto risk =
            assess_command(suggestions.selected().empty() || suppressed || !line.at_end()
                               ? std::string_view(line.text())
                               : suggestions.selected());
        if (risk.level != RiskLevel::None) {
            status += " [" + std::string(risk_name(risk.level)) + "]";
        }
        return std::string("proto") + (offline_ ? " [local] " : " ") + short_path + status + " $ ";
    };
    const auto update = [&] {
        if (pasting || suppressed || !line.at_end() || line.text().empty() ||
            line.text().starts_with("??")) {
            completions_->cancel();
            return;
        }
        suggestions.filter(line.text());
        if (!suggestions.selected().empty()) {
            completions_->cancel();
            return;
        }
        request = completions_->submit(line.text(), cwd, history);
    };
    const auto finish = [&](ReadState state, int signal = 0) {
        completions_->cancel();
        if (state == ReadState::Submitted || state == ReadState::Plan) {
            write_terminal("\r\x1b[2K\x1b[36m" + prompt() + "\x1b[0m" + line.text() + "\n");
        } else {
            write_terminal(state == ReadState::Cancelled ? "^C\n" : "\n");
        }
        return ReadResult{state,
                          state == ReadState::Submitted || state == ReadState::Plan ? line.text()
                                                                                    : std::string{},
                          signal};
    };

    for (;;) {
        const int signal = take_signal();
        if (signal != 0) {
            return finish(signal == SIGINT ? ReadState::Cancelled : ReadState::Terminated, signal);
        }
        while (auto result = completions_->take_result()) {
            if (result->generation == request && result->prefix == line.text() && !suppressed &&
                !pasting) {
                if (result->ai_response) {
                    offline_ = false;
                    warning_.clear();
                }
                if (!result->commands.empty()) {
                    suggestions.assign(std::move(result->commands));
                }
                if (!result->warning.empty()) {
                    warning_ = std::move(result->warning);
                    offline_ = true;
                }
            }
        }
        const auto selected = suppressed ? std::string_view{} : suggestions.selected();
        const auto frame = render(
            make_frame(prompt(), line, selected, terminal_columns()),
            assess_command(selected.empty() ? std::string_view(line.text()) : selected).level);
        if (frame != last_frame) {
            write_terminal(frame);
            last_frame = frame;
        }
        struct pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, 25);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Could not poll terminal input");
        }
        std::vector<KeyEvent> events;
        if (ready > 0 && (input.revents & POLLIN) != 0) {
            std::array<char, 512> bytes{};
            const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return finish(ReadState::Eof);
            }
            events = decoder.feed(std::string_view(bytes.data(), static_cast<std::size_t>(count)));
            escape_time = std::chrono::steady_clock::now();
        } else if (ready > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return finish(ReadState::Eof);
        } else if (decoder.pending_escape() && std::chrono::steady_clock::now() - escape_time >=
                                                   std::chrono::milliseconds{75}) {
            events = decoder.flush_escape();
        }
        for (const auto &event : events) {
            const auto old_text = line.text();
            const auto old_cursor = line.cursor();
            switch (event.key) {
            case Key::Text:
                if (!line.insert(event.text)) {
                    write_terminal("\a");
                }
                suppressed = false;
                break;
            case Key::Enter:
                if (assess_command(line.text()).level == RiskLevel::Danger) {
                    if (!confirm_command(line.text())) {
                        write_terminal("\x1b[?2004h");
                        return finish(ReadState::Cancelled);
                    }
                    write_terminal("\x1b[?2004h");
                }
                return finish(ReadState::Submitted);
            case Key::Cancel:
                return finish(ReadState::Cancelled, SIGINT);
            case Key::Eof:
                if (line.text().empty()) {
                    return finish(ReadState::Eof);
                }
                line.erase_next();
                break;
            case Key::Backspace:
                line.backspace();
                suppressed = false;
                break;
            case Key::Delete:
                line.erase_next();
                suppressed = false;
                break;
            case Key::Left:
                line.left();
                break;
            case Key::Right:
            case Key::Accept:
                if (line.accept(suggestions.selected())) {
                    suggestions.clear();
                    suppressed = true;
                } else if (event.key == Key::Right) {
                    line.right();
                }
                break;
            case Key::WordRight:
                if (!line.accept(suggestions.selected(), true)) {
                    line.word_right();
                }
                break;
            case Key::WordLeft:
                line.word_left();
                break;
            case Key::Home:
                line.home();
                break;
            case Key::End:
                line.end();
                break;
            case Key::KillStart:
                line.kill_start();
                suppressed = false;
                break;
            case Key::KillEnd:
                line.kill_end();
                suppressed = false;
                break;
            case Key::KillWord:
                line.kill_word();
                suppressed = false;
                break;
            case Key::Up:
                if (history_index > 0) {
                    if (history_index == history.size()) {
                        draft = line.text();
                    }
                    line.replace(history[--history_index]);
                    suppressed = false;
                }
                break;
            case Key::Down:
                if (history_index < history.size()) {
                    ++history_index;
                    line.replace(history_index == history.size() ? draft : history[history_index]);
                    suppressed = false;
                }
                break;
            case Key::Escape:
                suggestions.clear();
                suppressed = true;
                completions_->cancel();
                break;
            case Key::Redraw:
                write_terminal("\x1b[2J\x1b[H");
                last_frame.clear();
                break;
            case Key::PasteBegin:
                pasting = true;
                suggestions.clear();
                completions_->cancel();
                break;
            case Key::PasteEnd:
                pasting = false;
                suppressed = false;
                update();
                break;
            case Key::NextSuggestion:
            case Key::PreviousSuggestion:
                if (line.at_end() && !suppressed) {
                    suggestions.cycle(event.key == Key::NextSuggestion ? 1 : -1);
                }
                break;
            case Key::Menu:
                completions_->cancel();
                if (const auto recalled = show_menu(config_, cwd, history)) {
                    line.replace(*recalled);
                }
                completions_ = std::make_unique<CompletionService>(config_);
                suggestions.clear();
                suppressed = false;
                write_terminal("\x1b[?2004h");
                last_frame.clear();
                update();
                break;
            case Key::Plan:
                if (!line.text().empty()) {
                    return finish(ReadState::Plan);
                }
                break;
            }
            if (line.text() != old_text || line.cursor() != old_cursor) {
                if (event.key != Key::Up && event.key != Key::Down) {
                    history_index = history.size();
                }
                suggestions.filter(line.text());
                update();
            }
        }
    }
}

} // namespace proto
