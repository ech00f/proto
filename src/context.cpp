#include "proto/context.hpp"

#include "proto/text.hpp"

#include <algorithm>
#include <system_error>

namespace proto {
namespace {

std::vector<std::string> directory_names(const std::filesystem::path &path, bool directories_only,
                                         std::size_t limit) {
    std::vector<std::string> names;
    if (private_directory(path)) {
        return names;
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        path, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    std::size_t scanned = 0;
    while (!error && iterator != end && scanned++ < 4096) {
        const auto &entry = *iterator;
        const auto name = entry.path().filename().string();
        const auto status = entry.symlink_status(error);
        if (!error && !private_name(name) && name.size() <= 256 &&
            !std::filesystem::is_symlink(status)) {
            if (std::filesystem::is_directory(status)) {
                names.push_back(name + "/");
            } else if (!directories_only && std::filesystem::is_regular_file(status)) {
                names.push_back(name);
            }
        }
        error.clear();
        iterator.increment(error);
    }
    std::sort(names.begin(), names.end());
    if (names.size() > limit) {
        names.resize(limit);
    }
    return names;
}

} // namespace

std::string display_path(const std::filesystem::path &path, const std::filesystem::path &home) {
    const auto normalized = path.lexically_normal();
    auto relative = normalized.lexically_relative(home.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") {
        std::error_code error;
        const auto resolved_home = std::filesystem::canonical(home, error);
        if (!error) {
            const auto resolved_path = std::filesystem::weakly_canonical(normalized, error);
            if (!error) {
                relative = resolved_path.lexically_relative(resolved_home);
            }
        }
    }
    std::string result;
    if (relative == ".") {
        result = "~";
    } else if (!relative.empty() && *relative.begin() != "..") {
        result = "~/" + relative.generic_string();
    } else {
        result = normalized.generic_string();
    }
    return printable_utf8(result) ? result : "[non-printable path]";
}

bool private_directory(const std::filesystem::path &path) {
    return std::any_of(path.begin(), path.end(), [](const std::filesystem::path &part) {
        const auto name = part.string();
        return name != "/" && name != "." && name != ".." && private_name(name);
    });
}

nlohmann::json build_context(const Config &config, std::string_view prefix,
                             const std::filesystem::path &cwd,
                             const std::vector<std::string> &history) {
    nlohmann::json context{{"prefix", prefix},
                           {"shell", std::filesystem::path(config.shell).filename().string()}};
    if (config.context_scope != ContextScope::None) {
        context["cwd"] =
            private_directory(cwd) ? "[private directory]" : display_path(cwd, config.home);
        context["cwd_entries"] = directory_names(cwd, false, config.max_entries);
    }
    if (config.context_scope == ContextScope::Home) {
        context["home_directories"] = directory_names(config.home, true, config.max_entries);
    }
    if (config.send_history) {
        std::vector<std::string> recent;
        for (auto it = history.rbegin(); it != history.rend() && recent.size() < 8; ++it) {
            if (can_send_prefix(*it)) {
                recent.push_back(*it);
            }
        }
        std::reverse(recent.begin(), recent.end());
        context["recent_commands"] = std::move(recent);
    }
    return context;
}

} // namespace proto
