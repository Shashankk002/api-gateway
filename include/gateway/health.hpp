#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
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

class MetricsRegistry;

/// Health of every configured instance, keyed by service and by position in
/// that service's configured list.
///
/// Written by the health checker, read by request threads. Built once and never
/// resized, so each flag is an independent atomic and reads take no lock.
///
/// Instances start healthy: before its first probe an instance is not *known*
/// to be bad, so the gateway serves normally instead of rejecting everything
/// during a startup window.
class BackendHealth {
public:
    explicit BackendHealth(const BackendTable& backends);

    BackendHealth(const BackendHealth&) = delete;
    BackendHealth& operator=(const BackendHealth&) = delete;

    [[nodiscard]] bool is_healthy(std::string_view service, std::size_t index) const;

    /// Records a probe result. Returns true when the state actually changed.
    bool set_healthy(std::string_view service, std::size_t index, bool healthy);

    /// Flags for `service` in configured order, empty when unknown. Lets a
    /// caller read a whole service without repeating the lookup.
    [[nodiscard]] std::span<const std::atomic<bool>> flags(std::string_view service) const;

    /// Blocks until `predicate` holds or `timeout` elapses. Woken by every
    /// state change, so callers need no polling sleeps.
    [[nodiscard]] bool wait_for(const std::function<bool()>& predicate,
                                std::chrono::milliseconds timeout) const;

private:
    std::map<std::string, std::vector<std::atomic<bool>>, std::less<>> states_;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
};

/// Periodically probes every instance with `GET /health` and records the result
/// in a BackendHealth. Healthy means a response arrived with a 2xx status;
/// connection failures, timeouts and non-2xx are unhealthy. Unhealthy instances
/// keep being probed, so recovery needs no restart.
///
/// Probing runs only on this component's thread; nothing on the request path
/// ever waits for a probe.
class HealthChecker {
public:
    /// `health`, and `metrics` when given, must outlive this object.
    HealthChecker(BackendTable backends, BackendHealth& health,
                  std::chrono::milliseconds interval, std::chrono::milliseconds timeout,
                  MetricsRegistry* metrics = nullptr);
    ~HealthChecker();

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    /// Begins with an immediate sweep. Does nothing when the interval is zero,
    /// which disables health checking.
    void start();

    /// Signals the sweep to finish and joins the thread. Idempotent.
    void stop();

private:
    void run();
    void sweep();
    [[nodiscard]] bool probe(const BackendEndpoint& backend) const;

    BackendTable backends_;
    BackendHealth& health_;
    MetricsRegistry* metrics_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread thread_;

    /// Serialises stop() itself. The sweep thread waits on mutex_, so the join
    /// cannot be guarded by that one, and two concurrent stops must not both
    /// join the same thread.
    std::mutex stop_mutex_;
};

}  // namespace gateway
