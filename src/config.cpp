#include "gateway/config.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gateway {
namespace {

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

/// Returns the value of `name`, or nullopt when it is unset or empty.
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

std::string_view require_value(int argc, const char* const* argv, int index, std::string_view flag) {
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
        } else {
            throw std::invalid_argument("unknown argument '" + std::string(arg) + "'");
        }
    }

    return config;
}

}  // namespace gateway
