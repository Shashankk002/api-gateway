#pragma once

#include <chrono>
#include <string_view>

#include "gateway/config.hpp"

namespace httplib {
class Request;
class Response;
}  // namespace httplib

namespace gateway {

/// Outcome of a forwarding attempt.
enum class ProxyStatus {
    kForwarded,           ///< Backend answered; its response was copied out.
    kUnknownService,      ///< No backend is configured for the service.
    kBackendUnreachable,  ///< Could not connect to the backend.
    kBackendTimeout,      ///< Backend did not answer in time.
};

/// Forwards a matched request to the one backend configured for its service.
///
/// Destinations come only from the table this is constructed with, never from
/// the request, so the gateway cannot be turned into an open proxy.
class ReverseProxy {
public:
    ReverseProxy(BackendTable backends, std::chrono::milliseconds timeout);

    /// Sends `request` to `service`'s backend and, on success, copies the
    /// backend's status, body and headers into `response`. `response` is left
    /// untouched on failure.
    [[nodiscard]] ProxyStatus forward(std::string_view service,
                                      const httplib::Request& request,
                                      httplib::Response& response) const;

    [[nodiscard]] const BackendTable& backends() const noexcept { return backends_; }
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

private:
    BackendTable backends_;
    std::chrono::milliseconds timeout_;
};

}  // namespace gateway
