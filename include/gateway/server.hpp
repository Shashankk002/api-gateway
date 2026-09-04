#pragma once

#include <cstdint>
#include <memory>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/health.hpp"
#include "gateway/load_balancer.hpp"
#include "gateway/proxy.hpp"
#include "gateway/router.hpp"

namespace httplib {
class Server;
class Request;
class Response;
}

namespace gateway {

/// The gateway's HTTP front door.
///
/// It answers `GET /health` itself and delegates every other path to a Router,
/// which decides whether the request belongs to a logical service, is
/// method-not-allowed, or is unknown. For a matched request the LoadBalancer
/// picks one of that service's eligible backend instances and the ReverseProxy
/// forwards to it. On a transient failure the server may retry onto another
/// eligible instance within a bounded budget, recording each outcome in that
/// instance's circuit breaker. The server holds none of the routing, selection
/// or forwarding logic itself; it sequences them.
///
/// A HealthChecker runs in the background for the object's lifetime, keeping the
/// health state the LoadBalancer reads up to date. It starts on construction and
/// is stopped and joined by stop() and by the destructor.
///
/// The type owns all of those, so handler state lives on the instance instead of
/// in globals, which lets tests run servers side by side on their own ports,
/// route tables and backends.
///
/// Binding and serving are separate steps so that a caller (notably a test) can
/// learn the port before the blocking serve loop starts.
class GatewayServer {
public:
    /// Serves the gateway's built-in service route table.
    explicit GatewayServer(ServerConfig config);

    /// Serves an explicit route table. Backends and the backend timeout come
    /// from `config`. Used by tests today, and the seam through which
    /// configured routes will arrive later.
    GatewayServer(ServerConfig config, Router router);

    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    /// Binds the listening socket on the configured host. Pass 0 to let the OS
    /// pick a free port. Returns false if the socket could not be bound.
    [[nodiscard]] bool bind(std::uint16_t port);

    /// The port actually bound, or 0 when bind() has not succeeded.
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

    /// Accepts connections until stop() is called. Requires a successful bind().
    [[nodiscard]] bool serve();

    /// bind() on the configured port, followed by serve().
    [[nodiscard]] bool run();

    /// Blocks until serve() is accepting connections. Returns false if the
    /// server stopped before becoming ready.
    [[nodiscard]] bool wait_until_ready();

    /// Stops the accept loop and the background health checker; safe to call
    /// from another thread, and idempotent.
    void stop();

    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const Router& router() const noexcept { return router_; }
    [[nodiscard]] const BackendHealth& health() const noexcept { return health_; }
    [[nodiscard]] const CircuitBreakers& breakers() const noexcept { return breakers_; }
    [[nodiscard]] const LoadBalancer& balancer() const noexcept { return balancer_; }
    [[nodiscard]] const ReverseProxy& proxy() const noexcept { return proxy_; }

private:
    void register_routes();

    /// Turns the Router's decision for `request` into a response, proxying to a
    /// selected backend instance when the request matches a route.
    void handle_service_request(const httplib::Request& request, httplib::Response& response) const;

    /// Selects an eligible instance of `service`, forwards to it, and retries
    /// onto other instances while the failure is transient and budget remains.
    void dispatch_to_service(const std::string& service, const httplib::Request& request,
                             httplib::Response& response) const;

    // Declaration order is also destruction order reversed: health_ outlives
    // both the balancer that reads it and the checker that writes it.
    ServerConfig config_;
    BackendHealth health_;
    CircuitBreakers breakers_;
    Router router_;
    LoadBalancer balancer_;
    ReverseProxy proxy_;
    HealthChecker checker_;
    std::unique_ptr<httplib::Server> http_;
    std::uint16_t bound_port_{0};
};

}  // namespace gateway
