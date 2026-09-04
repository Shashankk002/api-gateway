#pragma once

#include <cstdint>
#include <string>

namespace gateway {

/// Settings that control how the gateway's HTTP listener is started.
///
/// Kept as a plain value type so that later stages can grow it (or load it
/// from a file) without touching the server implementation.
struct ServerConfig {
    static constexpr std::uint16_t kDefaultPort = 8080;
    static constexpr const char* kDefaultHost = "0.0.0.0";

    std::string host{kDefaultHost};
    std::uint16_t port{kDefaultPort};
};

/// Builds a ServerConfig from the process environment and command line.
///
/// Precedence, lowest to highest: built-in defaults, the GATEWAY_HOST and
/// GATEWAY_PORT environment variables, then `--host <value>` / `--port <value>`
/// arguments.
///
/// Throws std::invalid_argument if a supplied value is missing or malformed.
ServerConfig load_config(int argc, const char* const* argv);

}  // namespace gateway
