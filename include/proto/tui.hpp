#pragma once

#include "proto/config.hpp"
#include "proto/plan.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace proto {

std::optional<std::string> show_menu(Config &config, const std::filesystem::path &cwd,
                                     const std::vector<std::string> &history);
bool confirm_command(std::string_view command);
std::optional<Plan> review_plan(const Config &config, std::string task,
                                const std::filesystem::path &cwd,
                                const std::vector<std::string> &history);

} // namespace proto
