#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace proto {

inline constexpr std::size_t max_command_bytes = 4096;

bool printable_utf8(std::string_view text);
bool looks_sensitive(std::string_view text);
bool private_name(std::string_view name);
bool can_send_prefix(std::string_view prefix);
std::size_t previous_character(std::string_view text, std::size_t position);
std::size_t next_character(std::string_view text, std::size_t position);
int display_width(std::string_view text);
std::string clip_columns(std::string_view text, int columns);

} // namespace proto
