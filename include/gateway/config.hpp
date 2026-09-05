#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace gateway {

struct BackendEndpoint {
    std::string host;
    std::uint16_t port{0};

    friend bool operator==(const BackendEndpoint&, const BackendEndpoint&) = default;
};

enum class RateLimitAlgorithm { kTokenBucket, kSlidingWindow };
enum class RateLimitMode { kLocal, kRedis };

/// What to do when Redis cannot be reached for a decision.
enum class RedisFailurePolicy {
    kFailOpen,    ///< Availability over protection.
    kFailClosed,  ///< Protection over availability.
};

/// Service name -> instances, in configured order. std::less<> allows
/// string_view lookups.
using BackendTable = std::map<std::string, std::vector<BackendEndpoint>, std::less<>>;

/// Built-in backend table, matching default_service_router().
[[nodiscard]] BackendTable default_backends();

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
    /// request, so the gateway cannot be used as an open proxy.
    BackendTable backends{default_backends()};

    /// Connect, read and write timeout for outbound requests, health probes
    /// included.
    std::chrono::milliseconds backend_timeout{kDefaultBackendTimeout};

    /// Zero disables health checking, leaving every instance eligible.
    std::chrono::milliseconds health_check_interval{kDefaultHealthCheckInterval};

    /// Extra attempts after a transient failure. Zero disables retries.
    unsigned max_retries{kDefaultMaxRetries};

    unsigned circuit_failure_threshold{kDefaultCircuitFailureThreshold};
    std::chrono::milliseconds circuit_cooldown{kDefaultCircuitCooldown};

    /// Off by default: throttling is an explicit decision.
    bool rate_limit_enabled{false};
    RateLimitAlgorithm rate_limit_algorithm{RateLimitAlgorithm::kTokenBucket};

    /// "requests per window". The token bucket reads it as a capacity refilled
    /// over the window; the sliding window as a ceiling over the trailing one.
    std::uint64_t rate_limit_requests{kDefaultRateLimitRequests};
    std::chrono::milliseconds rate_limit_window{kDefaultRateLimitWindow};

    RateLimitMode rate_limit_mode{RateLimitMode::kLocal};
    std::string redis_host{kDefaultRedisHost};
    std::uint16_t redis_port{kDefaultRedisPort};
    std::string redis_key_prefix{kDefaultRedisKeyPrefix};
    RedisFailurePolicy redis_failure_policy{RedisFailurePolicy::kFailOpen};

    bool metrics_enabled{true};
};

/// Precedence, lowest to highest: defaults, GATEWAY_* environment variables,
/// then the matching command-line flags. The README lists both spellings.
///
/// The first backend given from any source replaces the built-in table; later
/// ones append, so repeating a service name gives it several instances.
///
/// Throws std::invalid_argument if a value is missing or malformed.
ServerConfig load_config(int argc, const char* const* argv);

}  // namespace gateway
