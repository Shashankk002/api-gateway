#pragma once

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/health.hpp"
#include "gateway/load_balancer.hpp"
#include "gateway/middleware.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/router.hpp"
#include "gateway/server.hpp"

namespace gateway_test {

/// How long a test waits for an expected health transition before failing.
inline constexpr std::chrono::milliseconds kHealthWaitTimeout{5000};

/// A real HTTP backend on an OS-assigned loopback port. It records every
/// request it receives so tests can assert on what was forwarded, and its reply
/// is configurable.
///
/// `GET /health` is answered separately from proxied traffic so a test can make
/// the backend look unhealthy while it keeps serving normal requests, and can
/// count the probes it received.
class TestBackend {
public:
    struct Received {
        std::string method;
        std::string path;
        std::string target;
        std::string body;
        httplib::Headers headers;

        [[nodiscard]] std::string header(const std::string& name) const {
            const auto found = headers.find(name);
            return found == headers.end() ? std::string{} : found->second;
        }
        [[nodiscard]] bool has_header(const std::string& name) const {
            return headers.find(name) != headers.end();
        }
    };

    explicit TestBackend(std::string name = "backend") {
        body_ = R"({"backend":")" + name + R"("})";

        const auto handler = [this](const httplib::Request& request,
                                    httplib::Response& response) {
            record_and_reply(request, response);
        };
        // Registered before the catch-alls, which httplib matches in order.
        server_.Get("/health", [this](const httplib::Request&, httplib::Response& response) {
            reply_to_health_probe(response);
        });
        // Per-method catch-alls: httplib only reads the body once routing picks
        // a handler, so a pre-routing hook would see an empty body.
        server_.Get(".*", handler);
        server_.Post(".*", handler);
        server_.Put(".*", handler);
        server_.Patch(".*", handler);
        server_.Delete(".*", handler);
        server_.Options(".*", handler);

        const int assigned = server_.bind_to_any_port("127.0.0.1");
        if (assigned < 0) {
            throw std::runtime_error("test backend could not bind a port");
        }
        port_ = static_cast<std::uint16_t>(assigned);
        thread_ = std::thread([this] { (void)server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~TestBackend() { stop(); }

    TestBackend(const TestBackend&) = delete;
    TestBackend& operator=(const TestBackend&) = delete;

    /// Stops serving. Idempotent; the port is released once this returns.
    void stop() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] gateway::BackendEndpoint endpoint() const {
        return gateway::BackendEndpoint{"127.0.0.1", port_};
    }

    void set_response(int status, std::string body,
                      std::string content_type = "application/json",
                      httplib::Headers headers = {}) {
        const std::lock_guard<std::mutex> guard(mutex_);
        status_ = status;
        body_ = std::move(body);
        content_type_ = std::move(content_type);
        extra_headers_ = std::move(headers);
    }

    /// Delays every reply, so a test can drive the gateway's backend timeout.
    void set_delay(std::chrono::milliseconds delay) {
        const std::lock_guard<std::mutex> guard(mutex_);
        delay_ = delay;
    }

    /// Status this backend answers health probes with. 200 by default; anything
    /// outside 2xx makes the gateway consider it unhealthy.
    void set_health_status(int status) {
        const std::lock_guard<std::mutex> guard(mutex_);
        health_status_ = status;
    }

    /// Delays health probes only, so a test can drive the probe timeout without
    /// slowing proxied traffic.
    void set_health_delay(std::chrono::milliseconds delay) {
        const std::lock_guard<std::mutex> guard(mutex_);
        health_delay_ = delay;
    }

    [[nodiscard]] std::size_t health_check_count() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return health_checks_;
    }

    [[nodiscard]] std::vector<Received> received() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return received_;
    }

    [[nodiscard]] std::size_t request_count() const {
        const std::lock_guard<std::mutex> guard(mutex_);
        return received_.size();
    }

private:
    void reply_to_health_probe(httplib::Response& response) {
        int status = 0;
        std::chrono::milliseconds delay{0};
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            ++health_checks_;
            status = health_status_;
            delay = health_delay_;
        }
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        response.status = status;
        response.set_content(R"({"status":"backend"})", "application/json");
    }

    void record_and_reply(const httplib::Request& request, httplib::Response& response) {
        int status = 0;
        std::string body;
        std::string content_type;
        httplib::Headers extra_headers;
        std::chrono::milliseconds delay{0};
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            received_.push_back(Received{request.method, request.path, request.target,
                                         request.body, request.headers});
            status = status_;
            body = body_;
            content_type = content_type_;
            extra_headers = extra_headers_;
            delay = delay_;
        }

        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        for (const auto& [name, value] : extra_headers) {
            response.set_header(name, value);
        }
        response.status = status;
        response.set_content(body, content_type);
    }

    mutable std::mutex mutex_;
    httplib::Server server_;
    std::thread thread_;
    std::uint16_t port_{0};

    int status_{200};
    std::string body_;
    std::string content_type_{"application/json"};
    httplib::Headers extra_headers_;
    std::chrono::milliseconds delay_{0};
    std::vector<Received> received_;

    int health_status_{200};
    std::chrono::milliseconds health_delay_{0};
    std::size_t health_checks_{0};
};

/// A LoadBalancer with the health state, circuit breakers and backend table it
/// needs, for tests that exercise selection without opening a socket. Members
/// are declared in the order the balancer depends on them.
///
/// The default failure threshold is high enough that a test never trips a
/// circuit unless it means to.
class SelectionPool {
public:
    explicit SelectionPool(gateway::BackendTable table, unsigned failure_threshold = 1000,
                           std::chrono::milliseconds cooldown = std::chrono::milliseconds{60000})
        : backends(std::move(table)),
          health(backends),
          breakers(backends, failure_threshold, cooldown),
          balancer(backends, health, breakers) {}

    SelectionPool(const SelectionPool&) = delete;
    SelectionPool& operator=(const SelectionPool&) = delete;

    /// Port of the next selection, or 0 when nothing is eligible.
    [[nodiscard]] std::uint16_t next_port(std::string_view service) {
        const auto selection = balancer.select(service);
        return selection ? selection.endpoint->port : 0;
    }

    gateway::BackendTable backends;
    gateway::BackendHealth health;
    gateway::CircuitBreakers breakers;
    gateway::LoadBalancer balancer;
};

/// Base fixture for integration tests: runs a real GatewayServer on an
/// OS-assigned loopback port for the duration of one test, and hands out
/// clients pointed at it.
///
/// Subclasses override make_router() to choose the route table under test.
class GatewayServerTestBase : public ::testing::Test {
protected:
    /// A loopback config with no backends and no background health checking, so
    /// a test sees only the behaviour it sets up. Tests that want health checks
    /// set health_check_interval themselves.
    [[nodiscard]] static gateway::ServerConfig loopback_config() {
        gateway::ServerConfig config;
        config.host = "127.0.0.1";
        config.backends.clear();
        config.health_check_interval = std::chrono::milliseconds{0};
        return config;
    }

    /// The route table the server under test should serve. Defaults to the
    /// gateway's built-in table.
    virtual gateway::Router make_router() { return gateway::default_service_router(); }

    /// The configuration, including backends, for the server under test.
    virtual gateway::ServerConfig make_config() { return loopback_config(); }

    void SetUp() override {
        // Captured rather than written to stderr, so the suite stays quiet and
        // logging assertions have something to read.
        log_sink_ = std::make_shared<gateway::CapturingLogSink>();
        server_ = std::make_unique<gateway::GatewayServer>(make_config(), make_router(), log_sink_);

        // Port 0 lets the OS pick a free port, so tests never collide.
        ASSERT_TRUE(server_->bind(0)) << "could not bind an ephemeral port";
        port_ = server_->bound_port();
        ASSERT_NE(port_, 0);

        serve_thread_ = std::thread([this] { (void)server_->serve(); });
        ASSERT_TRUE(server_->wait_until_ready());
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (serve_thread_.joinable()) {
            serve_thread_.join();
        }
    }

    /// Health state of the running gateway, for waiting on transitions.
    [[nodiscard]] const gateway::BackendHealth& health() const { return server_->health(); }

    /// Circuit breakers of the running gateway, for asserting on their state.
    [[nodiscard]] const gateway::CircuitBreakers& breakers() const { return server_->breakers(); }

    /// The running gateway's rate limiter, or nullptr when disabled.
    [[nodiscard]] gateway::RateLimiter* rate_limiter() const { return server_->rate_limiter(); }

    /// Access-log lines this gateway has written.
    [[nodiscard]] gateway::CapturingLogSink& log_sink() const { return *log_sink_; }

    /// Waits for `predicate` to hold, bounded, without polling sleeps.
    [[nodiscard]] bool wait_for_health(const std::function<bool()>& predicate) const {
        return server_->health().wait_for(predicate, kHealthWaitTimeout);
    }

    httplib::Client make_client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        return client;
    }

private:
    std::shared_ptr<gateway::CapturingLogSink> log_sink_;
    std::unique_ptr<gateway::GatewayServer> server_;
    std::thread serve_thread_;
    std::uint16_t port_{0};
};

}  // namespace gateway_test
