#pragma once

#include "proto/api_client.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace proto {

std::filesystem::path daemon_socket_path();
nlohmann::json daemon_status();
int serve_daemon();
std::optional<std::string> generate_via_daemon(const Config &config, const nlohmann::json &context,
                                               std::string_view instructions, int max_tokens,
                                               Cancellation cancellation);

} // namespace proto
