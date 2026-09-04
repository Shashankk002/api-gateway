#include "gateway/circuit_breaker.hpp"

namespace gateway {
namespace {

std::int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

std::int64_t to_ns(std::chrono::milliseconds duration) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration).count();
}

}  // namespace

CircuitBreaker::CircuitBreaker(unsigned failure_threshold, std::chrono::milliseconds cooldown)
    : threshold_(failure_threshold == 0 ? 1 : failure_threshold), cooldown_(cooldown) {}

bool CircuitBreaker::try_acquire() {
    const std::lock_guard<std::mutex> guard(mutex_);
    switch (state_.load(std::memory_order_relaxed)) {
        case CircuitState::kClosed:
            return true;

        case CircuitState::kOpen:
            if (now_ns() < open_until_ns_.load(std::memory_order_relaxed)) {
                return false;
            }
            state_.store(CircuitState::kHalfOpen, std::memory_order_relaxed);
            probe_in_flight_ = true;
            return true;

        case CircuitState::kHalfOpen:
            if (probe_in_flight_) {
                return false;
            }
            probe_in_flight_ = true;
            return true;
    }
    return false;
}

void CircuitBreaker::record_success() {
    const std::lock_guard<std::mutex> guard(mutex_);
    failures_ = 0;
    probe_in_flight_ = false;
    state_.store(CircuitState::kClosed, std::memory_order_relaxed);
}

void CircuitBreaker::record_failure() {
    const std::lock_guard<std::mutex> guard(mutex_);
    const CircuitState current = state_.load(std::memory_order_relaxed);
    probe_in_flight_ = false;

    if (current == CircuitState::kHalfOpen) {
        open_locked();
        return;
    }
    if (current == CircuitState::kOpen) {
        return;  // Already open; the cooldown is already running.
    }
    if (++failures_ >= threshold_) {
        open_locked();
    }
}

void CircuitBreaker::open_locked() {
    failures_ = threshold_;
    probe_in_flight_ = false;
    open_until_ns_.store(now_ns() + to_ns(cooldown_), std::memory_order_relaxed);
    state_.store(CircuitState::kOpen, std::memory_order_relaxed);
}

bool CircuitBreaker::blocks_selection() const {
    if (state_.load(std::memory_order_relaxed) != CircuitState::kOpen) {
        return false;
    }
    return now_ns() < open_until_ns_.load(std::memory_order_relaxed);
}

unsigned CircuitBreaker::consecutive_failures() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return failures_;
}

CircuitBreakers::CircuitBreakers(const BackendTable& backends, unsigned failure_threshold,
                                 std::chrono::milliseconds cooldown) {
    for (const auto& [service, instances] : backends) {
        std::vector<std::unique_ptr<CircuitBreaker>> service_breakers;
        service_breakers.reserve(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            service_breakers.push_back(
                std::make_unique<CircuitBreaker>(failure_threshold, cooldown));
        }
        breakers_.emplace(service, std::move(service_breakers));
    }
}

CircuitBreaker* CircuitBreakers::find(std::string_view service, std::size_t index) const {
    const auto entry = breakers_.find(service);
    if (entry == breakers_.end() || index >= entry->second.size()) {
        return nullptr;
    }
    return entry->second[index].get();
}

bool CircuitBreakers::blocks_selection(std::string_view service, std::size_t index) const {
    const CircuitBreaker* breaker = find(service, index);
    return breaker != nullptr && breaker->blocks_selection();
}

}  // namespace gateway
