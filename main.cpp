#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/context.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/text.hpp"

#include <clocale>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr auto help = R"(proto — AI command autocomplete for your terminal

Usage: proto [--no-ai] [--context | --complete PREFIX]
       proto --help | --version

  --complete PREFIX   Print a complete command without executing it
  --context           Preview the exact directory/history context sent to the model
  --no-ai             Use only local path and proto-history completions

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
            } else if (argument == "--complete" && !preview && !prefix && index + 1 < argc) {
                prefix = argv[++index];
            } else {
                throw std::runtime_error("Unknown or conflicting argument; run proto --help");
            }
        }
        auto config = proto::Config::from_environment();
        config.ai_enabled = !no_ai;
        config.validate();
        const proto::History history(config);
        const auto cwd = std::filesystem::current_path();
        if (preview) {
            std::cout << proto::build_context(config, "", cwd, history.entries()).dump(2) << '\n';
            return 0;
        }
        if (prefix) {
            if (prefix->size() > proto::max_command_bytes || !proto::printable_utf8(*prefix)) {
                throw std::runtime_error("Prefix must be printable UTF-8, at most 4096 bytes");
            }
            auto completion = proto::complete_local(*prefix, cwd, config.home, history.entries());
            if (config.ai_enabled && proto::can_send_prefix(*prefix)) {
                try {
                    const auto generated = proto::ApiClient(config).complete(
                        proto::build_context(config, *prefix, cwd, history.entries()));
                    if (generated) {
                        completion = generated;
                    }
                } catch (const std::exception &error) {
                    std::cerr << "proto: " << error.what() << "; using local completion\n";
                    if (!completion) {
                        return 2;
                    }
                }
            }
            if (completion) {
                std::cout << *completion << '\n';
            }
            return 0;
        }
        std::cout << help;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "proto: " << error.what() << '\n';
        return 1;
    }
}
