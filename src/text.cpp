#include "proto/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <limits>

namespace proto {
namespace {

struct Codepoint {
    std::uint32_t value = 0;
    std::size_t bytes = 0;
};

Codepoint decode(std::string_view text, std::size_t position) {
    if (position >= text.size()) {
        return {};
    }
    const auto first = static_cast<unsigned char>(text[position]);
    if (first < 0x80) {
        return {first, 1};
    }
    std::size_t count = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        count = 2;
        value = first & 0x1fU;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 3;
        value = first & 0x0fU;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 4;
        value = first & 0x07U;
        minimum = 0x10000;
    } else {
        return {};
    }
    if (count > text.size() - position) {
        return {};
    }
    for (std::size_t offset = 1; offset < count; ++offset) {
        const auto byte = static_cast<unsigned char>(text[position + offset]);
        if ((byte & 0xc0U) != 0x80U) {
            return {};
        }
        value = (value << 6U) | (byte & 0x3fU);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return {};
    }
    return {value, count};
}

bool forbidden(std::uint32_t value) {
    return value < 0x20 || (value >= 0x7f && value <= 0x9f) ||
           (value >= 0x202a && value <= 0x202e) || (value >= 0x2066 && value <= 0x2069) ||
           value == 0x200e || value == 0x200f || value == 0xfeff;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

template <std::size_t N>
bool contains_any(std::string_view text, const std::array<std::string_view, N> &needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [text](std::string_view needle) { return text.find(needle) != text.npos; });
}

int codepoint_width(std::uint32_t value) {
    const int width = ::wcwidth(static_cast<wchar_t>(value));
    return width < 0 ? 1 : width;
}

} // namespace

bool printable_utf8(std::string_view text) {
    for (std::size_t position = 0; position < text.size();) {
        const auto point = decode(text, position);
        if (point.bytes == 0 || forbidden(point.value)) {
            return false;
        }
        position += point.bytes;
    }
    return true;
}

bool looks_sensitive(std::string_view text) {
    const auto lowered = lowercase(text);
    static constexpr auto markers = std::to_array<std::string_view>(
        {"password", "passwd", "secret", "token", "api_key", "api-key", "apikey", "authorization",
         "private_key", "private-key", "access_key", "access-key", "sk-", "ghp_", "github_pat_",
         "-----begin", "eyj"});
    return contains_any(lowered, markers);
}

bool private_name(std::string_view name) {
    if (name.empty() || name.front() == '.' || !printable_utf8(name) || looks_sensitive(name)) {
        return true;
    }
    const auto lowered = lowercase(name);
    static constexpr auto markers =
        std::to_array<std::string_view>({"credential", "recovery", "wallet", "seed", ".pem", ".key",
                                         ".p12", ".pfx", "id_rsa", "id_ed25519"});
    return contains_any(lowered, markers);
}

bool can_send_prefix(std::string_view prefix) {
    return !prefix.empty() && prefix.size() <= max_command_bytes && printable_utf8(prefix) &&
           !looks_sensitive(prefix);
}

std::size_t previous_character(std::string_view text, std::size_t position) {
    position = std::min(position, text.size());
    if (position == 0) {
        return 0;
    }
    --position;
    while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U) {
        --position;
    }
    return position;
}

std::size_t next_character(std::string_view text, std::size_t position) {
    position = std::min(position, text.size());
    const auto point = decode(text, position);
    return std::min(text.size(), position + (point.bytes == 0 ? 1 : point.bytes));
}

int display_width(std::string_view text) {
    int result = 0;
    for (std::size_t position = 0; position < text.size();) {
        const auto point = decode(text, position);
        if (point.bytes == 0) {
            return result + 1;
        }
        const int width = codepoint_width(point.value);
        if (result > std::numeric_limits<int>::max() - width) {
            return std::numeric_limits<int>::max();
        }
        result += width;
        position += point.bytes;
    }
    return result;
}

std::string clip_columns(std::string_view text, int columns) {
    int used = 0;
    std::size_t position = 0;
    while (position < text.size() && columns >= 0) {
        const auto point = decode(text, position);
        if (point.bytes == 0 || forbidden(point.value)) {
            break;
        }
        const int width = codepoint_width(point.value);
        if (width > columns - used) {
            break;
        }
        used += width;
        position += point.bytes;
    }
    return std::string(text.substr(0, position));
}

} // namespace proto
