#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/context.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/text.hpp"

#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
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
        std::filesystem::remove_all(root, error); // Only this uniquely created test fixture.
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
    check(!proto::can_send_prefix("echo password=123"), "Secret prefix permitted");
    check(proto::private_name(".env"), "Dotfile exposed");
    check(proto::private_name("wallet-recovery.txt"), "Recovery file exposed");
    check(proto::private_name("server.pem"), "Certificate exposed");
}

void config_tests() {
    Fixture fixture;
    fixture.config.validate();
    for (const auto *url : {"https://api.openai.com/v1", "http://localhost:11434/v1",
                            "http://[::1]:11434/v1"}) {
        fixture.config.api_base = url;
        fixture.config.validate();
    }
    for (const auto *url : {"http://example.com/v1", "ftp://localhost/v1",
                            "https://user:pass@example.com/v1", "https://example.com/v1?key=x",
                            "https://example.com/v1#fragment", "http://localhost.example.com/v1"}) {
        fixture.config.api_base = url;
        throws([&] { fixture.config.validate(); }, "Unsafe API URL accepted");
    }
    fixture.config.api_base = "https://api.openai.com/v1";
    fixture.config.api_key = "key\r\nInjected: header";
    throws([&] { fixture.config.validate(); }, "Header injection accepted");
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
    fixture.config.context_scope = proto::ContextScope::None;
    const auto minimal = proto::build_context(fixture.config, "git st", cwd);
    check(!minimal.contains("cwd") && !minimal.contains("home_directories"), "Context not disabled");
    fixture.config.send_history = true;
    const auto history_context = proto::build_context(fixture.config, "git st", cwd,
                                                       {"echo password=bad", "git status"});
    check(history_context.at("recent_commands") == nlohmann::json::array({"git status"}),
          "Secret history sent");
}

void completion_tests() {
    check(proto::normalize_completion("git st", "git status\n") == "git status", "Valid completion");
    check(!proto::normalize_completion("git st", "git stash\nrm -rf /"), "Multiline completion");
    check(!proto::normalize_completion("git st", "git status\x1b[2J"), "Escape in completion");
    check(!proto::normalize_completion("git st", "status"), "Prefix was replaced");
    check(!proto::normalize_completion("git st", "```sh\ngit status\n```"), "Markdown accepted");
    check(!proto::normalize_completion("git status", "git status"), "Empty suffix accepted");
    Fixture fixture;
    std::filesystem::create_directory(fixture.config.home / "Documents");
    std::ofstream(fixture.config.home / "my file.txt") << "test";
    std::ofstream(fixture.config.home / "Bob's.txt") << "test";
    check(proto::complete_local("cd Doc", fixture.config.home, fixture.config.home) == "cd Documents/",
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
    struct stat status {};
    check(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "History permissions");
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

} // namespace

int main() {
    (void)std::setlocale(LC_ALL, "");
    const std::pair<const char *, std::function<void()>> tests[]{
        {"text / privacy", text_tests}, {"configuration", config_tests},
        {"context", context_tests},     {"completion", completion_tests},
        {"history", history_tests}};
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
