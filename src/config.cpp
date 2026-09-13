#include "gateway/config.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <initializer_list>
#include <system_error>
#include <utility>

namespace gateway {
namespace {

// Bounds keep a misconfiguration from turning into a huge retry budget, an
// unreachable circuit threshold, or a cooldown that never expires.
constexpr unsigned long kMaxRetriesLimit = 10;
constexpr unsigned long kMaxFailureThreshold = 1000;
constexpr unsigned long kMaxCooldownMs = 3600000;
constexpr unsigned long kMaxRateLimitRequests = 1000000;
constexpr unsigned long kMaxRateLimitWindowMs = 3600000;
constexpr unsigned long kMaxRequestBodyBytesLimit = 1024UL * 1024UL * 1024UL;

std::uint16_t parse_port(std::string_view text, std::string_view source) {
    unsigned long value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);

    const bool fully_consumed = result.ec == std::errc{} && result.ptr == end;
    const bool in_range = value >= 1 && value <= std::numeric_limits<std::uint16_t>::max();
    if (text.empty() || !fully_consumed || !in_range) {
        throw std::invalid_argument(std::string(source) + ": expected a port in 1-65535, got '" +
                                    std::string(text) + "'");
    }
    return static_cast<std::uint16_t>(value);
}

/// Returns the value of `name`, or null when it is unset or empty.
const char* non_empty_env(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : nullptr;
}

std::chrono::milliseconds parse_timeout_ms(std::string_view text, std::string_view source) {
    unsigned long value = 0;
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != end || value == 0) {
        throw std::invalid_argument(std::string(source) +
                                    ": expected a positive number of milliseconds, got '" +
                                    std::string(text) + "'");
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

/// Parses one of a fixed set of spellings, so an unknown value is rejected
/// rather than silently defaulted.
template <typename Value>
Value parse_choice(std::string_view text, std::string_view source,
                   std::initializer_list<std::pair<std::string_view, Value>> choices) {
    for (const auto& [name, value] : choices) {
        if (text == name) {
            return value;
        }
    }
    std::string expected;
    for (const auto& [name, value] : choices) {
        if (!expected.empty()) {
            expected += "|";
        }
        expected.append(name);
    }
    throw std::invalid_argument(std::string(source) + ": expected " + expected + ", got '" +
                                std::string(text) + "'");
}

bool parse_bool(std::string_view text, std::string_view source) {
    return parse_choice<bool>(text, source,
                              {{"on", true}, {"true", true}, {"1", true}, {"yes", true},
                               {"off", false}, {"false", false}, {"0", false}, {"no", false}});
}

RateLimitAlgorithm parse_algorithm(std::string_view text, std::string_view source) {
    return parse_choice<RateLimitAlgorithm>(
        text, source,
        {{"token-bucket", RateLimitAlgorithm::kTokenBucket},
         {"sliding-window", RateLimitAlgorithm::kSlidingWindow}});
}

RateLimitMode parse_mode(std::string_view text, std::string_view source) {
    return parse_choice<RateLimitMode>(
        text, source, {{"local", RateLimitMode::kLocal}, {"redis", RateLimitMode::kRedis}});
}

RedisFailurePolicy parse_failure_policy(std::string_view text, std::string_view source) {
    return parse_choice<RedisFailurePolicy>(text, source,
                                            {{"open", RedisFailurePolicy::kFailOpen},
                                             {"closed", RedisFailurePolicy::kFailClosed}});
}

/// Parses a non-negative integer within [min, max].
unsigned long parse_bounded(std::string_view text, std::string_view source, unsigned long min,
                            unsigned long max) {
    unsigned long value = 0;
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != end || value < min ||
        value > max) {
        throw std::invalid_argument(std::string(source) + ": expected a value in " +
                                    std::to_string(min) + "-" + std::to_string(max) + ", got '" +
                                    std::string(text) + "'");
    }
    return value;
}

/// Health-check interval: 0 disables checking, otherwise up to an hour. The
/// upper bound also keeps the value far from any duration overflow.
std::chrono::milliseconds parse_interval_ms(std::string_view text, std::string_view source) {
    constexpr unsigned long kMaxIntervalMs = 3600000;

    unsigned long value = 0;
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != end || value > kMaxIntervalMs) {
        throw std::invalid_argument(std::string(source) + ": expected 0 (disabled) or 1-" +
                                    std::to_string(kMaxIntervalMs) + " milliseconds, got '" +
                                    std::string(text) + "'");
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

/// Parses "<host>:<port>", tolerating a leading "http://".
BackendEndpoint parse_endpoint(std::string_view text, std::string_view source) {
    constexpr std::string_view kScheme = "http://";
    if (text.starts_with(kScheme)) {
        text.remove_prefix(kScheme.size());
    }
    while (!text.empty() && text.back() == '/') {
        text.remove_suffix(1);
    }

    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
        throw std::invalid_argument(std::string(source) + ": expected <host>:<port>, got '" +
                                    std::string(text) + "'");
    }
    return BackendEndpoint{std::string(text.substr(0, colon)),
                           parse_port(text.substr(colon + 1), source)};
}

/// Parses "<service>=<host>:<port>".
std::pair<std::string, BackendEndpoint> parse_backend(std::string_view text,
                                                      std::string_view source) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        throw std::invalid_argument(std::string(source) +
                                    ": expected <service>=<host>:<port>, got '" +
                                    std::string(text) + "'");
    }
    return {std::string(text.substr(0, equals)),
            parse_endpoint(text.substr(equals + 1), source)};
}

std::string_view require_value(int argc, const char* const* argv, int index,
                               std::string_view flag) {
    if (index >= argc) {
        throw std::invalid_argument(std::string(flag) + " requires a value");
    }
    return argv[index];
}

}  // namespace

BackendTable default_backends() {
    return {
        {"users",
         {BackendEndpoint{"127.0.0.1", 9001}, BackendEndpoint{"127.0.0.1", 9002},
          BackendEndpoint{"127.0.0.1", 9003}}},
        {"orders", {BackendEndpoint{"127.0.0.1", 9010}, BackendEndpoint{"127.0.0.1", 9011}}},
        {"products", {BackendEndpoint{"127.0.0.1", 9020}}},
    };
}

ServerConfig load_config(int argc, const char* const* argv) {
    ServerConfig config{};

    // The first backend supplied from any source replaces the built-in table;
    // later ones append, so repeating a service name adds instances to it.
    bool backends_replaced = false;
    const auto add_backend = [&config, &backends_replaced](std::string_view spec,
                                                           std::string_view source) {
        if (!std::exchange(backends_replaced, true)) {
            config.backends.clear();
        }
        auto [service, endpoint] = parse_backend(spec, source);
        config.backends[std::move(service)].push_back(std::move(endpoint));
    };

    if (const char* host = non_empty_env("GATEWAY_HOST")) {
        config.host = host;
    }
    if (const char* port = non_empty_env("GATEWAY_PORT")) {
        config.port = parse_port(port, "GATEWAY_PORT");
    }
    if (const char* timeout = non_empty_env("GATEWAY_BACKEND_TIMEOUT_MS")) {
        config.backend_timeout = parse_timeout_ms(timeout, "GATEWAY_BACKEND_TIMEOUT_MS");
    }
    if (const char* interval = non_empty_env("GATEWAY_HEALTH_CHECK_INTERVAL_MS")) {
        config.health_check_interval =
            parse_interval_ms(interval, "GATEWAY_HEALTH_CHECK_INTERVAL_MS");
    }
    if (const char* retries = non_empty_env("GATEWAY_MAX_RETRIES")) {
        config.max_retries = static_cast<unsigned>(
            parse_bounded(retries, "GATEWAY_MAX_RETRIES", 0, kMaxRetriesLimit));
    }
    if (const char* threshold = non_empty_env("GATEWAY_CIRCUIT_FAILURE_THRESHOLD")) {
        config.circuit_failure_threshold = static_cast<unsigned>(parse_bounded(
            threshold, "GATEWAY_CIRCUIT_FAILURE_THRESHOLD", 1, kMaxFailureThreshold));
    }
    if (const char* enabled = non_empty_env("GATEWAY_RATE_LIMIT")) {
        config.rate_limit_enabled = parse_bool(enabled, "GATEWAY_RATE_LIMIT");
    }
    if (const char* algorithm = non_empty_env("GATEWAY_RATE_LIMIT_ALGORITHM")) {
        config.rate_limit_algorithm = parse_algorithm(algorithm, "GATEWAY_RATE_LIMIT_ALGORITHM");
    }
    if (const char* requests = non_empty_env("GATEWAY_RATE_LIMIT_REQUESTS")) {
        config.rate_limit_requests =
            parse_bounded(requests, "GATEWAY_RATE_LIMIT_REQUESTS", 1, kMaxRateLimitRequests);
    }
    if (const char* window = non_empty_env("GATEWAY_RATE_LIMIT_WINDOW_MS")) {
        config.rate_limit_window =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(parse_bounded(
                window, "GATEWAY_RATE_LIMIT_WINDOW_MS", 1, kMaxRateLimitWindowMs)));
    }
    if (const char* mode = non_empty_env("GATEWAY_RATE_LIMIT_MODE")) {
        config.rate_limit_mode = parse_mode(mode, "GATEWAY_RATE_LIMIT_MODE");
    }
    if (const char* host = non_empty_env("GATEWAY_REDIS_HOST")) {
        config.redis_host = host;
    }
    if (const char* port = non_empty_env("GATEWAY_REDIS_PORT")) {
        config.redis_port = parse_port(port, "GATEWAY_REDIS_PORT");
    }
    if (const char* prefix = non_empty_env("GATEWAY_REDIS_KEY_PREFIX")) {
        config.redis_key_prefix = prefix;
    }
    if (const char* body = non_empty_env("GATEWAY_MAX_REQUEST_BODY_BYTES")) {
        config.max_request_body_bytes = parse_bounded(body, "GATEWAY_MAX_REQUEST_BODY_BYTES", 0,
                                                      kMaxRequestBodyBytesLimit);
    }
    if (const char* metrics = non_empty_env("GATEWAY_METRICS")) {
        config.metrics_enabled = parse_bool(metrics, "GATEWAY_METRICS");
    }
    if (const char* policy = non_empty_env("GATEWAY_REDIS_FAILURE_POLICY")) {
        config.redis_failure_policy = parse_failure_policy(policy, "GATEWAY_REDIS_FAILURE_POLICY");
    }
    if (const char* cooldown = non_empty_env("GATEWAY_CIRCUIT_COOLDOWN_MS")) {
        config.circuit_cooldown =
            std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
                parse_bounded(cooldown, "GATEWAY_CIRCUIT_COOLDOWN_MS", 0, kMaxCooldownMs)));
    }
    if (const char* backends = non_empty_env("GATEWAY_BACKENDS")) {
        std::string_view remaining = backends;
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            add_backend(remaining.substr(0, comma), "GATEWAY_BACKENDS");
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--port") {
            config.port = parse_port(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--host") {
            config.host = require_value(argc, argv, ++i, arg);
        } else if (arg == "--backend") {
            add_backend(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--backend-timeout-ms") {
            config.backend_timeout = parse_timeout_ms(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--health-check-interval-ms") {
            config.health_check_interval =
                parse_interval_ms(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--max-retries") {
            config.max_retries = static_cast<unsigned>(
                parse_bounded(require_value(argc, argv, ++i, arg), arg, 0, kMaxRetriesLimit));
        } else if (arg == "--circuit-failure-threshold") {
            config.circuit_failure_threshold = static_cast<unsigned>(
                parse_bounded(require_value(argc, argv, ++i, arg), arg, 1, kMaxFailureThreshold));
        } else if (arg == "--rate-limit") {
            config.rate_limit_enabled = parse_bool(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--rate-limit-algorithm") {
            config.rate_limit_algorithm = parse_algorithm(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--rate-limit-requests") {
            config.rate_limit_requests = parse_bounded(require_value(argc, argv, ++i, arg), arg, 1,
                                                       kMaxRateLimitRequests);
        } else if (arg == "--rate-limit-window-ms") {
            config.rate_limit_window =
                std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
                    parse_bounded(require_value(argc, argv, ++i, arg), arg, 1,
                                  kMaxRateLimitWindowMs)));
        } else if (arg == "--rate-limit-mode") {
            config.rate_limit_mode = parse_mode(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--redis-host") {
            config.redis_host = require_value(argc, argv, ++i, arg);
        } else if (arg == "--redis-port") {
            config.redis_port = parse_port(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--redis-key-prefix") {
            config.redis_key_prefix = require_value(argc, argv, ++i, arg);
        } else if (arg == "--max-request-body-bytes") {
            config.max_request_body_bytes = parse_bounded(require_value(argc, argv, ++i, arg), arg,
                                                          0, kMaxRequestBodyBytesLimit);
        } else if (arg == "--metrics") {
            config.metrics_enabled = parse_bool(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--redis-failure-policy") {
            config.redis_failure_policy =
                parse_failure_policy(require_value(argc, argv, ++i, arg), arg);
        } else if (arg == "--circuit-cooldown-ms") {
            config.circuit_cooldown =
                std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
                    parse_bounded(require_value(argc, argv, ++i, arg), arg, 0, kMaxCooldownMs)));
        } else {
            throw std::invalid_argument("unknown argument '" + std::string(arg) + "'");
        }
    }

    return config;
}

}  // namespace gateway
