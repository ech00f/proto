#include "proto/signals.hpp"

#include <atomic>
#include <stdexcept>

namespace proto {
namespace {

std::atomic<int> pending_signal{0};
static_assert(std::atomic<int>::is_always_lock_free);
constexpr std::array watched{SIGINT, SIGTERM, SIGHUP};

extern "C" void handle_signal(int signal) {
    pending_signal.store(signal, std::memory_order_relaxed);
}

} // namespace

SignalGuard::SignalGuard() {
    struct sigaction action{};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    for (std::size_t index = 0; index < watched.size(); ++index) {
        if (::sigaction(watched[index], &action, &previous_[index]) != 0) {
            for (std::size_t previous = 0; previous < installed_; ++previous) {
                ::sigaction(watched[previous], &previous_[previous], nullptr);
            }
            throw std::runtime_error("Could not install terminal signal handlers");
        }
        ++installed_;
    }
}

SignalGuard::~SignalGuard() {
    for (std::size_t index = 0; index < installed_; ++index) {
        ::sigaction(watched[index], &previous_[index], nullptr);
    }
}

int take_signal() {
    return pending_signal.exchange(0, std::memory_order_relaxed);
}

} // namespace proto
