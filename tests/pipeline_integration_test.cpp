// The middleware pipeline seen from outside: every existing gateway behaviour
// must survive it, and every response must carry a correlation id that also
// appears in the access log.

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/middleware.hpp"
#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::CircuitState;
using gateway::Route;
using gateway::Router;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

Router users_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"POST", "/users", "users"});
    return router;
}

/// True when some access-log line mentions `id` and `expected_status`.
bool logged(const gateway_test::GatewayServerTestBase&, const std::vector<std::string>& lines,
            const std::string& id, int expected_status) {
    for (const std::string& line : lines) {
        if (contains(line, id) && contains(line, "-> " + std::to_string(expected_status))) {
            return true;
        }
    }
    return false;
}

class GatewayPipelineTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(GatewayPipelineTest, HealthStillWorksAndCarriesARequestId) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    EXPECT_EQ(response->get_header_value("X-Request-Id").size(), 32U);
    EXPECT_EQ(backend_.request_count(), 0U) << "/health must stay gateway-owned";
}

TEST_F(GatewayPipelineTest, ProxiedResponseKeepsItsBodyHeadersAndRequestId) {
    backend_.set_response(201, R"({"created":true})", "application/json",
                          httplib::Headers{{"X-Backend-Version", "7"}});

    auto client = make_client();
    const auto response = client.Post("/users", R"({"name":"ada"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    EXPECT_EQ(response->body, R"({"created":true})");
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_EQ(response->get_header_value("X-Backend-Version"), "7")
        << "the backend's headers must survive the pipeline";
    EXPECT_EQ(response->get_header_value("X-Request-Id").size(), 32U)
        << "the id must survive the proxy replacing the header map";
}

TEST_F(GatewayPipelineTest, NotFoundAndMethodNotAllowedAreUnchanged) {
    auto client = make_client();

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);
    EXPECT_TRUE(contains(unknown->body, R"("error":"not_found")")) << unknown->body;
    EXPECT_FALSE(unknown->get_header_value("X-Request-Id").empty());

    const auto wrong_method = client.Delete("/users");
    ASSERT_TRUE(wrong_method);
    EXPECT_EQ(wrong_method->status, 405);
    EXPECT_EQ(wrong_method->get_header_value("Allow"), "GET, POST")
        << "the Allow header must not be lost";
    EXPECT_FALSE(wrong_method->get_header_value("X-Request-Id").empty());
}

TEST_F(GatewayPipelineTest, EachRequestGetsItsOwnId) {
    auto client = make_client();
    std::set<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        ids.insert(response->get_header_value("X-Request-Id"));
    }

    EXPECT_EQ(ids.size(), 20U) << "ids must not repeat across requests";
}

TEST_F(GatewayPipelineTest, TheResponseIdAppearsInTheAccessLog) {
    auto client = make_client();
    const auto response = client.Get("/users/7?debug=1");
    ASSERT_TRUE(response);
    const std::string id = response->get_header_value("X-Request-Id");
    ASSERT_FALSE(id.empty());

    const std::vector<std::string> lines = log_sink().lines();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(logged(*this, lines, id, 200))
        << "the id returned to the client must be the one logged";

    bool target_logged = false;
    for (const std::string& line : lines) {
        target_logged = target_logged || contains(line, "/users/7?debug=1");
    }
    EXPECT_TRUE(target_logged);
}

TEST_F(GatewayPipelineTest, FailedOutcomesAreLoggedWithTheirStatus) {
    auto client = make_client();

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    const std::vector<std::string> lines = log_sink().lines();
    EXPECT_TRUE(logged(*this, lines, unknown->get_header_value("X-Request-Id"), 404));
}

TEST_F(GatewayPipelineTest, ConcurrentRequestsGetDistinctIdsAndOneLogLineEach) {
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;

    std::vector<std::vector<std::string>> per_thread(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, &per_thread, t] {
            auto client = make_client();
            for (int i = 0; i < kPerThread; ++i) {
                const auto response = client.Get("/users");
                if (response) {
                    per_thread[static_cast<std::size_t>(t)].push_back(
                        response->get_header_value("X-Request-Id"));
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::set<std::string> ids;
    std::size_t total = 0;
    for (const auto& thread_ids : per_thread) {
        total += thread_ids.size();
        ids.insert(thread_ids.begin(), thread_ids.end());
    }

    EXPECT_EQ(total, static_cast<std::size_t>(kThreads) * kPerThread);
    EXPECT_EQ(ids.size(), total) << "concurrent requests must not share an id";
    EXPECT_EQ(log_sink().count(), total) << "one log line per request";
}

/// Rate limiting on: the 429 path must keep its headers and still be logged.
class PipelineWithRateLimitTest : public gateway_test::GatewayServerTestBase {
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

TEST_F(PipelineWithRateLimitTest, RateLimitHeadersAndRequestIdCoexist) {
    auto client = make_client();

    const auto allowed = client.Get("/users");
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 200);
    EXPECT_EQ(allowed->get_header_value("RateLimit-Limit"), "2");
    EXPECT_EQ(allowed->get_header_value("RateLimit-Remaining"), "1");
    EXPECT_FALSE(allowed->get_header_value("X-Request-Id").empty());

    ASSERT_TRUE(client.Get("/users"));

    const auto rejected = client.Get("/users");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 429);
    EXPECT_EQ(rejected->get_header_value("RateLimit-Limit"), "2");
    EXPECT_EQ(rejected->get_header_value("RateLimit-Remaining"), "0");
    EXPECT_FALSE(rejected->get_header_value("Retry-After").empty());
    EXPECT_FALSE(rejected->get_header_value("X-Request-Id").empty());
    EXPECT_TRUE(contains(rejected->body, R"("error":"rate_limited")")) << rejected->body;

    EXPECT_EQ(backend_.request_count(), 2U) << "throttling still precedes the backend";
    EXPECT_TRUE(logged(*this, log_sink().lines(), rejected->get_header_value("X-Request-Id"), 429));
}

/// A dead instance beside a live one: retries and the circuit must behave
/// exactly as they did before the pipeline existed.
class PipelineWithFailuresTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_endpoint_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 2;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint dead_endpoint_ = [] {
        const gateway_test::TestBackend probe;  // frees its port when destroyed
        return probe.endpoint();
    }();
};

TEST_F(PipelineWithFailuresTest, RetryStillRecoversAndTheResponseIsTheLiveBackends) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200) << "the retry should have reached the live instance";
    EXPECT_EQ(response->body, R"({"backend":"live"})")
        << "the failed attempt must not leak into the final response";
    EXPECT_FALSE(response->get_header_value("X-Request-Id").empty());
}

TEST_F(PipelineWithFailuresTest, UnsafeMethodStillSurfacesTheBackendFailureAndIsLogged) {
    auto client = make_client();

    // POST is never retried, so one of the two attempts must surface a 502.
    std::string failure_id;
    for (int i = 0; i < 4 && failure_id.empty(); ++i) {
        const auto response = client.Post("/users", "{}", "application/json");
        ASSERT_TRUE(response);
        if (response->status == 502) {
            failure_id = response->get_header_value("X-Request-Id");
            EXPECT_TRUE(contains(response->body, R"("reason":"backend_unreachable")"))
                << response->body;
        }
    }

    ASSERT_FALSE(failure_id.empty()) << "the dead instance never surfaced a 502";
    EXPECT_TRUE(logged(*this, log_sink().lines(), failure_id, 502));
}

TEST_F(PipelineWithFailuresTest, CircuitStillOpensForTheFailingInstance) {
    auto client = make_client();
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(client.Post("/users", "{}", "application/json"));
    }

    const gateway::CircuitBreaker* dead = breakers().find("users", 0);
    ASSERT_NE(dead, nullptr);
    EXPECT_EQ(dead->state(), CircuitState::kOpen);

    const gateway::CircuitBreaker* alive = breakers().find("users", 1);
    ASSERT_NE(alive, nullptr);
    EXPECT_EQ(alive->state(), CircuitState::kClosed);
}

}  // namespace
