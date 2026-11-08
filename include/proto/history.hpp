#pragma once

#include "proto/config.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

class History {
  public:
    explicit History(const Config &config);
    void add(std::string_view command);
    const std::vector<std::string> &entries() const;
    const std::string &warning() const;

  private:
    void load();
    void persist(std::string_view command);

    std::filesystem::path path_;
    bool enabled_;
    std::vector<std::string> entries_;
    std::string warning_;
};

} // namespace proto
