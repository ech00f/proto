#include "proto/safety.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace proto {
namespace {

struct Token {
    std::string value;
    bool operator_token = false;
};

std::vector<Token> tokenize(std::string_view command, Risk &risk) {
    std::vector<Token> tokens;
    std::string word;
    bool started = false;
    char quote = 0;
    const auto flush = [&] {
        if (started) {
            tokens.push_back({std::move(word), false});
            word.clear();
            started = false;
        }
    };
    for (std::size_t index = 0; index < command.size(); ++index) {
        const char value = command[index];
        if (value == '\\' && quote != '\'') {
            started = true;
            if (index + 1 < command.size()) {
                word += command[++index];
            }
        } else if (quote != 0 && value == quote) {
            quote = 0;
        } else if (quote == 0 && (value == '\'' || value == '"')) {
            quote = value;
            started = true;
        } else if (quote != '\'' && (value == '`' || (value == '$' && index + 1 < command.size() &&
                                                      command[index + 1] == '('))) {
            risk.add(RiskLevel::Danger, "Command substitution can run hidden commands");
            started = true;
            word += value;
        } else if (quote != 0) {
            word += value;
        } else if (std::isspace(static_cast<unsigned char>(value))) {
            flush();
        } else if (std::string_view(";|&<>()").find(value) != std::string_view::npos) {
            flush();
            std::string op(1, value);
            if (index + 1 < command.size() && command[index + 1] == value) {
                op += command[++index];
            }
            tokens.push_back({std::move(op), true});
        } else if (value == '#' && !started) {
            break;
        } else {
            word += value;
            started = true;
        }
    }
    flush();
    return tokens;
}

bool has(const std::vector<std::string> &words, std::string_view word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

void inspect(const std::vector<std::string> &words, Risk &risk) {
    std::size_t first = 0;
    while (first < words.size() &&
           (words[first].find('=') != std::string::npos || words[first] == "command" ||
            words[first] == "builtin" || words[first] == "env")) {
        ++first;
    }
    if (first == words.size()) {
        return;
    }
    auto program = std::filesystem::path(words[first]).filename().string();
    if (program == "sudo" || program == "doas") {
        risk.add(RiskLevel::Caution, "Elevated privileges");
        ++first;
        while (first < words.size() && words[first].starts_with('-')) {
            const auto option = words[first++];
            if ((option == "-u" || option == "-g") && first < words.size()) {
                ++first;
            }
        }
        if (first == words.size()) {
            return;
        }
        program = std::filesystem::path(words[first]).filename().string();
    }
    if (program == "rm" || program == "rmdir" || program == "unlink" || program == "shred") {
        risk.add(RiskLevel::Danger, "Deletes files or directories");
    } else if (program == "dd" || program.starts_with("mkfs") || program == "wipefs" ||
               program == "fdisk" || program == "truncate") {
        risk.add(RiskLevel::Danger, "Can overwrite files or disks");
    } else if (program == "mv" || program == "cp" || program == "tee") {
        risk.add(RiskLevel::Caution, "Can overwrite destination files");
    } else if (program == "chmod" || program == "chown") {
        risk.add(RiskLevel::Caution, "Changes permissions or ownership");
    } else if (program == "eval" || program == "source" || program == ".") {
        risk.add(RiskLevel::Danger, "Executes additional code");
    } else if (program == "find" && has(words, "-delete")) {
        risk.add(RiskLevel::Danger, "Deletes matched files");
    } else if (program == "git") {
        if ((has(words, "reset") && has(words, "--hard")) ||
            (has(words, "clean") &&
             (has(words, "-f") || has(words, "-fd") || has(words, "-fdx"))) ||
            (has(words, "push") &&
             (has(words, "--force") || has(words, "-f") || has(words, "--force-with-lease")))) {
            risk.add(RiskLevel::Danger, "Destructive Git operation");
        }
    } else if (program == "sh" || program == "bash" || program == "zsh") {
        risk.add(RiskLevel::Caution, "Runs a shell or script; inspect its input");
    }
}

} // namespace

std::string_view risk_name(RiskLevel level) {
    switch (level) {
    case RiskLevel::None:
        return "none";
    case RiskLevel::Caution:
        return "caution";
    case RiskLevel::Danger:
        return "danger";
    }
    return "danger";
}

void Risk::add(RiskLevel severity, std::string reason) {
    level = std::max(level, severity);
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(std::move(reason));
    }
}

nlohmann::json Risk::json() const {
    return {{"level", risk_name(level)}, {"reasons", reasons}};
}

Risk assess_command(std::string_view command) {
    Risk risk;
    const auto tokens = tokenize(command, risk);
    std::vector<std::string> words;
    bool network = false;
    bool shell = false;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto &token = tokens[index];
        if (!token.operator_token) {
            words.push_back(token.value);
            network = network || token.value == "curl" || token.value == "wget";
            shell = shell || token.value == "sh" || token.value == "bash" || token.value == "zsh";
        } else if (token.value == ">" || token.value == ">>") {
            if (index + 1 < tokens.size() && tokens[index + 1].value != "/dev/null" &&
                tokens[index + 1].value != "&") {
                risk.add(RiskLevel::Caution, token.value == ">"
                                                 ? "Output redirection can overwrite a file"
                                                 : "Appends output to a file");
            }
        } else if (token.value == ";" || token.value == "|" || token.value == "||" ||
                   token.value == "&&" || token.value == "&") {
            inspect(words, risk);
            words.clear();
        }
    }
    inspect(words, risk);
    if (network && shell && command.find('|') != command.npos) {
        risk.add(RiskLevel::Danger, "Pipes downloaded content into a shell");
    }
    return risk;
}

} // namespace proto
