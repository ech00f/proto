#include "proto/completion_service.hpp"

#include "proto/api_client.hpp"
#include "proto/context.hpp"
#include "proto/local_completion.hpp"
#include "proto/text.hpp"

#include <utility>

namespace proto {

CompletionService::CompletionService(Config config)
    : config_(std::move(config)), worker_([this] { run(); }) {}

CompletionService::~CompletionService() {
    stopping_.store(true, std::memory_order_relaxed);
    cancel();
    condition_.notify_all();
    worker_.join();
}

std::uint64_t CompletionService::submit(std::string prefix, std::filesystem::path cwd,
                                        std::vector<std::string> history) {
    const auto generation = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        const std::lock_guard lock(mutex_);
        pending_ = Request{generation, std::move(prefix), std::move(cwd), std::move(history),
                           std::chrono::steady_clock::now() + config_.debounce};
        results_.clear();
    }
    condition_.notify_one();
    return generation;
}

void CompletionService::cancel() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard lock(mutex_);
        pending_.reset();
        results_.clear();
    }
    condition_.notify_one();
}

std::optional<CompletionResult> CompletionService::take_result() {
    const std::lock_guard lock(mutex_);
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

void CompletionService::publish(CompletionResult result) {
    const std::lock_guard lock(mutex_);
    if (generation_.load(std::memory_order_relaxed) == result.generation) {
        results_.push_back(std::move(result));
    }
}

void CompletionService::run() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] {
            return stopping_.load(std::memory_order_relaxed) || pending_.has_value();
        });
        if (stopping_.load(std::memory_order_relaxed)) {
            return;
        }
        const auto generation = pending_->generation;
        const auto ready = pending_->ready;
        const bool changed = condition_.wait_until(lock, ready, [&] {
            return stopping_.load(std::memory_order_relaxed) || !pending_ ||
                   pending_->generation != generation;
        });
        if (changed) {
            continue;
        }
        auto request = std::move(*pending_);
        pending_.reset();
        lock.unlock();

        const Cancellation cancellation{&generation_, request.generation};
        try {
            const auto local =
                complete_local(request.prefix, request.cwd, config_.home, request.history);
            if (cancellation.cancelled()) {
                continue;
            }
            if (local) {
                publish({request.generation, request.prefix, local, {}});
            }
            if (config_.ai_enabled && can_send_prefix(request.prefix)) {
                const auto context =
                    build_context(config_, request.prefix, request.cwd, request.history);
                const auto generated = ApiClient(config_).complete(context, cancellation);
                publish({request.generation, request.prefix, generated, {}, true});
            }
        } catch (const RequestCancelled &) {
        } catch (const std::exception &error) {
            publish({request.generation, request.prefix, std::nullopt, error.what()});
        }
    }
}

} // namespace proto
