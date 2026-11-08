#pragma once

#include "proto/config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace proto {

struct CompletionResult {
    std::uint64_t generation = 0;
    std::string prefix;
    std::vector<std::string> commands;
    std::string warning;
    bool ai_response = false;
};

class CompletionService {
  public:
    explicit CompletionService(Config config);
    ~CompletionService();
    CompletionService(const CompletionService &) = delete;
    CompletionService &operator=(const CompletionService &) = delete;

    std::uint64_t submit(std::string prefix, std::filesystem::path cwd,
                         std::vector<std::string> history);
    void cancel();
    std::optional<CompletionResult> take_result();

  private:
    struct Request {
        std::uint64_t generation;
        std::string prefix;
        std::filesystem::path cwd;
        std::vector<std::string> history;
        std::chrono::steady_clock::time_point ready;
    };

    void run();
    void publish(CompletionResult result);

    Config config_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Request> pending_;
    std::deque<CompletionResult> results_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

} // namespace proto
