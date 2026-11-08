#pragma once

#include "proto/config.hpp"

#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

struct Cancellation {
    const std::atomic<std::uint64_t> *generation = nullptr;
    std::uint64_t expected = 0;
    bool cancelled() const;
};

class RequestCancelled : public std::runtime_error {
  public:
    RequestCancelled();
};

void initialize_http();
std::optional<std::string> normalize_completion(std::string_view prefix, std::string_view content);
std::vector<std::string> normalize_suggestions(std::string_view prefix, std::string_view content,
                                              std::size_t limit);

class ApiClient {
  public:
    explicit ApiClient(Config config);
    std::optional<std::string> complete(const nlohmann::json &context,
                                        Cancellation cancellation = {}) const;
    std::vector<std::string> suggest(const nlohmann::json &context,
                                     Cancellation cancellation = {}) const;
    std::string generate(const nlohmann::json &context, std::string_view instructions,
                         int max_tokens, Cancellation cancellation = {}) const;

  private:
    Config config_;
};

} // namespace proto
