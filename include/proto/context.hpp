#pragma once

#include "proto/config.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

std::string display_path(const std::filesystem::path &path, const std::filesystem::path &home);
bool private_directory(const std::filesystem::path &path);
nlohmann::json build_context(const Config &config, std::string_view prefix,
                             const std::filesystem::path &cwd,
                             const std::vector<std::string> &history = {});

} // namespace proto
