// End-to-end rate limiting: a real gateway in front of a real backend, proving
// a rejected request is answered by the gateway alone and never reaches any
// backend machinery.

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::CircuitState;
using gateway::Route;
using gateway::Router;

using gateway_test::contains;

Router users_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"POST", "/users", "users"});
    return router;
}

/// Three requests per long window, so the fourth from one client is rejected
/// and the window never rolls over mid-test.
class RateLimitTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.rate_limit_enabled = true;
        config.rate_limit_algorithm = gateway::RateLimitAlgorithm::kTokenBucket;
        config.rate_limit_requests = 3;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(RateLimitTest, RequestsUnderTheLimitReachTheBackend) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200) << "request " << i;
        EXPECT_EQ(response->body, R"({"backend":"users"})");
    }
    EXPECT_EQ(backend_.request_count(), 3U);
}

TEST_F(RateLimitTest, ExceedingTheLimitReturns429) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    const auto rejected = client.Get("/users");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 429);
    EXPECT_EQ(rejected->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(rejected->body, R"("error":"rate_limited")")) << rejected->body;
    EXPECT_TRUE(contains(rejected->body, R"("reason":"rate_limit_exceeded")")) << rejected->body;
}

TEST_F(RateLimitTest, RejectedRequestsNeverReachTheBackend) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }
    ASSERT_EQ(backend_.request_count(), 3U);

    for (int i = 0; i < 10; ++i) {
        const auto rejected = client.Get("/users");
        ASSERT_TRUE(rejected);
        ASSERT_EQ(rejected->status, 429);
    }

    EXPECT_EQ(backend_.request_count(), 3U) << "throttled traffic must not be proxied";
}

TEST_F(RateLimitTest, RejectionCarriesLimitRemainingAndRetryAfterHeaders) {
    auto client = make_client();

    const auto first = client.Get("/users");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->get_header_value("RateLimit-Limit"), "3");
    EXPECT_EQ(first->get_header_value("RateLimit-Remaining"), "2")
        << "an allowed response reports what is left";

    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    const auto rejected = client.Get("/users");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 429);
    EXPECT_EQ(rejected->get_header_value("RateLimit-Limit"), "3");
    EXPECT_EQ(rejected->get_header_value("RateLimit-Remaining"), "0");
    EXPECT_FALSE(rejected->get_header_value("Retry-After").empty())
        << "a rejected caller must be told when to come back";
}

TEST_F(RateLimitTest, ThrottlingDoesNotTouchTheBackendCircuit) {
    auto client = make_client();
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    const gateway::CircuitBreaker* breaker = breakers().find("users", 0);
    ASSERT_NE(breaker, nullptr);
    EXPECT_EQ(breaker->state(), CircuitState::kClosed);
    EXPECT_EQ(breaker->consecutive_failures(), 0U)
        << "a 429 is the gateway's own answer, not a backend failure";
}

TEST_F(RateLimitTest, GatewayHealthIsNotRateLimited) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    // /health is gateway-owned and answered before routing, so throttling a
    // client's proxied traffic must never take its health checks away.
    for (int i = 0; i < 10; ++i) {
        const auto response = client.Get("/health");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
    }
}

TEST_F(RateLimitTest, RoutingDecisionsStillComeFirst) {
    auto client = make_client();
    // Exhaust the allowance, then confirm 404/405 are unchanged: an unknown
    // route is not a rate-limit decision.
    for (int i = 0; i < 5; ++i) {
        (void)client.Get("/users");
    }

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);

    const auto wrong_method = client.Delete("/users");
    ASSERT_TRUE(wrong_method);
    EXPECT_EQ(wrong_method->status, 405);
    EXPECT_EQ(wrong_method->get_header_value("Allow"), "GET, POST");
}

/// A dead backend plus a tight limit: proves throttling short-circuits the
/// retry path, since a rejected request cannot spend retry budget it never got.
class RateLimitBeforeRetryTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {live_.endpoint(), closed_endpoint_}}};
        config.max_retries = 1;
        config.rate_limit_enabled = true;
        config.rate_limit_requests = 2;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint closed_endpoint_ = gateway_test::closed_endpoint();
};

TEST_F(RateLimitBeforeRetryTest, ThrottledRequestsConsumeNoRetryBudgetAndNoCircuitState) {
    auto client = make_client();

    // Spend the allowance. These may retry across instances as usual.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }
    const std::size_t live_before = live_.request_count();
    const gateway::CircuitBreaker* dead_breaker = breakers().find("users", 1);
    ASSERT_NE(dead_breaker, nullptr);
    const unsigned failures_before = dead_breaker->consecutive_failures();

    for (int i = 0; i < 15; ++i) {
        const auto rejected = client.Get("/users");
        ASSERT_TRUE(rejected);
        ASSERT_EQ(rejected->status, 429);
    }

    EXPECT_EQ(live_.request_count(), live_before) << "no attempt, so no proxying";
    EXPECT_EQ(dead_breaker->consecutive_failures(), failures_before)
        << "a throttled request must not be recorded against any circuit";
}

/// The sliding-window algorithm, driven through the gateway.
class SlidingWindowGatewayTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.rate_limit_enabled = true;
        config.rate_limit_algorithm = gateway::RateLimitAlgorithm::kSlidingWindow;
        config.rate_limit_requests = 2;
        config.rate_limit_window = std::chrono::milliseconds{400};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(SlidingWindowGatewayTest, LimitsThenRecoversAsTheWindowSlides) {
    auto client = make_client();

    EXPECT_EQ(client.Get("/users")->status, 200);
    EXPECT_EQ(client.Get("/users")->status, 200);
    const auto rejected = client.Get("/users");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 429);
    EXPECT_EQ(rejected->get_header_value("RateLimit-Limit"), "2");
    EXPECT_EQ(backend_.request_count(), 2U);

    // The window is a wall-clock span, so waiting past it is the only way to
    // observe it slide through the HTTP layer.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    const auto recovered = client.Get("/users");
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->status, 200);
    EXPECT_EQ(backend_.request_count(), 3U);
}

/// Rate limiting off: every prior behaviour must be untouched.
class RateLimitDisabledTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        return config;  // rate_limit_enabled defaults to false
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(RateLimitDisabledTest, NoLimiterIsBuiltAndTrafficFlowsFreely) {
    EXPECT_EQ(rate_limiter(), nullptr);

    auto client = make_client();
    for (int i = 0; i < 30; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
    }
    EXPECT_EQ(backend_.request_count(), 30U);
}

TEST_F(RateLimitDisabledTest, NoRateLimitHeadersAreAdded) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_TRUE(response->get_header_value("RateLimit-Limit").empty());
    EXPECT_TRUE(response->get_header_value("RateLimit-Remaining").empty());
}

}  // namespace
