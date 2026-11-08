#pragma once

#include "proto/api_client.hpp"
#include "proto/safety.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

struct PlanStep {
    std::string command;
    std::string explanation;
    Risk risk;
};

struct Plan {
    std::string title;
    std::vector<PlanStep> steps;
    RiskLevel risk() const;
    nlohmann::json json() const;
};

Plan parse_plan(std::string_view content);
Plan generate_plan(const Config &config, std::string_view task, const std::filesystem::path &cwd,
                   const std::vector<std::string> &history = {}, Cancellation cancellation = {});
std::string plan_script(const Plan &plan);

} // namespace proto
