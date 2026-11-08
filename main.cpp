#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/context.hpp"
#include "proto/editor.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/safety.hpp"
#include "proto/shell.hpp"
#include "proto/signals.hpp"
#include "proto/text.hpp"

#include <clocale>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

constexpr auto help = R"(proto — AI command autocomplete for your terminal

Usage: proto [--no-ai]
       proto [--no-ai] --context | --complete PREFIX
       proto --help | --version

  --complete PREFIX   Print a complete command without executing it
  --suggest PREFIX    Print alternative commands, one per line
  --suggest-json TEXT Print alternatives with risk labels as JSON
  --context           Preview the exact directory/history context sent to the model
  --no-ai             Use only local path and proto-history completions

In interactive mode: Tab / Right accepts ghost text; Ctrl+Right accepts one word.
Enter runs only the current input. Esc dismisses a suggestion. Up / Down browses
proto history. Ctrl+C cancels input; Ctrl+D exits an empty prompt.

Default backend: Ollama at http://127.0.0.1:11434/v1, model qwen2.5-coder:7b.
Configure PROTO_API_BASE, PROTO_MODEL, and optionally PROTO_API_KEY for another API.
PROTO_CONTEXT=none|cwd|home controls metadata (default: home).
File contents and shell history are never imported. PROTO_SEND_HISTORY=1 opts in
to sharing recent non-sensitive proto commands. See README.md for all settings.
)";

} // namespace

int main(int argc, char **argv) {
    (void)std::setlocale(LC_ALL, "");
    try {
        bool no_ai = false;
        bool preview = false;
        bool alternatives = false;
        bool json = false;
        std::optional<std::string> prefix;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") {
                std::cout << help;
                return 0;
            }
            if (argument == "--version") {
                std::cout << "proto 0.1.0\n";
                return 0;
            }
            if (argument == "--no-ai") {
                no_ai = true;
            } else if (argument == "--context" && !preview && !prefix) {
                preview = true;
            } else if ((argument == "--complete" || argument == "--suggest" ||
                        argument == "--suggest-json") &&
                       !preview && !prefix && index + 1 < argc) {
                prefix = argv[++index];
                alternatives = argument != "--complete";
                json = argument == "--suggest-json";
            } else {
                throw std::runtime_error("Unknown or conflicting argument; run proto --help");
            }
        }
        auto config = proto::Config::from_environment();
        config.ai_enabled = !no_ai;
        config.validate();
        proto::History history(config);
        const auto cwd = std::filesystem::current_path();
        if (preview) {
            std::cout << proto::build_context(config, "", cwd, history.entries()).dump(2) << '\n';
            return 0;
        }
        if (prefix) {
            if (prefix->size() > proto::max_command_bytes || !proto::printable_utf8(*prefix)) {
                throw std::runtime_error("Prefix must be printable UTF-8, at most 4096 bytes");
            }
            auto candidates = proto::complete_locals(*prefix, cwd, config.home, history.entries(),
                                                      alternatives ? config.suggestion_count : 1);
            if (config.ai_enabled && proto::can_send_prefix(*prefix)) {
                try {
                    const auto context = proto::build_context(config, *prefix, cwd, history.entries());
                    if (alternatives) {
                        auto generated = proto::ApiClient(config).suggest(context);
                        if (!generated.empty()) {
                            candidates = std::move(generated);
                        }
                    } else if (const auto generated = proto::ApiClient(config).complete(context)) {
                        candidates = {*generated};
                    }
                } catch (const std::exception &error) {
                    std::cerr << "proto: " << error.what() << "; using local completion\n";
                    if (candidates.empty()) {
                        return 2;
                    }
                }
            }
            if (json) {
                auto output = nlohmann::json::array();
                for (const auto &command : candidates) {
                    output.push_back({{"command", command}, {"risk", proto::assess_command(command).json()}});
                }
                std::cout << output.dump() << '\n';
            } else {
                for (const auto &command : candidates) {
                    std::cout << command << '\n';
                }
            }
            return 0;
        }
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
            throw std::runtime_error("Interactive mode requires a TTY; use --complete in scripts");
        }
        const proto::SignalGuard signals;
        proto::Editor editor(config);
        proto::Shell shell(config);
        std::cout << "proto — Tab accepts a suggestion; Enter runs your input; Ctrl+D exits.\n";
        if (config.ai_enabled) {
            std::cout << "AI uses your typed prefix and configured metadata; --context previews "
                         "metadata.\n";
        } else {
            std::cout << "AI disabled; local path/history completion only.\n";
        }
        std::string last_warning;
        for (;;) {
            const auto input = editor.read_line(std::filesystem::current_path(), history.entries());
            if (!editor.warning().empty() && editor.warning() != last_warning) {
                last_warning = editor.warning();
                std::cerr << "proto: " << last_warning << "; local completion remains available\n";
            }
            if (input.state == proto::ReadState::Terminated) {
                return 128 + input.signal;
            }
            if (input.state == proto::ReadState::Eof) {
                return shell.last_status();
            }
            if (input.state == proto::ReadState::Cancelled || input.command.empty()) {
                continue;
            }
            history.add(input.command);
            if (!history.warning().empty() && history.warning() != last_warning) {
                last_warning = history.warning();
                std::cerr << "proto: " << last_warning << '\n';
            }
            const auto result = shell.execute(input.command);
            if (result.exit) {
                return result.status;
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "proto: " << error.what() << '\n';
        return 1;
    }
}
