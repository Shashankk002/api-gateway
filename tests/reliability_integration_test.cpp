// End-to-end reliability tests: retries across real backend servers and
// circuit breakers opening, blocking and recovering through a real gateway.

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

Router users_router() {
    Router router;
    for (const char* method : {"GET", "POST", "PUT", "OPTIONS"}) {
        router.add_route(Route{method, "/users", "users"});
    }
    return router;
}

/// Two instances, health checking off so only retry and circuit behaviour is in
/// play, and a circuit threshold high enough not to trip unless a test says so.
class RetryTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint()}}};
        config.backend_timeout = std::chrono::milliseconds{300};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
};

TEST_F(RetryTest, SuccessfulFirstAttemptDoesNotRetry) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(a_.request_count() + b_.request_count(), 1U) << "one attempt only";
}

TEST_F(RetryTest, TransientFailureRetriesOntoTheOtherInstance) {
    a_.set_response(503, R"({"from":"a"})");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200) << "the retry should have succeeded on b";
    EXPECT_EQ(response->body, R"({"backend":"b"})");
    EXPECT_EQ(a_.request_count(), 1U);
    EXPECT_EQ(b_.request_count(), 1U);
}

TEST_F(RetryTest, BackendReported502IsTreatedAsTransient) {
    a_.set_response(502, R"({"from":"a"})");

    auto client = make_client();
    const auto response = client.Get("/users/42");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    ASSERT_EQ(b_.request_count(), 1U);
    EXPECT_EQ(b_.received().front().path, "/users/42") << "the retry keeps the original path";
}

TEST_F(RetryTest, BackendTimeoutIsRetried) {
    a_.set_delay(std::chrono::milliseconds{1200});  // Well past the 300ms timeout.

    auto client = make_client();
    client.set_read_timeout(10, 0);
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, R"({"backend":"b"})");
}

TEST_F(RetryTest, RetryCountIsBoundedAndFinalFailureIsReported) {
    a_.set_response(503, R"({"from":"a"})");
    b_.set_response(503, R"({"from":"b"})");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    // Both instances answered transiently; the backend's own last response is
    // more informative than a fabricated gateway error.
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(a_.request_count(), 1U) << "an instance is never retried within one request";
    EXPECT_EQ(b_.request_count(), 1U);
}

TEST_F(RetryTest, AllInstancesTimingOutReturns504) {
    a_.set_delay(std::chrono::milliseconds{1200});
    b_.set_delay(std::chrono::milliseconds{1200});

    auto client = make_client();
    client.set_read_timeout(10, 0);
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 504);
    EXPECT_TRUE(contains(response->body, R"("reason":"backend_timeout")")) << response->body;
}

TEST_F(RetryTest, NonTransientStatusIsNotRetried) {
    for (const int status : {400, 404, 418, 500, 501}) {
        a_.set_response(status, R"({"from":"a"})");
        b_.set_response(status, R"({"from":"b"})");
        const std::size_t before = a_.request_count() + b_.request_count();

        auto client = make_client();
        const auto response = client.Get("/users");

        ASSERT_TRUE(response) << "status under test: " << status;
        EXPECT_EQ(response->status, status);
        EXPECT_EQ(a_.request_count() + b_.request_count(), before + 1)
            << "status " << status << " must not be retried";
    }
}

TEST_F(RetryTest, UnsafeMethodsAreNotRetried) {
    a_.set_response(503, R"({"from":"a"})");
    b_.set_response(503, R"({"from":"b"})");

    auto client = make_client();
    for (const auto& response : {client.Post("/users", "{}", "application/json"),
                                 client.Put("/users", "{}", "application/json")}) {
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 503);
    }
    EXPECT_EQ(a_.request_count() + b_.request_count(), 2U) << "one attempt per unsafe request";
}

TEST_F(RetryTest, SafeMethodsOtherThanGetAreRetried) {
    a_.set_response(503, R"({"from":"a"})");

    auto client = make_client();
    const auto response = client.Options("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(a_.request_count(), 1U);
    EXPECT_EQ(b_.request_count(), 1U);
}

TEST_F(RetryTest, TargetAndHeadersSurviveARetry) {
    a_.set_response(503, R"({"from":"a"})");

    auto client = make_client();
    const auto response =
        client.Get("/users?page=2", httplib::Headers{{"X-Request-Id", "req-9"}});

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    ASSERT_EQ(b_.request_count(), 1U);
    const auto received = b_.received().front();
    EXPECT_EQ(received.target, "/users?page=2");
    EXPECT_EQ(received.header("X-Request-Id"), "req-9");
}

TEST_F(RetryTest, RetriedResponseDoesNotLeakHeadersFromTheFailedAttempt) {
    a_.set_response(503, R"({"from":"a"})", "application/json",
                    httplib::Headers{{"X-Failed-Attempt", "yes"}});
    b_.set_response(200, R"({"from":"b"})", "application/json",
                    httplib::Headers{{"X-Good-Attempt", "yes"}});

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("X-Good-Attempt"), "yes");
    EXPECT_TRUE(response->get_header_value("X-Failed-Attempt").empty())
        << "the abandoned attempt must not contribute headers";
    EXPECT_EQ(response->body, R"({"from":"b"})");
}

/// A configured instance whose port nothing is listening on, alongside a live
/// one, to exercise the connection-failure retry path rather than a status code.
class DeadInstanceRetryTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {closed_endpoint_, live_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint closed_endpoint_ = [] {
        const gateway_test::TestBackend probe;  // frees its port when destroyed
        return probe.endpoint();
    }();
};

TEST_F(DeadInstanceRetryTest, ConnectionFailureRetriesOntoTheLiveInstance) {
    auto client = make_client();
    const auto response = client.Get("/users/7");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200) << "the retry should have reached the live instance";
    EXPECT_EQ(response->body, R"({"backend":"live"})");
    ASSERT_EQ(live_.request_count(), 1U);
    EXPECT_EQ(live_.received().front().path, "/users/7");
}

TEST_F(DeadInstanceRetryTest, UnsafeMethodIsNotRetriedPastADeadInstance) {
    auto client = make_client();

    // Whichever instance is picked first, POST is never replayed elsewhere.
    const auto first = client.Post("/users", "{}", "application/json");
    const auto second = client.Post("/users", "{}", "application/json");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    const bool one_failed = first->status == 502 || second->status == 502;
    EXPECT_TRUE(one_failed) << "the dead instance must surface as a 502, not be retried away";
    EXPECT_EQ(live_.request_count(), 1U);
}

/// Retries disabled, so the gateway makes exactly one attempt per request.
class RetriesDisabledTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint()}}};
        config.max_retries = 0;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
};

TEST_F(RetriesDisabledTest, TransientFailureIsReturnedWithoutRetrying) {
    a_.set_response(503, R"({"from":"a"})");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(a_.request_count(), 1U);
    EXPECT_EQ(b_.request_count(), 0U);
}

/// Three instances with a low circuit threshold, so a failing instance is
/// removed from the rotation after a couple of requests.
class CircuitIntegrationTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint(), c_.endpoint()}}};
        config.max_retries = 1;
        config.circuit_failure_threshold = 2;
        config.circuit_cooldown = std::chrono::milliseconds{250};
        return config;
    }

    [[nodiscard]] CircuitState circuit_state(std::size_t index) const {
        const gateway::CircuitBreaker* breaker = breakers().find("users", index);
        return breaker == nullptr ? CircuitState::kClosed : breaker->state();
    }

    /// Sends requests until instance `index`'s circuit opens, bounded so a
    /// broken implementation fails instead of looping.
    [[nodiscard]] bool drive_until_open(std::size_t index, int max_requests = 40) {
        auto client = make_client();
        for (int i = 0; i < max_requests; ++i) {
            if (circuit_state(index) == CircuitState::kOpen) {
                return true;
            }
            (void)client.Get("/users");
        }
        return circuit_state(index) == CircuitState::kOpen;
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
    gateway_test::TestBackend c_{"c"};
};

TEST_F(CircuitIntegrationTest, HealthyTrafficLeavesEveryCircuitClosed) {
    auto client = make_client();
    for (int i = 0; i < 9; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
    }

    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_EQ(circuit_state(index), CircuitState::kClosed) << "index " << index;
    }
    // Round robin is untouched when nothing fails.
    EXPECT_EQ(a_.request_count(), 3U);
    EXPECT_EQ(b_.request_count(), 3U);
    EXPECT_EQ(c_.request_count(), 3U);
}

TEST_F(CircuitIntegrationTest, RepeatedlyFailingInstanceOpensItsCircuitAndLosesTraffic) {
    b_.set_response(503, R"({"from":"b"})");

    ASSERT_TRUE(drive_until_open(1)) << "b's circuit never opened";
    EXPECT_EQ(circuit_state(0), CircuitState::kClosed);
    EXPECT_EQ(circuit_state(2), CircuitState::kClosed);

    const std::size_t b_before = b_.request_count();
    auto client = make_client();
    for (int i = 0; i < 8; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
    }
    EXPECT_EQ(b_.request_count(), b_before) << "an open circuit must receive no traffic";
    EXPECT_GT(a_.request_count(), 0U);
    EXPECT_GT(c_.request_count(), 0U);
}

TEST_F(CircuitIntegrationTest, CooldownAllowsARecoveryProbeThatClosesTheCircuit) {
    b_.set_response(503, R"({"from":"b"})");
    ASSERT_TRUE(drive_until_open(1));

    b_.set_response(200, R"({"backend":"b"})");
    // The cooldown is a wall-clock deadline, so waiting past it is the only way
    // to reach the probe; the wait is generous relative to the 250ms cooldown.
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    auto client = make_client();
    for (int i = 0; i < 6 && circuit_state(1) != CircuitState::kClosed; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
    }

    EXPECT_EQ(circuit_state(1), CircuitState::kClosed) << "a good probe must close the circuit";

    const std::size_t b_before = b_.request_count();
    for (int i = 0; i < 6; ++i) {
        (void)client.Get("/users");
    }
    EXPECT_GT(b_.request_count(), b_before) << "a recovered instance rejoins the rotation";
}

TEST_F(CircuitIntegrationTest, FailedRecoveryProbeReopensTheCircuit) {
    b_.set_response(503, R"({"from":"b"})");
    ASSERT_TRUE(drive_until_open(1));

    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    auto client = make_client();
    for (int i = 0; i < 6; ++i) {
        (void)client.Get("/users");  // b is still failing, so the probe fails.
    }

    EXPECT_EQ(circuit_state(1), CircuitState::kOpen);
}

TEST_F(CircuitIntegrationTest, EveryInstanceOpenReturnsServiceUnavailable) {
    a_.set_response(503, R"({"from":"a"})");
    b_.set_response(503, R"({"from":"b"})");
    c_.set_response(503, R"({"from":"c"})");

    auto client = make_client();
    for (int i = 0; i < 30; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        if (response->status == 503 && contains(response->body, R"("reason":"circuit_open")")) {
            SUCCEED();
            return;
        }
    }
    FAIL() << "the gateway never reported every circuit as open";
}

TEST_F(CircuitIntegrationTest, GatewayHealthAndRoutingAreUnaffectedByOpenCircuits) {
    b_.set_response(503, R"({"from":"b"})");
    ASSERT_TRUE(drive_until_open(1));

    auto client = make_client();
    const auto health = client.Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_TRUE(contains(health->body, R"("status":"healthy")")) << health->body;

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);

    const auto wrong_method = client.Delete("/users");
    ASSERT_TRUE(wrong_method);
    EXPECT_EQ(wrong_method->status, 405);
}

TEST_F(CircuitIntegrationTest, ConcurrentRequestsDriveCircuitStateSafely) {
    b_.set_response(503, R"({"from":"b"})");

    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;
    std::atomic<int> unanswered{0};

    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([this, &unanswered] {
            auto client = make_client();
            for (int i = 0; i < kPerThread; ++i) {
                if (!client.Get("/users")) {
                    ++unanswered;
                }
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }

    EXPECT_EQ(unanswered.load(), 0) << "the gateway must answer every request";
    EXPECT_EQ(circuit_state(1), CircuitState::kOpen) << "the failing instance was isolated";
    EXPECT_EQ(circuit_state(0), CircuitState::kClosed);
    EXPECT_EQ(circuit_state(2), CircuitState::kClosed);
}

/// Retries and health checking together: an instance the health checker has
/// already removed must not be picked, and circuits stay independent of health.
class ReliabilityWithHealthChecksTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint()}}};
        config.health_check_interval = std::chrono::milliseconds{50};
        config.backend_timeout = std::chrono::milliseconds{2000};
        config.max_retries = 1;
        config.circuit_failure_threshold = 1000;
        return config;
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
};

TEST_F(ReliabilityWithHealthChecksTest, UnhealthyInstanceIsExcludedWhileRetriesStillWork) {
    a_.set_health_status(500);
    ASSERT_TRUE(wait_for_health([this] { return !health().is_healthy("users", 0); }));

    auto client = make_client();
    for (int i = 0; i < 6; ++i) {
        const auto response = client.Get("/users");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_EQ(response->body, R"({"backend":"b"})");
    }
    EXPECT_EQ(a_.request_count(), 0U) << "health checks still gate selection";

    // Health state is untouched by circuit-breaker activity.
    EXPECT_TRUE(health().is_healthy("users", 1));
}

}  // namespace
