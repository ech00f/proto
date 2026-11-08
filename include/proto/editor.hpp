#pragma once

#include "proto/completion_service.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

enum class Key {
    Text,
    Enter,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Accept,
    WordRight,
    WordLeft,
    KillStart,
    KillEnd,
    KillWord,
    Cancel,
    Eof,
    Redraw,
    Escape,
    NextSuggestion,
    PreviousSuggestion,
    Menu,
    Plan,
    PasteBegin,
    PasteEnd
};

struct KeyEvent {
    Key key;
    std::string text;
};

class InputDecoder {
  public:
    std::vector<KeyEvent> feed(std::string_view bytes);
    std::vector<KeyEvent> flush_escape();
    bool pending_escape() const;

  private:
    void byte(unsigned char value, std::vector<KeyEvent> &events);
    void escape(std::vector<KeyEvent> &events);

    std::string escape_;
    std::string utf8_;
    std::size_t utf8_size_ = 0;
    bool pasting_ = false;
};

class LineBuffer {
  public:
    const std::string &text() const;
    std::size_t cursor() const;
    bool at_end() const;
    bool insert(std::string_view text);
    void replace(std::string text);
    void left();
    void right();
    void home();
    void end();
    void backspace();
    void erase_next();
    void kill_start();
    void kill_end();
    void word_left();
    void word_right();
    void kill_word();
    bool accept(std::string_view command, bool word_only = false);

  private:
    std::string text_;
    std::size_t cursor_ = 0;
};

struct Frame {
    std::string prompt;
    std::string text;
    std::string ghost;
    int cursor_column = 0;
};

Frame make_frame(std::string_view prompt, const LineBuffer &line, std::string_view completion,
                 int columns);

enum class ReadState { Submitted, Cancelled, Eof, Terminated, Plan };
struct ReadResult {
    ReadState state;
    std::string command;
    int signal = 0;
};

class Editor {
  public:
    explicit Editor(Config config);
    ReadResult read_line(const std::filesystem::path &cwd, const std::vector<std::string> &history);
    const std::string &warning() const;
    const Config &config() const;

  private:
    Config config_;
    std::unique_ptr<CompletionService> completions_;
    std::string warning_;
    bool offline_ = false;
};

} // namespace proto
