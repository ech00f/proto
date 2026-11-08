#include "proto/config.hpp"

#include "proto/api_client.hpp"
#include "proto/text.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <curl/curl.h>
#include <memory>
#include <pwd.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace proto {
namespace {

std::string environment(const char *name, std::string fallback = {}) {
    const char *value = std::getenv(name);
    return value != nullptr ? value : std::move(fallback);
}

bool boolean(const char *name, bool fallback) {
    const auto value = environment(name);
    if (value.empty()) {
        return fallback;
    }
    if (value == "1" || value == "true") {
        return true;
    }
    if (value == "0" || value == "false") {
        return false;
    }
    throw std::runtime_error(std::string(name) + " must be 0, 1, false, or true");
}

int number(const char *name, int fallback, int minimum, int maximum) {
    const auto value = environment(name);
    if (value.empty()) {
        return fallback;
    }
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < minimum ||
        result > maximum) {
        throw std::runtime_error(std::string(name) + " must be between " + std::to_string(minimum) +
                                 " and " + std::to_string(maximum));
    }
    return result;
}

std::string url_part(CURLU *url, CURLUPart part) {
    char *raw = nullptr;
    if (curl_url_get(url, part, &raw, 0) != CURLUE_OK) {
        return {};
    }
    const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
    return raw;
}

bool has_url_part(CURLU *url, CURLUPart part) {
    char *raw = nullptr;
    const bool present = curl_url_get(url, part, &raw, 0) == CURLUE_OK;
    curl_free(raw);
    return present;
}

bool loopback(std::string_view host) {
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parse_url(const std::string &value) {
    initialize_http();
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || !printable_utf8(value) ||
        curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK) {
        throw std::runtime_error("PROTO_API_BASE must be a valid HTTP(S) URL");
    }
    if (has_url_part(url.get(), CURLUPART_USER) || has_url_part(url.get(), CURLUPART_PASSWORD) ||
        value.find_first_of("?#") != std::string::npos) {
        throw std::runtime_error(
            "PROTO_API_BASE cannot contain credentials, a query, or a fragment");
    }
    const auto scheme = url_part(url.get(), CURLUPART_SCHEME);
    const auto host = url_part(url.get(), CURLUPART_HOST);
    if (scheme != "https" && !(scheme == "http" && loopback(host))) {
        throw std::runtime_error("Use HTTPS for remote APIs; HTTP is allowed only on loopback");
    }
    return url;
}

} // namespace

Config Config::from_environment() {
    Config config;
    config.home = environment("HOME");
    if (config.home.empty()) {
        const passwd *user = ::getpwuid(::getuid());
        if (user != nullptr) {
            config.home = user->pw_dir;
        }
    }
    const auto state = environment("XDG_STATE_HOME");
    config.state_directory = !state.empty() && std::filesystem::path(state).is_absolute()
                                 ? std::filesystem::path(state)
                                 : config.home / ".local/state";
    const auto directory = environment("XDG_CONFIG_HOME");
    config.config_file = (!directory.empty() && std::filesystem::path(directory).is_absolute()
                              ? std::filesystem::path(directory)
                              : config.home / ".config") /
                         "proto/config.json";
    config.load_file();
    config.api_base = environment("PROTO_API_BASE", config.api_base);
    while (!config.api_base.empty() && config.api_base.back() == '/') {
        config.api_base.pop_back();
    }
    config.model = environment("PROTO_MODEL", config.model);
    config.api_key = environment("PROTO_API_KEY");
    config.shell = environment("PROTO_SHELL", environment("SHELL", "/bin/sh"));
    config.ai_enabled = boolean("PROTO_AI", config.ai_enabled);
    config.theme = environment("PROTO_THEME", config.theme);
    config.debounce = std::chrono::milliseconds(
        number("PROTO_DEBOUNCE_MS", static_cast<int>(config.debounce.count()), 0, 5000));
    config.timeout = std::chrono::milliseconds(
        number("PROTO_TIMEOUT_MS", static_cast<int>(config.timeout.count()), 100, 30000));
    config.max_entries = static_cast<std::size_t>(
        number("PROTO_MAX_ENTRIES", static_cast<int>(config.max_entries), 1, 200));
    config.suggestion_count = static_cast<std::size_t>(
        number("PROTO_SUGGESTIONS", static_cast<int>(config.suggestion_count), 1, 5));
    config.persist_history = !boolean("PROTO_NO_HISTORY", !config.persist_history);
    config.send_history = boolean("PROTO_SEND_HISTORY", config.send_history);
    const auto scope =
        environment("PROTO_CONTEXT", std::string(context_name(config.context_scope)));
    if (scope == "none") {
        config.context_scope = ContextScope::None;
    } else if (scope == "cwd") {
        config.context_scope = ContextScope::CurrentDirectory;
    } else if (scope == "home") {
        config.context_scope = ContextScope::Home;
    } else {
        throw std::runtime_error("PROTO_CONTEXT must be none, cwd, or home");
    }
    return config;
}

void Config::validate() const {
    if (api_base.size() > 2048 || !printable_utf8(api_base)) {
        throw std::runtime_error("PROTO_API_BASE must be a printable HTTP(S) URL");
    }
    if (ai_enabled) {
        (void)parse_url(api_base);
    }
    if (model.empty() || model.size() > 256 || !printable_utf8(model)) {
        throw std::runtime_error("PROTO_MODEL must be a nonempty, single-line model name");
    }
    if (!printable_utf8(api_key)) {
        throw std::runtime_error("PROTO_API_KEY contains invalid characters");
    }
    std::error_code error;
    if (!printable_utf8(shell) || !std::filesystem::path(shell).is_absolute() ||
        !std::filesystem::is_regular_file(shell, error) || ::access(shell.c_str(), X_OK) != 0) {
        throw std::runtime_error("PROTO_SHELL / SHELL must name an executable absolute path");
    }
    if (!home.is_absolute()) {
        throw std::runtime_error("HOME must be an absolute directory path");
    }
    if (max_entries < 1 || max_entries > 200 || suggestion_count < 1 || suggestion_count > 5 ||
        debounce.count() < 0 || debounce.count() > 5000 || timeout.count() < 100 ||
        timeout.count() > 30000) {
        throw std::runtime_error("Completion settings are outside their allowed ranges");
    }
    if (theme != "aurora" && theme != "midnight") {
        throw std::runtime_error("PROTO_THEME must be aurora or midnight");
    }
}

std::string_view context_name(ContextScope scope) {
    switch (scope) {
    case ContextScope::None:
        return "none";
    case ContextScope::CurrentDirectory:
        return "cwd";
    case ContextScope::Home:
        return "home";
    }
    return "none";
}

bool Config::local_endpoint() const {
    const auto url = parse_url(api_base);
    return loopback(url_part(url.get(), CURLUPART_HOST));
}

} // namespace proto
