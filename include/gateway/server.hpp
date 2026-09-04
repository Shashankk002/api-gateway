#pragma once

#include <cstdint>
#include <memory>

#include "gateway/config.hpp"
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
/// method-not-allowed, or is unknown. A matched request is then handed to the
/// ReverseProxy, which forwards it to that service's configured backend. The
/// server holds neither route-matching nor forwarding logic of its own.
///
/// The type owns its httplib::Server, Router and ReverseProxy, so handler state
/// lives on the instance instead of in globals, which lets tests run servers
/// side by side on their own ports, route tables and backends.
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

    /// Stops the accept loop; safe to call from another thread.
    void stop();

    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const Router& router() const noexcept { return router_; }
    [[nodiscard]] const ReverseProxy& proxy() const noexcept { return proxy_; }

private:
    void register_routes();

    /// Turns the Router's decision for `request` into a response, proxying to
    /// the selected service's backend when the request matches a route.
    void handle_service_request(const httplib::Request& request, httplib::Response& response) const;

    ServerConfig config_;
    Router router_;
    ReverseProxy proxy_;
    std::unique_ptr<httplib::Server> http_;
    std::uint16_t bound_port_{0};
};

}  // namespace gateway
