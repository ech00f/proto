#include "proto/local_completion.hpp"

#include "proto/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace proto {
namespace {

struct LastWord {
    std::size_t start = 0;
    std::string value;
    char quote = 0;
    bool escaped = false;
};

LastWord last_word(std::string_view prefix) {
    LastWord word;
    for (std::size_t position = 0; position < prefix.size(); ++position) {
        const char character = prefix[position];
        if (word.escaped) {
            word.value += character;
            word.escaped = false;
        } else if (character == '\\' && word.quote != '\'') {
            word.escaped = true;
        } else if (word.quote != 0) {
            if (character == word.quote) {
                word.quote = 0;
            } else {
                word.value += character;
            }
        } else if (character == '\'' || character == '"') {
            word.quote = character;
        } else if (std::isspace(static_cast<unsigned char>(character)) ||
                   std::string_view(";|&<>()").find(character) != std::string_view::npos) {
            word = {};
            word.start = position + 1;
        } else {
            word.value += character;
        }
    }
    return word;
}

std::string escape_suffix(std::string_view suffix, char quote) {
    std::string result;
    for (const char character : suffix) {
        if (quote == '\'') {
            result += character == '\'' ? "'\\''" : std::string(1, character);
        } else {
            const std::string_view special = quote == '"' ? "\\\"$`" : " \\$`\"'&;()|<>*?[]#!{}";
            if (special.find(character) != special.npos) {
                result += '\\';
            }
            result += character;
        }
    }
    return result;
}

} // namespace

std::optional<std::string> complete_local(std::string_view prefix, const std::filesystem::path &cwd,
                                          const std::filesystem::path &home,
                                          const std::vector<std::string> &history) {
    const auto candidates = complete_locals(prefix, cwd, home, history, 1);
    return candidates.empty() ? std::nullopt : std::optional<std::string>{candidates.front()};
}

std::vector<std::string> complete_locals(std::string_view prefix, const std::filesystem::path &cwd,
                                        const std::filesystem::path &home,
                                        const std::vector<std::string> &history, std::size_t limit) {
    std::vector<std::string> result;
    const auto add = [&](std::string command) {
        if (command.size() > prefix.size() && command.size() <= max_command_bytes &&
            printable_utf8(command) && result.size() < limit &&
            std::find(result.begin(), result.end(), command) == result.end()) {
            result.push_back(std::move(command));
        }
    };
    if (prefix.empty() || prefix.size() > max_command_bytes || !printable_utf8(prefix)) {
        return result;
    }
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->size() > prefix.size() && it->starts_with(prefix)) {
            add(*it);
        }
    }
    const auto word = last_word(prefix);
    if (word.escaped || word.value.find_first_of("$`") != std::string::npos) {
        return result;
    }
    if (word.start == 0 && word.value.find('/') == std::string::npos) {
        static constexpr auto commands = std::to_array<std::string_view>(
            {"cat", "cd", "curl", "echo", "exit", "find", "git", "grep", "ls", "mkdir", "mv", "pwd",
             "python3", "ssh", "tar", "touch", "wc"});
        for (const auto command : commands) {
            if (command.starts_with(word.value) && command.size() > word.value.size()) {
                add(std::string(prefix) + std::string(command.substr(word.value.size())));
            }
        }
        return result;
    }
    const auto slash = word.value.find_last_of('/');
    const auto stem = slash == word.value.npos ? word.value : word.value.substr(slash + 1);
    const auto directory =
        slash == word.value.npos ? std::string{} : word.value.substr(0, slash + 1);
    std::filesystem::path lookup = directory;
    if (directory == "~/" || directory.starts_with("~/")) {
        lookup = home / directory.substr(2);
    } else if (lookup.is_relative()) {
        lookup = cwd / lookup;
    }
    const bool directories_only = prefix.starts_with("cd ");
    std::vector<std::string> candidates;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        lookup, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    std::size_t scanned = 0;
    while (!error && iterator != end && scanned++ < 4096) {
        const auto name = iterator->path().filename().string();
        const bool is_directory = iterator->is_directory(error);
        if (!error && name.starts_with(stem) && printable_utf8(name) &&
            (!directories_only || is_directory) &&
            (name.front() != '.' || (!stem.empty() && stem.front() == '.'))) {
            candidates.push_back(name + (is_directory ? "/" : ""));
        }
        error.clear();
        iterator.increment(error);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto &candidate : candidates) {
        if (candidate.size() > stem.size()) {
            auto completion =
                std::string(prefix) + escape_suffix(candidate.substr(stem.size()), word.quote);
            if (word.quote != 0) {
                completion += word.quote;
            }
            if (completion.size() <= max_command_bytes) {
                add(std::move(completion));
            }
        }
    }
    return result;
}

} // namespace proto
