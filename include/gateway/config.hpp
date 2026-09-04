#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace gateway {

/// Where a logical service lives. Exactly one endpoint per service at this
/// stage; load balancing across several is a later stage.
struct BackendEndpoint {
    std::string host;
    std::uint16_t port{0};

    friend bool operator==(const BackendEndpoint&, const BackendEndpoint&) = default;
};

/// Logical service name -> backend. std::less<> allows string_view lookups.
using BackendTable = std::map<std::string, BackendEndpoint, std::less<>>;

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

    std::string host{kDefaultHost};
    std::uint16_t port{kDefaultPort};

    /// The only destinations the gateway will proxy to. Never derived from a
    /// request, so the gateway cannot be used as an open proxy.
    BackendTable backends{default_backends()};

    /// Connect, read and write timeout for outbound backend requests.
    std::chrono::milliseconds backend_timeout{kDefaultBackendTimeout};
};

/// Builds a ServerConfig from the process environment and command line.
///
/// Precedence, lowest to highest: built-in defaults, environment variables
/// (GATEWAY_HOST, GATEWAY_PORT, GATEWAY_BACKENDS, GATEWAY_BACKEND_TIMEOUT_MS),
/// then the matching `--host`, `--port`, `--backend` and `--backend-timeout-ms`
/// arguments.
///
/// The first backend given from any source replaces the built-in table; further
/// ones add to it or override a single service.
///
/// Throws std::invalid_argument if a supplied value is missing or malformed.
ServerConfig load_config(int argc, const char* const* argv);

}  // namespace gateway
