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
};

/// Builds a ServerConfig from the process environment and command line.
///
/// Precedence, lowest to highest: built-in defaults, environment variables
/// (GATEWAY_HOST, GATEWAY_PORT, GATEWAY_BACKENDS, GATEWAY_BACKEND_TIMEOUT_MS,
/// GATEWAY_HEALTH_CHECK_INTERVAL_MS), then the matching `--host`, `--port`,
/// `--backend`, `--backend-timeout-ms` and `--health-check-interval-ms`
/// arguments.
///
/// The first backend given from any source replaces the built-in table; further
/// ones add an instance, so repeating a service name gives it several
/// instances.
///
/// Throws std::invalid_argument if a supplied value is missing or malformed.
ServerConfig load_config(int argc, const char* const* argv);

}  // namespace gateway
