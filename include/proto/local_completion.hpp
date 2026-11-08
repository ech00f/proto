#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

std::optional<std::string> complete_local(std::string_view prefix, const std::filesystem::path &cwd,
                                          const std::filesystem::path &home,
                                          const std::vector<std::string> &history = {});
std::vector<std::string> complete_locals(std::string_view prefix, const std::filesystem::path &cwd,
                                        const std::filesystem::path &home,
                                        const std::vector<std::string> &history = {},
                                        std::size_t limit = 5);

} // namespace proto
