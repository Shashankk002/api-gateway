#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/middleware.hpp"

namespace gateway {

class Counter {
public:
    void increment(std::uint64_t amount = 1) noexcept {
        value_.fetch_add(amount, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> value_{0};
};

class Gauge {
public:
    void increment() noexcept { value_.fetch_add(1, std::memory_order_relaxed); }
    void decrement() noexcept { value_.fetch_sub(1, std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> value_{0};
};

/// Latency histogram in seconds. Keeps only bucket counts, a count and a sum,
/// so memory is constant however many requests are observed.
class Histogram {
public:
    /// Prometheus-style upper bounds suited to an HTTP gateway.
    [[nodiscard]] static std::vector<double> default_bounds();

    explicit Histogram(std::vector<double> bounds = default_bounds());

    void observe(double seconds) noexcept;

    [[nodiscard]] const std::vector<double>& bounds() const noexcept { return bounds_; }
    /// Cumulative, as Prometheus expects.
    [[nodiscard]] std::vector<std::uint64_t> cumulative_counts() const;
    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

private:
    std::vector<double> bounds_;
    std::vector<std::atomic<std::uint64_t>> counts_;  ///< Per-bucket, not cumulative.
    std::atomic<std::uint64_t> count_{0};
    std::atomic<double> sum_{0.0};
};

/// A counter family sharing a fixed set of label names. Label values come from
/// the gateway or from configuration, never from client input; the series cap is
/// a backstop against unbounded cardinality, folding overflow into one "other"
/// series rather than dropping it.
class LabeledCounter {
public:
    static constexpr std::size_t kMaxSeries = 128;

    LabeledCounter(std::string name, std::string help, std::vector<std::string> label_names);

    /// Values must match label_names in order and count.
    void increment(const std::vector<std::string_view>& label_values, std::uint64_t amount = 1);

    [[nodiscard]] std::uint64_t value(const std::vector<std::string_view>& label_values) const;
    [[nodiscard]] std::uint64_t total() const;
    [[nodiscard]] std::size_t series_count() const;

    void render(std::string& out) const;

private:
    struct Series {
        std::vector<std::string> values;
        Counter counter;
    };

    [[nodiscard]] std::string key_for(const std::vector<std::string_view>& values) const;

    std::string name_;
    std::string help_;
    std::vector<std::string> label_names_;

    mutable std::shared_mutex mutex_;  ///< Shared for lookup, exclusive to create.
    std::map<std::string, std::unique_ptr<Series>> series_;
};

/// Every metric the gateway keeps. One instance per server, safe to use from
/// concurrent request threads.
///
/// Labels are bounded and low-cardinality only: `method` is an allowlist,
/// `status` comes from the gateway or a configured backend, `service` from the
/// configured backend table. Request ids, client addresses, paths, query
/// strings and headers are never labels.
class MetricsRegistry {
public:
    /// Pre-creates per-service series so a scrape shows them at zero.
    explicit MetricsRegistry(const BackendTable& backends);

    /// Folds anything outside the allowlist into "other".
    [[nodiscard]] static std::string_view normalize_method(std::string_view method);

    void observe_request(std::string_view method, int status, double seconds);

    /// Prometheus text exposition, format 0.0.4.
    [[nodiscard]] std::string render() const;
    static constexpr const char* kContentType = "text/plain; version=0.0.4; charset=utf-8";

    LabeledCounter requests;   ///< gateway_requests_total{method,status}
    Histogram request_duration;
    Gauge requests_in_flight;

    Counter rate_limited;      ///< gateway_rate_limited_requests_total
    Counter retry_attempts;    ///< gateway_retry_attempts_total
    Counter retried_requests;  ///< gateway_retried_requests_total

    LabeledCounter circuit_opens;      ///< gateway_circuit_opens_total{service}
    LabeledCounter circuit_rejected;   ///< gateway_circuit_rejected_requests_total{service}

    LabeledCounter backend_requests;   ///< gateway_backend_requests_total{service}
    LabeledCounter backend_failures;   ///< gateway_backend_failures_total{service}
    LabeledCounter backend_timeouts;   ///< gateway_backend_timeouts_total{service}
    Histogram backend_duration;

    LabeledCounter health_failures;    ///< gateway_backend_health_failures_total{service}
    LabeledCounter health_recoveries;  ///< gateway_backend_health_recoveries_total{service}
};

/// Records in-flight, latency and the {method,status} counter. The gauge is
/// held by an RAII guard, so an early return or exception cannot leave it
/// elevated. The metrics endpoint itself is excluded, so scraping does not move
/// the numbers being scraped.
class MetricsMiddleware final : public Middleware {
public:
    MetricsMiddleware(MetricsRegistry& metrics, std::string excluded_path);

    void handle(RequestContext& context, const Next& next) override;

private:
    MetricsRegistry& metrics_;
    std::string excluded_path_;
};

}  // namespace gateway
