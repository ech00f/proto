#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace proto {

enum class ContextScope { None, CurrentDirectory, Home };

struct Config {
    std::string api_base = "http://127.0.0.1:11434/v1";
    std::string model = "qwen2.5-coder:7b";
    std::string api_key;
    std::string shell = "/bin/sh";
    std::filesystem::path home;
    std::filesystem::path state_directory;
    std::chrono::milliseconds debounce{250};
    std::chrono::milliseconds timeout{5000};
    std::size_t max_entries = 80;
    ContextScope context_scope = ContextScope::Home;
    bool ai_enabled = true;
    bool persist_history = true;
    bool send_history = false;

    static Config from_environment();
    void validate() const;
    bool local_endpoint() const;
};

} // namespace proto
