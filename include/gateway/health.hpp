#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/config.hpp"

namespace gateway {

/// Current health of every configured backend instance, keyed by service and by
/// the instance's position in that service's configured list.
///
/// Written by the health checker, read by request threads. The map is built
/// once and never resized afterwards, so each flag is an independent atomic and
/// reads take no lock.
///
/// Instances start healthy: before the first probe an instance is not *known*
/// to be bad, so behaviour up to that point matches Stage 4 rather than
/// rejecting all traffic during a startup window.
class BackendHealth {
public:
    explicit BackendHealth(const BackendTable& backends);

    BackendHealth(const BackendHealth&) = delete;
    BackendHealth& operator=(const BackendHealth&) = delete;

    [[nodiscard]] bool is_healthy(std::string_view service, std::size_t index) const;

    /// Records a probe result. Returns true when the state actually changed.
    bool set_healthy(std::string_view service, std::size_t index, bool healthy);

    /// Health flags for `service`, in configured order; empty when unknown.
    /// Lets a caller read a whole service without repeating the lookup.
    [[nodiscard]] std::span<const std::atomic<bool>> flags(std::string_view service) const;

    /// Blocks until `predicate` holds or `timeout` elapses; returns whether it
    /// held. Woken by every state change, so callers need no polling sleeps.
    [[nodiscard]] bool wait_for(const std::function<bool()>& predicate,
                                std::chrono::milliseconds timeout) const;

private:
    std::map<std::string, std::vector<std::atomic<bool>>, std::less<>> states_;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
};

/// Periodically probes every configured backend instance and records the result
/// in a BackendHealth.
///
/// A probe is `GET /health` sent straight to the instance; it is healthy when a
/// response arrives with a 2xx status. Connection failures, timeouts and non-2xx
/// responses are unhealthy. Unhealthy instances keep being probed, so a backend
/// that recovers becomes eligible again without restarting the gateway.
///
/// Probing happens only on this component's own thread; nothing on the request
/// path ever waits for a probe.
class HealthChecker {
public:
    /// `health` must outlive this object.
    HealthChecker(BackendTable backends, BackendHealth& health,
                  std::chrono::milliseconds interval, std::chrono::milliseconds timeout);
    ~HealthChecker();

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    /// Starts the background sweep, beginning with an immediate one. Does
    /// nothing when the interval is zero, which disables health checking.
    void start();

    /// Signals the sweep to finish and joins the thread. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    /// Sweeps completed so far. Monotonic; lets a caller tell that at least one
    /// full pass has happened.
    [[nodiscard]] std::uint64_t completed_sweeps() const;

private:
    void run();
    void sweep();
    [[nodiscard]] bool probe(const BackendEndpoint& backend) const;

    BackendTable backends_;
    BackendHealth& health_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::uint64_t sweeps_{0};
    std::thread thread_;
};

}  // namespace gateway
