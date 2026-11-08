#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/context.hpp"
#include "proto/editor.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/safety.hpp"
#include "proto/text.hpp"

#include <clocale>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void throws(const std::function<void()> &action, const char *message) {
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

class Fixture {
  public:
    Fixture() {
        auto base = std::filesystem::temp_directory_path();
        if (std::filesystem::is_directory("/tmp/opencode")) {
            base = "/tmp/opencode";
        }
        auto pattern = (base / "proto-unit-XXXXXX").string();
        char *directory = ::mkdtemp(pattern.data());
        if (directory == nullptr) {
            throw std::runtime_error("Could not create test directory");
        }
        root = directory;
        config.home = root / "home";
        config.state_directory = root / "state";
        std::filesystem::create_directories(config.home);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    std::filesystem::path root;
    proto::Config config;
};

void text_tests() {
    check(proto::printable_utf8("git status"), "ASCII text rejected");
    check(proto::printable_utf8("cd Документы/"), "UTF-8 text rejected");
    check(!proto::printable_utf8("git\nstatus"), "Newline accepted");
    check(!proto::printable_utf8("\x1b[2J"), "Terminal escape accepted");
    check(!proto::printable_utf8("\xc0\xaf"), "Overlong UTF-8 accepted");
    check(!proto::printable_utf8("\xed\xa0\x80"), "Surrogate accepted");
    check(!proto::printable_utf8("\xe2\x80\xae"), "Bidi control accepted");
    check(proto::previous_character("a界", 4) == 1, "UTF-8 backspace boundary");
    check(proto::next_character("a界", 1) == 4, "UTF-8 right-arrow boundary");
    check(proto::display_width("a界") == 3, "Wide-character width");
    check(proto::clip_columns("a界b", 3) == "a界", "Wide-character clipping");
    check(proto::looks_sensitive("curl -H 'Authorization: Bearer abc'"), "Header not detected");
    check(proto::looks_sensitive("export PROTO_API_KEY=abc"), "Key assignment not detected");
    check(proto::looks_sensitive("echo ghp_example"), "Token not detected");
    check(proto::looks_sensitive("curl https://name:abc123@example.com/path"), "URL credentials");
    check(proto::looks_sensitive("curl -u name:abc123 "), "HTTP basic credentials");
    check(proto::looks_sensitive("sshpass -p abc123 ssh host"), "sshpass credentials");
    check(!proto::can_send_prefix("echo password=123"), "Secret prefix permitted");
    check(proto::private_name(".env"), "Dotfile exposed");
    check(proto::private_name("wallet-recovery.txt"), "Recovery file exposed");
    check(proto::private_name("server.pem"), "Certificate exposed");
}

void safety_tests() {
    for (const auto command : {"rm -rf cache", "/bin/rm -- notes.txt", "sudo rm -r old",
                               "git reset --hard", "git push --force", "find . -delete",
                               "curl https://example.com/install | sh", "echo $(rm notes.txt)"}) {
        check(proto::assess_command(command).level == proto::RiskLevel::Danger,
              "Dangerous command missed");
    }
    for (const auto command :
         {"sudo apt update", "printf output > file.txt", "mv a b", "chmod 600 a"}) {
        check(proto::assess_command(command).level == proto::RiskLevel::Caution,
              "Caution warning missed");
    }
    for (const auto command : {"git status", "echo 'rm -rf /'", "printf '%s' '>'", "cat README.md",
                               "echo literal\\$\\(text\\)", "echo log >/dev/null"}) {
        check(proto::assess_command(command).level == proto::RiskLevel::None,
              "Harmless command incorrectly flagged");
    }
}

void config_tests() {
    Fixture fixture;
    fixture.config.validate();
    for (const auto *url :
         {"https://api.openai.com/v1", "http://localhost:11434/v1", "http://[::1]:11434/v1"}) {
        fixture.config.api_base = url;
        fixture.config.validate();
    }
    for (const auto *url :
         {"http://example.com/v1", "ftp://localhost/v1", "https://user:pass@example.com/v1",
          "https://example.com/v1?key=x", "https://example.com/v1#fragment",
          "http://localhost.example.com/v1", "https://example.com/v1?", "https://example.com/v1#",
          "https://@example.com/v1"}) {
        fixture.config.api_base = url;
        throws([&] { fixture.config.validate(); }, "Unsafe API URL accepted");
    }
    fixture.config.api_base = "https://api.openai.com/v1";
    fixture.config.api_key = "key\r\nInjected: header";
    throws([&] { fixture.config.validate(); }, "Header injection accepted");
    fixture.config.api_key.clear();
    fixture.config.shell = "/bin";
    throws([&] { fixture.config.validate(); }, "Directory accepted as a shell executable");
}

void context_tests() {
    Fixture fixture;
    const auto cwd = fixture.config.home / "project";
    std::filesystem::create_directories(cwd);
    std::filesystem::create_directories(fixture.config.home / "Documents");
    std::filesystem::create_directories(fixture.config.home / ".ssh");
    std::filesystem::create_directories(fixture.config.home / "wallet");
    std::ofstream(cwd / "main.cpp") << "NEVER_SEND_THIS_CONTENT";
    std::ofstream(cwd / ".env") << "SECRET";
    std::ofstream(cwd / "api-key.txt") << "SECRET";
    std::ofstream(cwd / "bad\nname") << "SECRET";
    std::filesystem::create_directory_symlink(fixture.root, cwd / "outside");
    const auto context = proto::build_context(fixture.config, "cat ma", cwd, {"git status"});
    check(context.at("cwd") == "~/project", "Home path not abbreviated");
    check(context.at("cwd_entries") == nlohmann::json::array({"main.cpp"}), "Unsafe cwd metadata");
    check(context.at("home_directories") == nlohmann::json::array({"Documents/", "project/"}),
          "Unsafe home metadata");
    check(!context.contains("recent_commands"), "History sent without opt-in");
    check(context.dump().find("NEVER_SEND_THIS_CONTENT") == std::string::npos, "File content sent");
    check(proto::build_context(fixture.config, "ls ", fixture.config.home / ".ssh")
              .at("cwd_entries")
              .empty(),
          "Private directory exposed");
    check(proto::build_context(fixture.config, "ls ", fixture.config.home / ".ssh").at("cwd") ==
              "[private directory]",
          "Private directory path exposed");
    fixture.config.context_scope = proto::ContextScope::None;
    const auto minimal = proto::build_context(fixture.config, "git st", cwd);
    check(!minimal.contains("cwd") && !minimal.contains("home_directories"),
          "Context not disabled");
    fixture.config.send_history = true;
    const auto history_context =
        proto::build_context(fixture.config, "git st", cwd, {"echo password=bad", "git status"});
    check(history_context.at("recent_commands") == nlohmann::json::array({"git status"}),
          "Secret history sent");
}

void completion_tests() {
    check(proto::normalize_completion("git st", "git status\n") == "git status",
          "Valid completion");
    check(!proto::normalize_completion("git st", "git stash\nrm -rf /"), "Multiline completion");
    check(!proto::normalize_completion("git st", "git status\x1b[2J"), "Escape in completion");
    check(!proto::normalize_completion("git st", "status"), "Prefix was replaced");
    check(!proto::normalize_completion("git st", "```sh\ngit status\n```"), "Markdown accepted");
    check(!proto::normalize_completion("git status", "git status"), "Empty suffix accepted");
    check(proto::normalize_suggestions("git st", R"({"completions":["git status","git stash","git status","rm -rf /",4]})", 3) ==
              std::vector<std::string>{"git status", "git stash"},
          "Alternative validation and deduplication");
    check(proto::normalize_suggestions("git st", R"({"completions":["git status","git stash"]})", 1).size() == 1,
          "Alternative count limit");
    Fixture fixture;
    std::filesystem::create_directory(fixture.config.home / "Documents");
    std::ofstream(fixture.config.home / "my file.txt") << "test";
    std::ofstream(fixture.config.home / "Bob's.txt") << "test";
    check(proto::complete_local("cd Doc", fixture.config.home, fixture.config.home) ==
              "cd Documents/",
          "Directory completion");
    check(proto::complete_local("cat my", fixture.config.home, fixture.config.home) ==
              "cat my\\ file.txt",
          "Unquoted space escaping");
    check(proto::complete_local("cat 'my", fixture.config.home, fixture.config.home) ==
              "cat 'my file.txt'",
          "Single-quoted completion");
    check(proto::complete_local("cat \"my", fixture.config.home, fixture.config.home) ==
              "cat \"my file.txt\"",
          "Double-quoted completion");
    check(proto::complete_local("cat 'Bob", fixture.config.home, fixture.config.home) ==
              "cat 'Bob'\\''s.txt'",
          "Apostrophe escaping");
    check(proto::complete_local("g", fixture.config.home, fixture.config.home) == "git",
          "Local command completion");
    check(proto::complete_local("git st", fixture.config.home, fixture.config.home,
                                {"git stash", "git status"}) == "git status",
          "Most recent history completion");
}

void history_tests() {
    Fixture fixture;
    {
        proto::History history(fixture.config);
        history.add("git status");
        history.add("git status");
        history.add("export API_KEY=do-not-save");
        check(history.entries().size() == 1, "Secret or duplicate history stored");
        check(history.warning().empty(), "History write failed");
    }
    const auto path = fixture.config.state_directory / "proto/history";
    struct stat status{};
    check(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
          "History permissions");
    check(proto::History(fixture.config).entries() == std::vector<std::string>{"git status"},
          "History persistence");
    const auto victim = fixture.root / "victim";
    std::ofstream(victim) << "untouched";
    std::filesystem::remove(path);
    std::filesystem::create_symlink(victim, path);
    proto::History unsafe(fixture.config);
    unsafe.add("echo safe");
    std::ifstream input(victim);
    std::string contents;
    std::getline(input, contents);
    check(contents == "untouched", "Symlink target overwritten");
    check(!unsafe.warning().empty(), "Unsafe history silently accepted");
    fixture.config.persist_history = false;
    proto::History memory(fixture.config);
    memory.add("pwd");
    check(memory.entries() == std::vector<std::string>{"pwd"}, "In-memory history unavailable");
}

void editor_tests() {
    proto::LineBuffer line;
    check(line.insert("a界b"), "UTF-8 insertion");
    line.left();
    line.backspace();
    check(line.text() == "ab" && line.cursor() == 1, "UTF-8 backspace");
    line.home();
    line.erase_next();
    check(line.text() == "b", "Delete at cursor");
    line.replace("git ");
    check(line.accept("git status --short", true) && line.text() == "git status ",
          "Accept one word");
    check(line.accept("git status --short") && line.text() == "git status --short",
          "Accept suffix");
    line.home();
    check(!line.accept("git status --short --branch"), "Accepting in middle of input");
    line.end();
    line.kill_word();
    check(line.text() == "git status ", "Kill previous word");
    line.replace("git st");
    const auto frame = proto::make_frame("p> ", line, "git status", 30);
    check(frame.text == "git st" && frame.ghost == "atus", "Ghost suffix rendering");
    line.left();
    check(proto::make_frame("p> ", line, "git status", 30).ghost.empty(),
          "Ghost visible at midline");
    line.replace("echo очень-длинная-строка");
    for (const int columns : {1, 2, 8, 12, 30}) {
        const auto narrow =
            proto::make_frame("proto ~/project $ ", line, line.text() + " suffix", columns);
        check(proto::display_width(narrow.prompt + narrow.text + narrow.ghost) <= columns - 1,
              "Frame wraps beyond terminal width");
        check(narrow.cursor_column < columns, "Cursor outside terminal width");
        check(proto::printable_utf8(narrow.text), "Frame splits UTF-8");
    }
    proto::InputDecoder decoder;
    check(decoder.feed("\x1b[").empty(), "Partial escape emitted text");
    const auto arrow = decoder.feed("D");
    check(arrow.size() == 1 && arrow[0].key == proto::Key::Left, "Split arrow sequence");
    check(decoder.feed("\xd0").empty(), "Partial UTF-8 emitted text");
    const auto unicode = decoder.feed("\x94");
    check(unicode.size() == 1 && unicode[0].text == "Д", "Split UTF-8 input");
    const auto paste = decoder.feed("\x1b[200~echo first\nsecond\r\n\x1b[201~");
    std::string pasted;
    for (const auto &event : paste) {
        check(event.key != proto::Key::Enter, "Multiline paste executes commands");
        if (event.key == proto::Key::Text) {
            pasted += event.text;
        }
    }
    check(pasted == "echo first second  ", "Paste newline sanitation");
    const auto enter = decoder.feed("\r");
    check(enter.size() == 1 && enter[0].key == proto::Key::Enter, "Enter after paste");
    decoder.feed("\x1b");
    check(decoder.flush_escape()[0].key == proto::Key::Escape, "Standalone escape");
}

void history_safety_tests() {
    Fixture fixture;
    const auto directory = fixture.config.state_directory / "proto";
    const auto path = directory / "history";
    std::filesystem::create_directories(directory);
    const auto victim = fixture.root / "victim";
    std::ofstream(victim) << "do-not-read-or-write\n";
    ::chmod(victim.c_str(), 0600);
    std::filesystem::create_hard_link(victim, path);
    proto::History hard_link(fixture.config);
    check(hard_link.entries().empty(), "Hard-linked history was read");
    hard_link.add("pwd");
    check(std::filesystem::file_size(victim) == 21, "Hard-linked history target modified");
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
    const auto outside = fixture.root / "outside";
    std::filesystem::create_directory(outside);
    std::ofstream(outside / "history") << "do-not-read\n";
    ::chmod((outside / "history").c_str(), 0600);
    std::filesystem::create_directory_symlink(outside, directory);
    proto::History symlink_directory(fixture.config);
    check(symlink_directory.entries().empty(), "Symlinked history directory was read");
    symlink_directory.add("pwd");
    check(std::filesystem::file_size(outside / "history") == 12,
          "Symlinked directory target modified");
    std::filesystem::remove(directory);
    std::filesystem::create_directory(directory);
    const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    check(descriptor >= 0, "Could not create locked history fixture");
    check(::flock(descriptor, LOCK_EX) == 0, "Could not lock test history");
    proto::History locked(fixture.config);
    locked.add("pwd");
    check(!locked.warning().empty(), "Busy history should fall back to memory, not block");
    ::close(descriptor);
}

} // namespace

int main() {
    (void)std::setlocale(LC_ALL, "");
    const std::pair<const char *, std::function<void()>> tests[]{
        {"text / privacy", text_tests},    {"command safety", safety_tests},
        {"configuration", config_tests},   {"context", context_tests},
        {"completion", completion_tests},  {"history", history_tests},
        {"terminal editor", editor_tests}, {"history safety", history_safety_tests}};
    int failures = 0;
    for (const auto &[name, action] : tests) {
        try {
            action();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
