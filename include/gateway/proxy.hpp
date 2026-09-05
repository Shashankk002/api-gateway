#pragma once

#include <chrono>

#include "gateway/config.hpp"

namespace httplib {
class Request;
class Response;
}  // namespace httplib

namespace gateway {

enum class ProxyStatus {
    kForwarded,
    kBackendUnreachable,
    kBackendTimeout,
};

/// Forwards a request to one already-chosen backend instance. Selection is the
/// LoadBalancer's job and retrying is the caller's; this type does neither.
class ReverseProxy {
public:
    explicit ReverseProxy(std::chrono::milliseconds timeout);

    /// On success copies the backend's status, body and headers into
    /// `response`, which is left untouched on failure.
    [[nodiscard]] ProxyStatus forward(const BackendEndpoint& backend,
                                      const httplib::Request& request,
                                      httplib::Response& response) const;

    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

private:
    std::chrono::milliseconds timeout_;
};

}  // namespace gateway
