// End-to-end health-check tests: real backend servers behind a real gateway
// with background health checking on. Transitions are waited for on the
// gateway's own health state, never guessed with sleeps.

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::Route;
using gateway::Router;

// Short enough that transitions are quick, long enough that a loopback probe
// never times out on a loaded machine.
constexpr std::chrono::milliseconds kInterval{50};
constexpr std::chrono::milliseconds kProbeTimeout{2000};

using gateway_test::contains;

Router users_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"POST", "/users", "users"});
    return router;
}

/// Three healthy instances of one service, with health checking enabled.
class HealthCheckTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint(), c_.endpoint()}}};
        config.backend_timeout = kProbeTimeout;
        config.health_check_interval = kInterval;
        return config;
    }

    /// Backend names answering `count` sequential GET /users calls.
    std::vector<std::string> backends_hit(std::size_t count) {
        auto client = make_client();
        std::vector<std::string> names;
        for (std::size_t i = 0; i < count; ++i) {
            const auto response = client.Get("/users");
            names.push_back(response && response->status == 200 ? response->body
                                                                : "<failed>");
        }
        return names;
    }

    [[nodiscard]] bool wait_until_healthy(std::size_t index, bool healthy) {
        return wait_for_health(
            [this, index, healthy] { return health().is_healthy("users", index) == healthy; });
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
    gateway_test::TestBackend c_{"c"};
};

TEST_F(HealthCheckTest, AllHealthyBackendsReceiveTraffic) {
    ASSERT_TRUE(wait_for_health([this] {
        return health().is_healthy("users", 0) && health().is_healthy("users", 1) &&
               health().is_healthy("users", 2);
    }));

    ASSERT_EQ(backends_hit(6).size(), 6U);
    EXPECT_EQ(a_.request_count(), 2U);
    EXPECT_EQ(b_.request_count(), 2U);
    EXPECT_EQ(c_.request_count(), 2U);
}

TEST_F(HealthCheckTest, UnhealthyBackendStopsReceivingTrafficAndThenRecovers) {
    // 1. Everything healthy: traffic is spread across all three.
    ASSERT_EQ(backends_hit(3).size(), 3U);
    EXPECT_EQ(b_.request_count(), 1U);

    // 2. b starts failing its health probe while still serving normal traffic.
    b_.set_health_status(500);
    ASSERT_TRUE(wait_until_healthy(1, false)) << "b was never detected as unhealthy";

    // 3. Traffic must now avoid b entirely, and split evenly over a and c.
    const std::size_t b_before = b_.request_count();
    const auto during_outage = backends_hit(8);
    for (const std::string& body : during_outage) {
        EXPECT_NE(body, R"({"backend":"b"})") << "unhealthy backend still served traffic";
        EXPECT_NE(body, "<failed>");
    }
    EXPECT_EQ(b_.request_count(), b_before) << "b received proxied traffic while unhealthy";
    EXPECT_EQ(a_.request_count(), 5U);
    EXPECT_EQ(c_.request_count(), 5U);

    // 4. b is still being probed, so it can come back.
    const std::size_t probes_before = b_.health_check_count();
    ASSERT_TRUE(wait_for_health([this, probes_before] {
        return b_.health_check_count() > probes_before + 1;
    })) << "an unhealthy backend must keep being health checked";

    // 5. b recovers and re-enters the rotation without restarting the gateway.
    b_.set_health_status(200);
    ASSERT_TRUE(wait_until_healthy(1, true)) << "b was never detected as recovered";

    const auto after_recovery = backends_hit(9);
    std::size_t served_by_b = 0;
    for (const std::string& body : after_recovery) {
        served_by_b += body == R"({"backend":"b"})" ? 1 : 0;
    }
    EXPECT_GT(served_by_b, 0U) << "recovered backend never received traffic again";
}

TEST_F(HealthCheckTest, NonSuccessHealthStatusMarksABackendUnhealthy) {
    for (const int status : {301, 400, 404, 500, 503}) {
        b_.set_health_status(status);
        ASSERT_TRUE(wait_until_healthy(1, false)) << "status under test: " << status;

        b_.set_health_status(200);
        ASSERT_TRUE(wait_until_healthy(1, true)) << "status under test: " << status;
    }
}

TEST_F(HealthCheckTest, HealthProbeTimeoutMarksABackendUnhealthy) {
    // Only the probe is delayed, so this isolates the probe timeout from the
    // backend's ability to serve normal traffic.
    b_.set_health_delay(kProbeTimeout + std::chrono::seconds{2});

    EXPECT_TRUE(wait_until_healthy(1, false));
}

TEST_F(HealthCheckTest, GatewayHealthIsIndependentOfBackendHealth) {
    a_.set_health_status(500);
    b_.set_health_status(500);
    c_.set_health_status(500);
    ASSERT_TRUE(wait_for_health([this] {
        return !health().is_healthy("users", 0) && !health().is_healthy("users", 1) &&
               !health().is_healthy("users", 2);
    }));

    auto client = make_client();
    const auto response = client.Get("/health");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
}

TEST_F(HealthCheckTest, AllBackendsUnhealthyReturns502) {
    a_.set_health_status(503);
    b_.set_health_status(503);
    c_.set_health_status(503);
    ASSERT_TRUE(wait_for_health([this] {
        return !health().is_healthy("users", 0) && !health().is_healthy("users", 1) &&
               !health().is_healthy("users", 2);
    }));

    auto client = make_client();
    const auto response = client.Get("/users/1");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_TRUE(contains(response->body, R"("error":"bad_gateway")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("reason":"no_backend_configured")")) << response->body;
}

TEST_F(HealthCheckTest, RoutingDecisionsStillPrecedeHealthAndSelection) {
    auto client = make_client();

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);

    const auto wrong_method = client.Delete("/users");
    ASSERT_TRUE(wrong_method);
    EXPECT_EQ(wrong_method->status, 405);
    EXPECT_EQ(wrong_method->get_header_value("Allow"), "GET, POST");
}

TEST_F(HealthCheckTest, ProxyingStillForwardsFullyForHealthyBackends) {
    a_.set_response(201, R"({"created":true})", "application/json",
                    httplib::Headers{{"X-Backend-Version", "7"}});

    auto client = make_client();
    const std::string body = R"({"name":"ada"})";
    const auto response = client.Post("/users/1", body, "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    EXPECT_EQ(response->body, R"({"created":true})");
    EXPECT_EQ(response->get_header_value("X-Backend-Version"), "7");

    ASSERT_EQ(a_.request_count(), 1U);
    const auto received = a_.received().front();
    EXPECT_EQ(received.method, "POST");
    EXPECT_EQ(received.path, "/users/1");
    EXPECT_EQ(received.body, body);
}

TEST_F(HealthCheckTest, RequestsAndHealthTransitionsRunConcurrentlyWithoutFailing) {
    constexpr int kThreads = 6;
    constexpr int kPerThread = 40;

    std::atomic<bool> stop_flipping{false};
    std::atomic<int> gateway_errors{0};
    std::atomic<int> unanswered{0};

    // c flaps for the whole test; a and b stay healthy, so every request must
    // still find a backend.
    std::thread flapper([this, &stop_flipping] {
        int status = 500;
        while (!stop_flipping.load(std::memory_order_relaxed)) {
            c_.set_health_status(status);
            status = status == 500 ? 200 : 500;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        c_.set_health_status(200);
    });

    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([this, &gateway_errors, &unanswered] {
            auto client = make_client();
            for (int i = 0; i < kPerThread; ++i) {
                const auto response = client.Get("/users");
                if (!response) {
                    ++unanswered;
                } else if (response->status != 200) {
                    ++gateway_errors;
                }
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    stop_flipping.store(true, std::memory_order_relaxed);
    flapper.join();

    EXPECT_EQ(unanswered.load(), 0) << "the gateway must answer every request";
    EXPECT_EQ(gateway_errors.load(), 0) << "healthy instances were always available";
}

/// A configured instance whose port nothing is listening on: the probe fails to
/// connect rather than returning a status.
class UnreachableInstanceHealthTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {live_.endpoint(), closed_endpoint_}}};
        config.backend_timeout = kProbeTimeout;
        config.health_check_interval = kInterval;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint closed_endpoint_ = gateway_test::closed_endpoint();
};

TEST_F(UnreachableInstanceHealthTest, ConnectionFailureMarksAnInstanceUnhealthyAndDrainsIt) {
    ASSERT_TRUE(wait_for_health([this] { return !health().is_healthy("users", 1); }));

    auto client = make_client();
    for (int i = 0; i < 6; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200) << "traffic must not reach the dead instance";
    }
    EXPECT_EQ(live_.request_count(), 6U);
}

}  // namespace
