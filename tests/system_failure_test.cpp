// System-level tests: the whole gateway under realistic failures, focused on
// invariants that only hold if the subsystems interact correctly.

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/metrics.hpp"
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
    for (const char* method : {"GET", "POST", "PUT", "DELETE"}) {
        router.add_route(Route{method, "/users", "users"});
    }
    return router;
}

/// Bounded wait for a condition the gateway reaches asynchronously.
bool wait_for(const std::function<bool()>& predicate,
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

// ==================================================================
// End-to-end response fidelity
// ==================================================================

class EndToEndTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.rate_limit_enabled = true;
        config.rate_limit_requests = 1000;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(EndToEndTest, SuccessfulRequestPreservesEveryPartOfTheBackendResponse) {
    backend_.set_response(201, R"({"created":"ada"})", "application/vnd.api+json",
                          httplib::Headers{{"X-Backend-Version", "7"},
                                           {"Cache-Control", "no-store"},
                                           {"X-Odd-Value", R"(a "quoted" \ value)"}});

    auto client = make_client();
    const auto response = client.Post("/users?team=eng", R"({"name":"ada"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    EXPECT_EQ(response->body, R"({"created":"ada"})");
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/vnd.api+json");
    EXPECT_EQ(response->get_header_value("X-Backend-Version"), "7");
    EXPECT_EQ(response->get_header_value("Cache-Control"), "no-store");
    EXPECT_EQ(response->get_header_value("X-Odd-Value"), R"(a "quoted" \ value)");
    EXPECT_EQ(response->get_header_value("X-Request-Id").size(), 32U);
    EXPECT_EQ(response->get_header_value("RateLimit-Limit"), "1000");

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_EQ(received.method, "POST");
    EXPECT_EQ(received.target, "/users?team=eng") << "path and query must survive";
    EXPECT_EQ(received.body, R"({"name":"ada"})");
}

TEST_F(EndToEndTest, GatewayErrorsCarryTheirBodiesHeadersAndIds) {
    auto client = make_client();

    const auto not_found = client.Get("/no-such-route");
    ASSERT_TRUE(not_found);
    EXPECT_EQ(not_found->status, 404);
    EXPECT_EQ(not_found->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(not_found->body, R"("error":"not_found")")) << not_found->body;
    EXPECT_FALSE(not_found->get_header_value("X-Request-Id").empty());

    const auto not_allowed = client.Patch("/users", "{}", "application/json");
    ASSERT_TRUE(not_allowed);
    EXPECT_EQ(not_allowed->status, 405);
    EXPECT_EQ(not_allowed->get_header_value("Allow"), "DELETE, GET, POST, PUT");
    EXPECT_TRUE(contains(not_allowed->body, R"("error":"method_not_allowed")"))
        << not_allowed->body;
}

TEST_F(EndToEndTest, BackendBodiesOfVariousShapesSurviveIntact) {
    // A large-but-reasonable body, plus an empty one.
    const std::string large(256 * 1024, 'x');
    backend_.set_response(200, large, "text/plain");
    auto client = make_client();

    const auto big = client.Get("/users");
    ASSERT_TRUE(big);
    EXPECT_EQ(big->body.size(), large.size());
    EXPECT_EQ(big->body, large);

    backend_.set_response(204, "", "text/plain");
    const auto empty = client.Get("/users");
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 204);
    EXPECT_TRUE(empty->body.empty());
}

TEST_F(EndToEndTest, LongButValidTargetsAreForwardedNotTruncated) {
    auto client = make_client();
    const std::string tail(2000, 'a');
    ASSERT_TRUE(client.Get("/users/" + tail));

    ASSERT_EQ(backend_.request_count(), 1U);
    EXPECT_EQ(backend_.received().front().path, "/users/" + tail)
        << "log truncation must not affect what is proxied";
}

// ==================================================================
// Backend failure matrix
// ==================================================================

/// One live instance plus one whose port nothing is listening on.
class FailureMatrixTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {live_.endpoint()}}};
        config.backend_timeout = std::chrono::milliseconds{300};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;  // Isolate retry behaviour.
        return config;
    }

    [[nodiscard]] unsigned failures() const {
        const gateway::CircuitBreaker* breaker = breakers().find("users", 0);
        return breaker == nullptr ? 0 : breaker->consecutive_failures();
    }

    gateway_test::TestBackend live_{"live"};
};

TEST_F(FailureMatrixTest, BackendFiveHundredIsPassedThroughNotRetried) {
    live_.set_response(500, R"({"from":"backend"})");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(response->body, R"({"from":"backend"})") << "the backend's own answer must survive";
    EXPECT_EQ(live_.request_count(), 1U) << "a 500 is an answer, not a transient failure";
    EXPECT_EQ(metrics().retry_attempts.value(), 0U);
    EXPECT_EQ(metrics().backend_failures.value({"users"}), 0U);
    EXPECT_EQ(failures(), 0U) << "a 500 must not count against the circuit";
}

TEST_F(FailureMatrixTest, BackendFourHundredIsPassedThroughUntouched) {
    live_.set_response(404, R"({"from":"backend"})");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(response->body, R"({"from":"backend"})")
        << "a backend 404 must not be replaced by the gateway's";
    EXPECT_EQ(failures(), 0U);
}

TEST_F(FailureMatrixTest, TransientBackendStatusesAreRetriedAndCounted) {
    for (const int status : {502, 503, 504}) {
        const std::size_t before = live_.request_count();
        live_.set_response(status, R"({"from":"backend"})");

        auto client = make_client();
        const auto response = client.Get("/users");

        ASSERT_TRUE(response) << "status " << status;
        EXPECT_EQ(response->status, status) << "the last transient answer is returned";
        // Only one instance exists, and an instance is never retried within one
        // request, so the retry budget cannot be spent.
        EXPECT_EQ(live_.request_count(), before + 1) << "status " << status;
    }
    EXPECT_EQ(metrics().backend_failures.value({"users"}), 3U);
    EXPECT_EQ(failures(), 3U) << "each transient answer counts against the circuit";
}

TEST_F(FailureMatrixTest, SlowBackendTimesOutPromptlyAndReturns504) {
    live_.set_delay(std::chrono::milliseconds{3000});

    auto client = make_client();
    client.set_read_timeout(20, 0);
    const auto started = std::chrono::steady_clock::now();
    const auto response = client.Get("/users");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(response) << "the gateway must answer rather than hang";
    EXPECT_EQ(response->status, 504);
    EXPECT_TRUE(contains(response->body, R"("reason":"backend_timeout")")) << response->body;
    // One instance, 300ms timeout, no retry possible: generous ceiling for CI.
    EXPECT_LT(elapsed, std::chrono::seconds{10}) << "the timeout must bound the wait";
    EXPECT_EQ(metrics().backend_timeouts.value({"users"}), 1U);
}

/// A dead instance beside a live one, for the connection-refused path.
class DeadInstanceTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_endpoint_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint dead_endpoint_ = [] {
        const gateway_test::TestBackend probe;  // frees its port when destroyed
        return probe.endpoint();
    }();
};

TEST_F(DeadInstanceTest, RefusedConnectionIsRetriedOntoTheLiveInstance) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, R"({"backend":"live"})");
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 2U);
    EXPECT_EQ(metrics().retry_attempts.value(), 1U);
    EXPECT_EQ(metrics().retried_requests.value(), 1U);
    EXPECT_EQ(metrics().requests.total(), 1U) << "still one client request";
}

TEST_F(DeadInstanceTest, FailedAttemptContributesNothingToTheFinalResponse) {
    live_.set_response(200, R"({"from":"live"})", "application/json",
                       httplib::Headers{{"X-Good", "yes"}});

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, R"({"from":"live"})");
    EXPECT_EQ(response->get_header_value("X-Good"), "yes");
    EXPECT_FALSE(response->get_header_value("X-Request-Id").empty());
}

TEST_F(DeadInstanceTest, UnsafeMethodsAreNeverReplayedOntoAnotherInstance) {
    auto client = make_client();
    std::size_t failures = 0;
    for (int i = 0; i < 6; ++i) {
        const auto response = client.Post("/users", "{}", "application/json");
        ASSERT_TRUE(response);
        failures += response->status == 502 ? 1 : 0;
    }

    EXPECT_GT(failures, 0U) << "the dead instance must surface, not be retried away";
    EXPECT_EQ(metrics().retry_attempts.value(), 0U) << "POST is never retried";
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 6U) << "one attempt per request";
}

// ==================================================================
// Retry invariants
// ==================================================================

/// Four instances, three of them dead, so the retry budget is the only thing
/// that can stop the search.
class RetryBudgetTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_a_, dead_b_, dead_c_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    static gateway::BackendEndpoint closed_port() {
        const gateway_test::TestBackend probe;
        return probe.endpoint();
    }
    const gateway::BackendEndpoint dead_a_ = closed_port();
    const gateway::BackendEndpoint dead_b_ = closed_port();
    const gateway::BackendEndpoint dead_c_ = closed_port();
};

TEST_F(RetryBudgetTest, AtMostMaxRetriesPlusOneBackendAttemptsPerClientRequest) {
    auto client = make_client();
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    // Eight client requests, at most two attempts each.
    EXPECT_LE(metrics().backend_requests.value({"users"}), 16U);
    EXPECT_LE(metrics().retry_attempts.value(), 8U);
    EXPECT_EQ(metrics().requests.total(), 8U) << "retries must not inflate client requests";
}

TEST_F(RetryBudgetTest, ExhaustedBudgetReturnsAGatewayErrorRatherThanLooping) {
    auto client = make_client();
    client.set_read_timeout(15, 0);

    const auto started = std::chrono::steady_clock::now();
    std::size_t answered = 0;
    for (int i = 0; i < 8; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response) << "every request must be answered";
        answered += (response->status == 200 || response->status == 502) ? 1 : 0;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(answered, 8U) << "only success or a bounded 502 are possible here";
    EXPECT_LT(elapsed, std::chrono::seconds{10}) << "no retry explosion";
}

// ==================================================================
// Circuit breaker driven through real traffic
// ==================================================================

class CircuitLifecycleTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {flaky_.endpoint()}}};
        config.max_retries = 0;
        config.circuit_failure_threshold = 2;
        config.circuit_cooldown = std::chrono::milliseconds{300};
        return config;
    }

    [[nodiscard]] CircuitState state() const {
        const gateway::CircuitBreaker* breaker = breakers().find("users", 0);
        return breaker == nullptr ? CircuitState::kClosed : breaker->state();
    }

    gateway_test::TestBackend flaky_{"flaky"};
};

TEST_F(CircuitLifecycleTest, ClosedToOpenToHalfOpenToClosedThroughRealRequests) {
    auto client = make_client();
    ASSERT_EQ(state(), CircuitState::kClosed);

    // 1. Repeated transient failures open the circuit.
    flaky_.set_response(503, R"({"from":"flaky"})");
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }
    ASSERT_EQ(state(), CircuitState::kOpen);

    // 2. While open, traffic must not reach the backend at all.
    const std::size_t during_open = flaky_.request_count();
    for (int i = 0; i < 5; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 503);
        EXPECT_TRUE(contains(response->body, R"("reason":"circuit_open")")) << response->body;
    }
    EXPECT_EQ(flaky_.request_count(), during_open) << "an open circuit must send nothing";

    // 3. After the cooldown a probe is admitted and, succeeding, closes it.
    flaky_.set_response(200, R"({"backend":"flaky"})");
    ASSERT_TRUE(wait_for([this, &client] {
        (void)client.Get("/users");
        return state() == CircuitState::kClosed;
    })) << "the circuit never recovered";

    const auto served = client.Get("/users");
    ASSERT_TRUE(served);
    EXPECT_EQ(served->status, 200);
}

TEST_F(CircuitLifecycleTest, FailedProbeReopensTheCircuit) {
    auto client = make_client();
    flaky_.set_response(503, R"({"from":"flaky"})");
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }
    ASSERT_EQ(state(), CircuitState::kOpen);

    // Still failing, so the probe after the cooldown must reopen it.
    const std::size_t before = flaky_.request_count();
    ASSERT_TRUE(wait_for([this, &client, before] {
        (void)client.Get("/users");
        return flaky_.request_count() > before;
    })) << "no probe was ever admitted";

    EXPECT_EQ(state(), CircuitState::kOpen);
}

TEST_F(CircuitLifecycleTest, ConcurrentTrafficDuringCooldownAdmitsOneProbeAtATime) {
    auto setup = make_client();
    flaky_.set_response(503, R"({"from":"flaky"})");
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(setup.Get("/users"));
    }
    ASSERT_EQ(state(), CircuitState::kOpen);

    const std::size_t before = flaky_.request_count();
    // Wait out the cooldown, then let several threads race for the probe.
    std::this_thread::sleep_for(std::chrono::milliseconds{450});

    constexpr int kThreads = 8;
    std::atomic<int> answered{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, &answered] {
            auto client = make_client();
            if (client.Get("/users")) {
                ++answered;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(answered.load(), kThreads) << "every racing request must be answered";
    EXPECT_LE(flaky_.request_count() - before, 1U)
        << "at most one probe may be admitted per cooldown window";
}

// ==================================================================
// Health and circuit are independent
// ==================================================================

class HealthAndCircuitTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {first_.endpoint(), second_.endpoint()}}};
        config.health_check_interval = std::chrono::milliseconds{40};
        config.backend_timeout = std::chrono::seconds{2};
        config.max_retries = 0;
        config.circuit_failure_threshold = 2;
        config.circuit_cooldown = std::chrono::hours{1};
        return config;
    }

    [[nodiscard]] CircuitState circuit(std::size_t index) const {
        const gateway::CircuitBreaker* breaker = breakers().find("users", index);
        return breaker == nullptr ? CircuitState::kClosed : breaker->state();
    }

    gateway_test::TestBackend first_{"first"};
    gateway_test::TestBackend second_{"second"};
};

TEST_F(HealthAndCircuitTest, NeitherMechanismOverwritesTheOther) {
    auto client = make_client();

    // Instance 0 fails its health probe while still serving traffic fine.
    first_.set_health_status(500);
    ASSERT_TRUE(wait_for_health([this] { return !health().is_healthy("users", 0); }));
    EXPECT_EQ(circuit(0), CircuitState::kClosed)
        << "a health failure must not open a circuit by itself";

    // All traffic now goes to instance 1; make it fail transiently until its
    // circuit opens, while its health stays good.
    second_.set_response(503, R"({"from":"second"})");
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }
    EXPECT_EQ(circuit(1), CircuitState::kOpen);
    EXPECT_TRUE(health().is_healthy("users", 1))
        << "request failures must not mark an instance unhealthy";

    // Nothing is selectable now: 0 is unhealthy, 1 has an open circuit.
    const auto blocked = client.Get("/users");
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked->status, 503);
    EXPECT_TRUE(contains(blocked->body, R"("reason":"circuit_open")")) << blocked->body;

    // Health recovery on instance 0 restores service without touching circuits.
    first_.set_health_status(200);
    ASSERT_TRUE(wait_for_health([this] { return health().is_healthy("users", 0); }));
    const auto served = client.Get("/users");
    ASSERT_TRUE(served);
    EXPECT_EQ(served->status, 200);
    EXPECT_EQ(served->body, R"({"backend":"first"})");
    EXPECT_EQ(circuit(1), CircuitState::kOpen) << "circuit state survived a health recovery";
}

// ==================================================================
// Rate limiting interactions
// ==================================================================

/// A dead instance beside a live one so a retry happens, with a tight limit.
class RateLimitInteractionTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_endpoint_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        config.rate_limit_enabled = true;
        config.rate_limit_requests = 3;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint dead_endpoint_ = [] {
        const gateway_test::TestBackend probe;
        return probe.endpoint();
    }();
};

TEST_F(RateLimitInteractionTest, RetriesCostOneRateLimitDecisionNotOnePerAttempt) {
    auto client = make_client();

    // Three client requests are allowed. Each may make two backend attempts.
    for (int i = 0; i < 3; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response) << "request " << i;
        EXPECT_EQ(response->status, 200) << "request " << i << " should have retried to live";
    }

    const auto rejected = client.Get("/users");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 429) << "the fourth request exhausts the allowance";

    EXPECT_EQ(metrics().requests.value({"GET", "200"}), 3U);
    EXPECT_GT(metrics().backend_requests.value({"users"}), 3U)
        << "retries produced more backend attempts than client requests";
    EXPECT_EQ(metrics().rate_limited.value(), 1U)
        << "only the client request consumes allowance, never the retry";
}

TEST_F(RateLimitInteractionTest, ThrottledRequestsTouchNoBackendMachineryAtAll) {
    auto client = make_client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client.Get("/users"));
    }

    const std::size_t attempts_before = metrics().backend_requests.value({"users"});
    const std::uint64_t retries_before = metrics().retry_attempts.value();
    const gateway::CircuitBreaker* dead = breakers().find("users", 0);
    ASSERT_NE(dead, nullptr);
    const unsigned circuit_failures_before = dead->consecutive_failures();
    const std::size_t live_before = live_.request_count();

    for (int i = 0; i < 15; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 429);
    }

    EXPECT_EQ(metrics().backend_requests.value({"users"}), attempts_before);
    EXPECT_EQ(metrics().retry_attempts.value(), retries_before);
    EXPECT_EQ(dead->consecutive_failures(), circuit_failures_before);
    EXPECT_EQ(live_.request_count(), live_before);
}

// ==================================================================
// Load balancing under failure
// ==================================================================

class AllBackendsDeadTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {dead_a_, dead_b_, dead_c_}}};
        // Budget of two against three instances, so the budget is what stops the
        // search and the attempt count per request is exactly max_retries + 1.
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    static gateway::BackendEndpoint closed_port() {
        const gateway_test::TestBackend probe;
        return probe.endpoint();
    }
    const gateway::BackendEndpoint dead_a_ = closed_port();
    const gateway::BackendEndpoint dead_b_ = closed_port();
    const gateway::BackendEndpoint dead_c_ = closed_port();
};

TEST_F(AllBackendsDeadTest, EveryRequestTerminatesPromptlyWithABoundedNumberOfAttempts) {
    auto client = make_client();
    client.set_read_timeout(15, 0);

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response) << "request " << i << " must be answered, not hang";
        EXPECT_EQ(response->status, 502);
        EXPECT_TRUE(contains(response->body, R"("reason":"backend_unreachable")")) << response->body;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds{10});
    // Exactly max_retries + 1 attempts per request: fewer would mean the budget
    // is not being spent, more would mean it is not being respected.
    EXPECT_EQ(metrics().backend_requests.value({"users"}), 10U);
    EXPECT_EQ(metrics().retry_attempts.value(), 5U);
    EXPECT_EQ(metrics().retried_requests.value(), 5U);
    EXPECT_EQ(metrics().requests.value({"GET", "502"}), 5U);
    EXPECT_EQ(metrics().requests_in_flight.value(), 0);
}

}  // namespace
