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

enum class CircuitState {
    kClosed,    ///< Normal traffic; consecutive failures are being counted.
    kOpen,      ///< Refusing traffic until the cooldown expires.
    kHalfOpen,  ///< Cooldown passed; one probe is allowed through.
};

/// Failure protection for a single backend instance.
///
/// Callers must pair every successful try_acquire() with exactly one
/// record_success() or record_failure(). The lock is held only around the state
/// transitions themselves, never around the request the caller then makes.
class CircuitBreaker {
public:
    CircuitBreaker(unsigned failure_threshold, std::chrono::milliseconds cooldown);

    /// Admits a request if the breaker allows one now. In OPEN this moves to
    /// HALF_OPEN once the cooldown has passed, admitting exactly one probe; a
    /// second concurrent probe is refused.
    [[nodiscard]] bool try_acquire();

    /// Closes the breaker and clears the failure count.
    void record_success();

    /// Counts a transient failure. Reaching the threshold from CLOSED opens the
    /// breaker; any failure in HALF_OPEN reopens it and restarts the cooldown.
    void record_failure();

    /// Lock-free hint for backend selection: true only while OPEN and still
    /// cooling down. HALF_OPEN stays selectable so a probe can get through,
    /// otherwise an open breaker would never be retried.
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
    unsigned failures_{0};        ///< Guarded by mutex_.
    bool probe_in_flight_{false}; ///< Guarded by mutex_.

    // Written under mutex_, read without it by blocks_selection() and state().
    std::atomic<CircuitState> state_{CircuitState::kClosed};
    std::atomic<std::int64_t> open_until_ns_{0};
};

/// One breaker per configured backend instance, addressed the same way health
/// state is: by service and by the instance's position in its configured list.
/// Built once from the backend table, so a breaker stays tied to its endpoint
/// even as circuits open and close.
class CircuitBreakers {
public:
    CircuitBreakers(const BackendTable& backends, unsigned failure_threshold,
                    std::chrono::milliseconds cooldown);

    CircuitBreakers(const CircuitBreakers&) = delete;
    CircuitBreakers& operator=(const CircuitBreakers&) = delete;

    /// Breaker for one instance, or nullptr when the instance is unknown.
    [[nodiscard]] CircuitBreaker* find(std::string_view service, std::size_t index) const;

    [[nodiscard]] bool blocks_selection(std::string_view service, std::size_t index) const;

private:
    std::map<std::string, std::vector<std::unique_ptr<CircuitBreaker>>, std::less<>> breakers_;
};

}  // namespace gateway
