#pragma once

#include "proto/config.hpp"

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace proto {

struct ShellResult {
    bool exit = false;
    int status = 0;
};

// A foreground command runner with small cd/exit/jobs/fg builtins, not a shell-language parser.
class Shell {
  public:
    explicit Shell(Config config);
    ~Shell();
    Shell(const Shell &) = delete;
    Shell &operator=(const Shell &) = delete;
    ShellResult execute(const std::string &command);
    int last_status() const;

  private:
    struct Job {
        int id;
        pid_t pid;
        std::string command;
    };

    int launch(const std::string &command);
    int wait(Job job, bool resume);

    Config config_;
    std::filesystem::path previous_directory_;
    std::vector<Job> jobs_;
    int next_job_id_ = 1;
    int last_status_ = 0;
};

} // namespace proto
