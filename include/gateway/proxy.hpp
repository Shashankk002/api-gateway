#pragma once

#include <chrono>

#include "gateway/config.hpp"

namespace httplib {
class Request;
class Response;
}  // namespace httplib

namespace gateway {

/// Outcome of a forwarding attempt.
enum class ProxyStatus {
    kForwarded,           ///< Backend answered; its response was copied out.
    kBackendUnreachable,  ///< Could not connect to the backend.
    kBackendTimeout,      ///< Backend did not answer in time.
};

/// Forwards a request to one already-chosen backend instance.
///
/// Choosing the instance is the LoadBalancer's job; this type only carries the
/// request there and brings the response back. It never selects among several
/// endpoints and never retries.
class ReverseProxy {
public:
    explicit ReverseProxy(std::chrono::milliseconds timeout);

    /// Sends `request` to `backend` and, on success, copies the backend's
    /// status, body and headers into `response`. `response` is left untouched
    /// on failure.
    [[nodiscard]] ProxyStatus forward(const BackendEndpoint& backend,
                                      const httplib::Request& request,
                                      httplib::Response& response) const;

    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

private:
    std::chrono::milliseconds timeout_;
};

}  // namespace gateway
