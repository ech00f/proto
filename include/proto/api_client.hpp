#pragma once

#include "proto/config.hpp"

#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

class ApiClient {
  public:
    explicit ApiClient(Config config);
    std::optional<std::string> complete(const nlohmann::json &context,
                                        Cancellation cancellation = {}) const;

  private:
    Config config_;
};

} // namespace proto
