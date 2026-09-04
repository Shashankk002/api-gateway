#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gateway {

/// One instance of a logical service.
struct BackendEndpoint {
    std::string host;
    std::uint16_t port{0};

    friend bool operator==(const BackendEndpoint&, const BackendEndpoint&) = default;
};

/// Which local algorithm backs rate limiting.
enum class RateLimitAlgorithm { kTokenBucket, kSlidingWindow };

/// Where rate-limit state lives: this process only, or shared through Redis.
enum class RateLimitMode { kLocal, kRedis };

/// What to do when Redis cannot be reached for a decision.
enum class RedisFailurePolicy {
    kFailOpen,    ///< Allow the request; availability over protection.
    kFailClosed,  ///< Reject the request; protection over availability.
};

/// Logical service name -> its backend instances, in configured order.
/// std::less<> allows string_view lookups.
using BackendTable = std::map<std::string, std::vector<BackendEndpoint>, std::less<>>;

/// The gateway's built-in backend table, matching default_service_router().
[[nodiscard]] BackendTable default_backends();

/// Settings that control how the gateway's HTTP listener is started.
///
/// Kept as a plain value type so that later stages can grow it (or load it
/// from a file) without touching the server implementation.
struct ServerConfig {
    static constexpr std::uint16_t kDefaultPort = 8080;
    static constexpr const char* kDefaultHost = "0.0.0.0";
    static constexpr std::chrono::milliseconds kDefaultBackendTimeout{5000};
    static constexpr std::chrono::milliseconds kDefaultHealthCheckInterval{5000};
    static constexpr unsigned kDefaultMaxRetries = 1;
    static constexpr unsigned kDefaultCircuitFailureThreshold = 5;
    static constexpr std::chrono::milliseconds kDefaultCircuitCooldown{5000};
    static constexpr std::uint64_t kDefaultRateLimitRequests = 100;
    static constexpr std::chrono::milliseconds kDefaultRateLimitWindow{60000};
    static constexpr const char* kDefaultRedisHost = "127.0.0.1";
    static constexpr std::uint16_t kDefaultRedisPort = 6379;
    static constexpr const char* kDefaultRedisKeyPrefix = "gateway:ratelimit";

    std::string host{kDefaultHost};
    std::uint16_t port{kDefaultPort};

    /// The only destinations the gateway will proxy to. Never derived from a
    /// request, so the gateway cannot be used as an open proxy. A service may
    /// list several instances; requests are spread across them round-robin.
    BackendTable backends{default_backends()};

    /// Connect, read and write timeout for outbound backend requests, health
    /// probes included.
    std::chrono::milliseconds backend_timeout{kDefaultBackendTimeout};

    /// How often each backend instance is probed with GET /health. Zero turns
    /// health checking off, leaving every configured instance eligible.
    std::chrono::milliseconds health_check_interval{kDefaultHealthCheckInterval};

    /// Extra attempts allowed after a transient failure, on top of the first
    /// one. Only safe methods are retried, and never onto an instance this
    /// request has already tried. Zero disables retries.
    unsigned max_retries{kDefaultMaxRetries};

    /// Consecutive transient failures that open a backend instance's circuit.
    unsigned circuit_failure_threshold{kDefaultCircuitFailureThreshold};

    /// How long an open circuit refuses traffic before admitting one probe.
    std::chrono::milliseconds circuit_cooldown{kDefaultCircuitCooldown};

    /// Off by default: enabling throttling is an explicit decision, and leaving
    /// it off keeps the gateway's behaviour unchanged until it is asked for.
    bool rate_limit_enabled{false};
    RateLimitAlgorithm rate_limit_algorithm{RateLimitAlgorithm::kTokenBucket};

    /// "rate_limit_requests per rate_limit_window". The token bucket reads this
    /// as a capacity of that many with a refill of the same amount per window;
    /// the sliding window reads it as a ceiling over the trailing window.
    std::uint64_t rate_limit_requests{kDefaultRateLimitRequests};
    std::chrono::milliseconds rate_limit_window{kDefaultRateLimitWindow};

    RateLimitMode rate_limit_mode{RateLimitMode::kLocal};
    std::string redis_host{kDefaultRedisHost};
    std::uint16_t redis_port{kDefaultRedisPort};
    std::string redis_key_prefix{kDefaultRedisKeyPrefix};
    RedisFailurePolicy redis_failure_policy{RedisFailurePolicy::kFailOpen};
};

/// Builds a ServerConfig from the process environment and command line.
///
/// Precedence, lowest to highest: built-in defaults, environment variables
/// (GATEWAY_HOST, GATEWAY_PORT, GATEWAY_BACKENDS, GATEWAY_BACKEND_TIMEOUT_MS,
/// GATEWAY_HEALTH_CHECK_INTERVAL_MS, GATEWAY_MAX_RETRIES,
/// GATEWAY_CIRCUIT_FAILURE_THRESHOLD, GATEWAY_CIRCUIT_COOLDOWN_MS), then the
/// matching `--host`, `--port`, `--backend`, `--backend-timeout-ms`,
/// `--health-check-interval-ms`, `--max-retries`,
/// `--circuit-failure-threshold`, `--circuit-cooldown-ms`, `--rate-limit`,
/// `--rate-limit-algorithm`, `--rate-limit-requests`, `--rate-limit-window-ms`,
/// `--rate-limit-mode`, `--redis-host`, `--redis-port`, `--redis-key-prefix`
/// and `--redis-failure-policy` arguments.
///
/// The first backend given from any source replaces the built-in table; further
/// ones add an instance, so repeating a service name gives it several
/// instances.
///
/// Throws std::invalid_argument if a supplied value is missing or malformed.
ServerConfig load_config(int argc, const char* const* argv);

}  // namespace gateway
