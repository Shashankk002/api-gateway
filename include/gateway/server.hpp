#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/health.hpp"
#include "gateway/load_balancer.hpp"
#include "gateway/metrics.hpp"
#include "gateway/middleware.hpp"
#include "gateway/proxy.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/router.hpp"

namespace httplib {
class Server;
class Request;
class Response;
}  // namespace httplib

namespace gateway {

/// The gateway's HTTP front door. It sequences the other components rather than
/// containing their logic:
///
///   middleware pipeline -> router -> rate limiter -> load balancer
///   -> circuit breaker -> reverse proxy (retrying within a bounded budget)
///
/// `/health` and `/metrics` are answered by the gateway itself and never reach
/// routing, rate limiting or a backend. A HealthChecker runs in the background
/// for the object's lifetime, started on construction and joined by stop() and
/// the destructor.
///
/// Everything is owned per instance rather than in globals, so tests can run
/// servers side by side. Binding and serving are separate steps so a caller can
/// learn the port before the blocking serve loop starts.
class GatewayServer {
public:
    /// Uses the built-in route table.
    explicit GatewayServer(ServerConfig config);

    /// A null `log_sink` sends the access log to std::cerr.
    GatewayServer(ServerConfig config, Router router,
                  std::shared_ptr<LogSink> log_sink = nullptr);

    ~GatewayServer();

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    /// Pass 0 to let the OS pick a free port. False if the socket could not be
    /// bound.
    [[nodiscard]] bool bind(std::uint16_t port);

    /// The port actually bound, or 0 before a successful bind().
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

    /// Accepts connections until stop() is called. Requires a successful bind().
    [[nodiscard]] bool serve();

    /// bind() on the configured port, followed by serve().
    [[nodiscard]] bool run();

    /// False if the server stopped before becoming ready.
    [[nodiscard]] bool wait_until_ready();

    /// Stops the accept loop and the health checker. Thread-safe, idempotent.
    void stop();

    [[nodiscard]] const Router& router() const noexcept { return router_; }
    [[nodiscard]] const BackendHealth& health() const noexcept { return health_; }
    [[nodiscard]] const CircuitBreakers& breakers() const noexcept { return breakers_; }
    [[nodiscard]] MetricsRegistry& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const Pipeline& pipeline() const noexcept { return pipeline_; }

    /// Null when rate limiting is disabled.
    [[nodiscard]] RateLimiter* rate_limiter() const noexcept { return limiter_.get(); }

    /// The peer address the socket reports. Client-supplied headers are never
    /// trusted, so nothing a caller sends can move it into someone else's bucket.
    [[nodiscard]] static std::string client_key(const httplib::Request& request);

private:
    void register_routes();

    void run_pipeline(const httplib::Request& request, httplib::Response& response,
                      const Handler& terminal) const;

    void handle_service_request(RequestContext& context) const;

    /// Forwards to an eligible instance, retrying onto others while the failure
    /// is transient and budget remains.
    void dispatch_to_service(const std::string& service, const httplib::Request& request,
                             httplib::Response& response) const;

    // Declaration order matters: health_ must outlive both the balancer that
    // reads it and the checker that writes it.
    ServerConfig config_;
    // Mutable so the const request handlers can record; instrumentation is not
    // a configuration change.
    mutable MetricsRegistry metrics_;
    Pipeline pipeline_;
    std::unique_ptr<RateLimiter> limiter_;
    BackendHealth health_;
    CircuitBreakers breakers_;
    Router router_;
    LoadBalancer balancer_;
    ReverseProxy proxy_;
    HealthChecker checker_;
    std::unique_ptr<httplib::Server> http_;
    std::uint16_t bound_port_{0};

    /// httplib::Server::stop() is neither safe against concurrent callers nor
    /// safe to repeat while the accept loop is still unwinding: it asserts on
    /// the socket it has already invalidated. These make the documented
    /// thread-safe, idempotent contract above true. Reset by bind(), so a
    /// stop() that arrived before serve() does not disarm the real one.
    std::mutex stop_mutex_;
    bool stop_called_{false};
};

}  // namespace gateway
