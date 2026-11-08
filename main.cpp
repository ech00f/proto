#include "proto/api_client.hpp"
#include "proto/config.hpp"
#include "proto/context.hpp"
#include "proto/editor.hpp"
#include "proto/history.hpp"
#include "proto/local_completion.hpp"
#include "proto/plan.hpp"
#include "proto/safety.hpp"
#include "proto/shell.hpp"
#include "proto/signals.hpp"
#include "proto/text.hpp"
#include "proto/tui.hpp"

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
  --suggest-shell TEXT Print risk<TAB>command records for shell widgets
  --plan TASK         Preview an AI multi-command plan as JSON; never execute it
  --run-plan TASK     Generate, review and confirm a plan in the TUI
  --approve-plan TASK Review a plan, then print its script only after confirmation
  --menu              Open the control center; selected history goes to stdout
  --menu-shell        Open the menu and return non-secret settings to shell widgets
  --settings-shell    Print non-secret setting records for shell widgets
  --check COMMAND     Print risk JSON; exit 3 for danger, 0 otherwise
  --confirm COMMAND   Confirm dangerous input in the TUI; exit 3 on cancellation
  --record COMMAND    Add non-sensitive input to proto history; never execute it
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

void print_shell_settings(const proto::Config &config) {
    const auto settings = config.shell_settings();
    for (const auto &[name, value] : settings.items()) {
        std::cout << "setting\t" << name << '\t' << value.get<std::string>() << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    (void)std::setlocale(LC_ALL, "");
    try {
        bool no_ai = false;
        bool preview = false;
        bool alternatives = false;
        bool json = false;
        bool menu = false;
        bool menu_shell = false;
        bool settings_shell = false;
        bool run_plan = false;
        bool approve_plan = false;
        bool shell_format = false;
        bool record = false;
        bool check = false;
        bool confirm = false;
        std::optional<std::string> prefix;
        std::optional<std::string> task;
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
            } else if ((argument == "--menu" || argument == "--menu-shell") && !menu &&
                       !settings_shell && !preview && !prefix && !task) {
                menu = true;
                menu_shell = argument == "--menu-shell";
            } else if (argument == "--settings-shell" && !menu && !settings_shell && !preview &&
                       !prefix && !task) {
                settings_shell = true;
            } else if (argument == "--context" && !menu && !settings_shell && !preview && !prefix &&
                       !task) {
                preview = true;
            } else if ((argument == "--complete" || argument == "--suggest" ||
                        argument == "--suggest-json" || argument == "--suggest-shell" ||
                        argument == "--check" || argument == "--confirm" || argument == "--record") &&
                       !menu && !settings_shell && !preview && !prefix && !task && index + 1 < argc) {
                prefix = argv[++index];
                alternatives = argument != "--complete";
                json = argument == "--suggest-json";
                check = argument == "--check";
                confirm = argument == "--confirm";
                shell_format = argument == "--suggest-shell";
                record = argument == "--record";
            } else if ((argument == "--plan" || argument == "--run-plan" ||
                        argument == "--approve-plan") && !menu && !settings_shell && !preview &&
                       !prefix && !task && index + 1 < argc) {
                task = argv[++index];
                run_plan = argument == "--run-plan";
                approve_plan = argument == "--approve-plan";
            } else {
                throw std::runtime_error("Unknown or conflicting argument; run proto --help");
            }
        }
        auto config = proto::Config::from_environment();
        config.ai_enabled = config.ai_enabled && !no_ai;
        config.validate();
        if (settings_shell) {
            print_shell_settings(config);
            return 0;
        }
        proto::History history(config);
        const auto cwd = std::filesystem::current_path();
        if (prefix &&
            (prefix->size() > proto::max_command_bytes || !proto::printable_utf8(*prefix))) {
            throw std::runtime_error("Prefix must be printable UTF-8, at most 4096 bytes");
        }
        if (menu || confirm || run_plan || approve_plan) {
            const proto::SignalGuard signals;
            if (menu) {
                const auto recalled = proto::show_menu(config, cwd, history.entries());
                const int signal = proto::take_signal();
                if (signal != 0) {
                    return 128 + signal;
                }
                if (menu_shell) {
                    print_shell_settings(config);
                }
                if (recalled) {
                    std::cout << (menu_shell ? "recall\t" : "") << *recalled << '\n';
                }
                return 0;
            }
            if (confirm) {
                const bool approved = proto::confirm_command(*prefix);
                const int signal = proto::take_signal();
                return signal != 0 ? 128 + signal : approved ? 0 : 3;
            }
            if (const auto plan = proto::review_plan(config, *task, cwd, history.entries())) {
                history.add("?? " + *task);
                if (approve_plan) {
                    std::cout << proto::plan_script(*plan);
                    return 0;
                }
                proto::Shell shell(config);
                const auto result = shell.execute(proto::plan_script(*plan));
                const int signal = proto::take_signal();
                return signal != 0 ? 128 + signal : result.status;
            }
            const int signal = proto::take_signal();
            return signal != 0 ? 128 + signal : 3;
        }
        if (task) {
            std::cout << proto::generate_plan(config, *task, cwd, history.entries()).json().dump(2)
                      << '\n';
            return 0;
        }
        if (preview) {
            std::cout << proto::build_context(config, "", cwd, history.entries()).dump(2) << '\n';
            return 0;
        }
        if (prefix) {
            if (record) {
                history.add(*prefix);
                return 0;
            }
            if (check) {
                const auto risk = proto::assess_command(*prefix);
                std::cout << risk.json().dump() << '\n';
                return risk.level == proto::RiskLevel::Danger ? 3 : 0;
            }
            auto candidates = proto::complete_locals(*prefix, cwd, config.home, history.entries(),
                                                     alternatives ? config.suggestion_count : 1);
            if (config.ai_enabled && proto::can_send_prefix(*prefix)) {
                try {
                    const auto context =
                        proto::build_context(config, *prefix, cwd, history.entries());
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
                    output.push_back(
                        {{"command", command}, {"risk", proto::assess_command(command).json()}});
                }
                std::cout << output.dump() << '\n';
            } else {
                for (const auto &command : candidates) {
                    if (shell_format) {
                        std::cout << proto::risk_name(proto::assess_command(command).level) << '\t';
                    }
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
            config = editor.config();
            history.configure(config);
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
            if (input.state == proto::ReadState::Plan || input.command.starts_with("??")) {
                auto goal =
                    input.command.starts_with("??") ? input.command.substr(2) : input.command;
                goal.erase(0, goal.find_first_not_of(' '));
                try {
                    if (const auto plan = proto::review_plan(
                            config, goal, std::filesystem::current_path(), history.entries())) {
                        history.add("?? " + goal);
                        shell.execute(proto::plan_script(*plan));
                    }
                } catch (const std::exception &error) {
                    std::cerr << "proto: " << error.what() << '\n';
                }
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
