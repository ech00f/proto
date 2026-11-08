#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

enum class RiskLevel { None, Caution, Danger };

struct Risk {
    RiskLevel level = RiskLevel::None;
    std::vector<std::string> reasons;
    void add(RiskLevel severity, std::string reason);
    nlohmann::json json() const;
};

Risk assess_command(std::string_view command);
std::string_view risk_name(RiskLevel level);

} // namespace proto
