#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace proto {

class Suggestions {
  public:
    void assign(std::vector<std::string> commands) {
        const std::string previous(selected());
        commands_ = std::move(commands);
        const auto match = std::find(commands_.begin(), commands_.end(), previous);
        index_ = match == commands_.end() ? 0 : static_cast<std::size_t>(match - commands_.begin());
    }
    void clear() {
        commands_.clear();
        index_ = 0;
    }
    void filter(std::string_view prefix) {
        const std::string previous(selected());
        std::erase_if(commands_, [prefix](const std::string &command) {
            return !command.starts_with(prefix) || command.size() <= prefix.size();
        });
        const auto match = std::find(commands_.begin(), commands_.end(), previous);
        index_ = match == commands_.end() ? 0 : static_cast<std::size_t>(match - commands_.begin());
    }
    void cycle(int direction) {
        if (!commands_.empty()) {
            index_ = direction < 0 ? (index_ + commands_.size() - 1) % commands_.size()
                                   : (index_ + 1) % commands_.size();
        }
    }
    std::string_view selected() const {
        return commands_.empty() ? std::string_view{} : commands_[index_];
    }
    std::size_t size() const {
        return commands_.size();
    }
    std::size_t index() const {
        return index_;
    }

  private:
    std::vector<std::string> commands_;
    std::size_t index_ = 0;
};

} // namespace proto
