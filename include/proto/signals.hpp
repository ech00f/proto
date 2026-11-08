#pragma once

#include <array>
#include <csignal>

namespace proto {

class SignalGuard {
  public:
    SignalGuard();
    ~SignalGuard();
    SignalGuard(const SignalGuard &) = delete;
    SignalGuard &operator=(const SignalGuard &) = delete;

  private:
    std::array<struct sigaction, 3> previous_{};
    std::size_t installed_ = 0;
};

int take_signal();

} // namespace proto
