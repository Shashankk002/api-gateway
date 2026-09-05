// /metrics through a real gateway: the endpoint itself, and that each subsystem
// moves the counter it is supposed to move.

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gateway/metrics.hpp"
#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::Route;
using gateway::Router;

using gateway_test::contains;

/// Waits, bounded, for a metric to reach a value. Health transitions notify on
/// the health state, which the checker updates from its probe threads before it
/// records the matching metric, so there is no notification to wait on here.
bool wait_for_metric(const std::function<bool()>& predicate,
                     std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}

Router users_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"POST", "/users", "users"});
    return router;
}

class MetricsGatewayTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        return config;
    }

    [[nodiscard]] std::string scrape() {
        auto client = make_client();
        const auto response = client.Get("/metrics");
        return response ? response->body : std::string{};
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(MetricsGatewayTest, MetricsEndpointReturns200WithPrometheusContentType) {
    auto client = make_client();
    const auto response = client.Get("/metrics");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("Content-Type"),
              "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_TRUE(contains(response->body, "# TYPE gateway_requests_total counter"))
        << response->body;
}

TEST_F(MetricsGatewayTest, MetricsIsGatewayOwnedAndNeverProxied) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/metrics"));
    ASSERT_TRUE(client.Get("/metrics"));

    EXPECT_EQ(backend_.request_count(), 0U) << "/metrics must never reach a backend";

    // A method the endpoint does not serve is not routed to a service either.
    const auto posted = client.Post("/metrics", "{}", "application/json");
    ASSERT_TRUE(posted);
    EXPECT_EQ(posted->status, 404);
    EXPECT_EQ(backend_.request_count(), 0U);
}

TEST_F(MetricsGatewayTest, HealthStillWorksAlongsideMetrics) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    EXPECT_EQ(backend_.request_count(), 0U);
}

TEST_F(MetricsGatewayTest, NormalRequestsIncrementTheLabelledCounter) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    EXPECT_EQ(metrics().requests.value({"GET", "200"}), 3U);
    EXPECT_TRUE(contains(scrape(), R"(gateway_requests_total{method="GET",status="200"} 3)"));
}

TEST_F(MetricsGatewayTest, StatusLabelsFollowTheActualOutcome) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users"));            // 200
    ASSERT_TRUE(client.Get("/nothing-here"));     // 404
    ASSERT_TRUE(client.Delete("/users"));         // 405

    EXPECT_EQ(metrics().requests.value({"GET", "200"}), 1U);
    EXPECT_EQ(metrics().requests.value({"GET", "404"}), 1U);
    EXPECT_EQ(metrics().requests.value({"DELETE", "405"}), 1U);
}

TEST_F(MetricsGatewayTest, LatencyHistogramRecordsEveryRequest) {
    const std::uint64_t before = metrics().request_duration.count();

    auto client = make_client();
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    EXPECT_EQ(metrics().request_duration.count(), before + 4);
    EXPECT_GT(metrics().request_duration.sum(), 0.0);
    EXPECT_EQ(metrics().request_duration.cumulative_counts().back(),
              metrics().request_duration.count())
        << "the last bucket must hold everything at or below the largest bound";
}

TEST_F(MetricsGatewayTest, ScrapingDoesNotCountItselfOrGrowWithEachScrape) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users"));

    const std::uint64_t after_one_request = metrics().requests.total();
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(client.Get("/metrics"));
    }

    EXPECT_EQ(metrics().requests.total(), after_one_request)
        << "scraping must not move the numbers being scraped";
    EXPECT_EQ(metrics().request_duration.count(), 1U);
}

TEST_F(MetricsGatewayTest, InFlightGaugeReturnsToZero) {
    auto client = make_client();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    EXPECT_EQ(metrics().requests_in_flight.value(), 0);
    EXPECT_TRUE(contains(scrape(), "gateway_requests_in_flight 0"));
}

TEST_F(MetricsGatewayTest, BackendAttemptsAreCountedSeparatelyFromClientRequests) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    EXPECT_EQ(metrics().requests.total(), 3U) << "one client request counts once";
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 3U);
    EXPECT_EQ(metrics().backend_duration.count(), 3U);
    EXPECT_EQ(metrics().retry_attempts.value(), 0U) << "no retries were needed";
    EXPECT_EQ(metrics().retried_requests.value(), 0U);
}

TEST_F(MetricsGatewayTest, ConcurrentRequestsProduceExactTotalsAndDrainTheGauge) {
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this] {
            auto client = make_client();
            for (int i = 0; i < kPerThread; ++i) {
                (void)client.Get("/users");
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const auto expected = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(metrics().requests.value({"GET", "200"}), expected);
    EXPECT_EQ(metrics().request_duration.count(), expected);
    EXPECT_EQ(metrics().backend_requests.value({"users"}), expected);
    EXPECT_EQ(metrics().requests_in_flight.value(), 0);
}

/// Rate limiting on, so the 429 path can be observed.
class MetricsRateLimitTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.rate_limit_enabled = true;
        config.rate_limit_requests = 2;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(MetricsRateLimitTest, RejectedRequestsIncrementTheRateLimitCounter) {
    auto client = make_client();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    EXPECT_EQ(metrics().rate_limited.value(), 3U) << "two allowed, three rejected";
    EXPECT_EQ(metrics().requests.value({"GET", "429"}), 3U);
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 2U)
        << "a throttled request must not reach a backend attempt";
}

/// One dead instance beside a live one, so retries and circuits can be observed.
class MetricsFailureTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_endpoint_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 2;
        config.circuit_cooldown = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint dead_endpoint_ = gateway_test::closed_endpoint();
};

TEST_F(MetricsFailureTest, RetryCountsAttemptsAndRetriedRequestsSeparately) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200) << "the retry should have reached the live instance";

    EXPECT_EQ(metrics().requests.total(), 1U) << "one client request";
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 2U) << "two backend attempts";
    EXPECT_EQ(metrics().retry_attempts.value(), 1U) << "the first attempt is not a retry";
    EXPECT_EQ(metrics().retried_requests.value(), 1U) << "counted once per request";
    EXPECT_EQ(metrics().backend_failures.value({"users"}), 1U);
}

TEST_F(MetricsFailureTest, RepeatedFailuresOpenACircuitExactlyOnce) {
    auto client = make_client();
    // POST is never retried, so each request makes one attempt and roughly half
    // land on the dead instance.
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(client.Post("/users", "{}", "application/json"));
    }

    EXPECT_EQ(metrics().circuit_opens.value({"users"}), 1U)
        << "only a real transition into open should count, not every failure";
    EXPECT_GT(metrics().backend_failures.value({"users"}), 1U);
}

TEST_F(MetricsFailureTest, RequestsRefusedByAnOpenCircuitAreCounted) {
    auto client = make_client();
    // Drive the dead instance's circuit open, then take the live one out too so
    // nothing is selectable and the gateway answers 503 circuit_open.
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(client.Post("/users", "{}", "application/json"));
    }
    ASSERT_EQ(metrics().circuit_opens.value({"users"}), 1U);

    live_.stop();
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(client.Post("/users", "{}", "application/json"));
    }

    EXPECT_GT(metrics().circuit_rejected.value({"users"}), 0U);
    EXPECT_GT(metrics().requests.value({"POST", "503"}), 0U);
}

/// A backend that answers far later than the gateway will wait.
class MetricsTimeoutTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {slow_.endpoint()}}};
        config.backend_timeout = std::chrono::milliseconds{200};
        config.max_retries = 0;
        return config;
    }

    gateway_test::TestBackend slow_{"slow"};
};

TEST_F(MetricsTimeoutTest, BackendTimeoutsAreCounted) {
    slow_.set_delay(std::chrono::milliseconds{1200});

    auto client = make_client();
    client.set_read_timeout(10, 0);
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 504);

    EXPECT_EQ(metrics().backend_timeouts.value({"users"}), 1U);
    EXPECT_EQ(metrics().backend_failures.value({"users"}), 1U);
    EXPECT_EQ(metrics().requests.value({"GET", "504"}), 1U);
    EXPECT_GT(metrics().backend_duration.sum(), 0.1) << "the slow attempt should be visible";
}

/// Health checking on, so probe transitions can be observed.
class MetricsHealthTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.health_check_interval = std::chrono::milliseconds{50};
        config.backend_timeout = std::chrono::seconds{2};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(MetricsHealthTest, HealthTransitionsAreCounted) {
    backend_.set_health_status(500);
    ASSERT_TRUE(wait_for_health([this] { return !health().is_healthy("users", 0); }));
    EXPECT_TRUE(wait_for_metric([this] { return metrics().health_failures.value({"users"}) == 1U; }))
        << "a probe that turned the instance unhealthy must be counted once";

    backend_.set_health_status(200);
    ASSERT_TRUE(wait_for_health([this] { return health().is_healthy("users", 0); }));
    EXPECT_TRUE(
        wait_for_metric([this] { return metrics().health_recoveries.value({"users"}) == 1U; }))
        << "a probe that brought it back must be counted once";
    EXPECT_EQ(metrics().health_failures.value({"users"}), 1U) << "no extra failures were recorded";
}

/// Metrics disabled: the endpoint disappears, instrumentation stays harmless.
class MetricsDisabledTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.metrics_enabled = false;
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(MetricsDisabledTest, MetricsEndpointIsNotServed) {
    auto client = make_client();
    const auto response = client.Get("/metrics");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(backend_.request_count(), 0U) << "/metrics must not fall through to a backend";

    // Normal traffic is unaffected.
    const auto served = client.Get("/users");
    ASSERT_TRUE(served);
    EXPECT_EQ(served->status, 200);
}

}  // namespace
