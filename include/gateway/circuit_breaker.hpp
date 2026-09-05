#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/config.hpp"

namespace gateway {

enum class CircuitState { kClosed, kOpen, kHalfOpen };

/// Failure protection for one backend instance.
///
/// Every successful try_acquire() must be paired with exactly one
/// record_success() or record_failure(). The lock covers only the state
/// transitions, never the request the caller goes on to make.
class CircuitBreaker {
public:
    CircuitBreaker(unsigned failure_threshold, std::chrono::milliseconds cooldown);

    /// In OPEN, moves to HALF_OPEN once the cooldown has passed and admits
    /// exactly one probe; a second concurrent probe is refused.
    [[nodiscard]] bool try_acquire();

    void record_success();

    /// Returns true when this call is what moved the breaker into OPEN, so
    /// callers can count transitions rather than failures. Advisory.
    bool record_failure();

    /// Lock-free hint for selection: true only while OPEN and still cooling.
    /// HALF_OPEN stays selectable, or an open breaker would never be probed.
    [[nodiscard]] bool blocks_selection() const;

    [[nodiscard]] CircuitState state() const noexcept {
        return state_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned consecutive_failures() const;

private:
    void open_locked();

    unsigned threshold_;
    std::chrono::milliseconds cooldown_;

    mutable std::mutex mutex_;
    unsigned failures_{0};         ///< Guarded by mutex_.
    bool probe_in_flight_{false};  ///< Guarded by mutex_.

    // Written under mutex_, read without it by blocks_selection() and state().
    std::atomic<CircuitState> state_{CircuitState::kClosed};
    std::atomic<std::int64_t> open_until_ns_{0};
};

/// One breaker per instance, addressed as health state is: by service and by
/// position in the configured list. Built once, so a breaker stays tied to its
/// endpoint as circuits open and close.
class CircuitBreakers {
public:
    CircuitBreakers(const BackendTable& backends, unsigned failure_threshold,
                    std::chrono::milliseconds cooldown);

    CircuitBreakers(const CircuitBreakers&) = delete;
    CircuitBreakers& operator=(const CircuitBreakers&) = delete;

    /// Null when the instance is unknown.
    [[nodiscard]] CircuitBreaker* find(std::string_view service, std::size_t index) const;

    [[nodiscard]] bool blocks_selection(std::string_view service, std::size_t index) const;

private:
    std::map<std::string, std::vector<std::unique_ptr<CircuitBreaker>>, std::less<>> breakers_;
};

}  // namespace gateway
