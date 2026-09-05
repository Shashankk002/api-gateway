// Lifecycle and concurrency stress. These assert invariants that must hold
// regardless of scheduling, rather than exact interleavings.

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/metrics.hpp"
#include "gateway/middleware.hpp"
#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::Route;
using gateway::Router;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

Router users_router() {
    Router router;
    for (const char* method : {"GET", "POST"}) {
        router.add_route(Route{method, "/users", "users"});
    }
    return router;
}

// ==================================================================
// Lifecycle
// ==================================================================

using gateway_test::ScopedGateway;

class LifecycleTest : public ::testing::Test {
protected:
    gateway::ServerConfig config_for(const gateway_test::TestBackend& backend) {
        gateway::ServerConfig config;
        config.host = "127.0.0.1";
        config.backends = {{"users", {backend.endpoint()}}};
        // Health checking on, so the checker's thread is created and joined on
        // every cycle; that is the part most likely to leak or hang.
        config.health_check_interval = std::chrono::milliseconds{20};
        config.backend_timeout = std::chrono::seconds{2};
        return config;
    }
};

TEST_F(LifecycleTest, RepeatedStartAndStopCyclesStayCleanAndIndependent) {
    gateway_test::TestBackend backend{"users"};

    for (int cycle = 0; cycle < 6; ++cycle) {
        auto sink = std::make_shared<gateway::CapturingLogSink>();
        ScopedGateway gateway(config_for(backend), users_router(), sink);
        ASSERT_TRUE(gateway.ok()) << "cycle " << cycle << " failed to start";

        auto client = gateway.client();
        for (int i = 0; i < 4; ++i) {
            const auto response = client.Get("/users");
            ASSERT_TRUE(response) << "cycle " << cycle;
            EXPECT_EQ(response->status, 200);
        }

        // Each gateway keeps its own state; nothing leaks in from the last one.
        EXPECT_EQ(gateway.server().metrics().requests.total(), 4U) << "cycle " << cycle;
        EXPECT_EQ(gateway.server().metrics().requests_in_flight.value(), 0) << "cycle " << cycle;
        EXPECT_EQ(sink->count(), 4U) << "cycle " << cycle;
    }

    EXPECT_EQ(backend.request_count(), 24U) << "every cycle's traffic reached the backend";
}

TEST_F(LifecycleTest, ShutdownIsPromptEvenWithHealthCheckingRunning) {
    gateway_test::TestBackend backend{"users"};

    for (int cycle = 0; cycle < 4; ++cycle) {
        const auto started = std::chrono::steady_clock::now();
        {
            auto sink = std::make_shared<gateway::CapturingLogSink>();
            ScopedGateway gateway(config_for(backend), users_router(), sink);
            ASSERT_TRUE(gateway.ok());
            ASSERT_TRUE(gateway.client().Get("/users"));
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        // The interruptible sleep means shutdown waits only for an in-flight
        // probe, not for a whole interval.
        EXPECT_LT(elapsed, std::chrono::seconds{5}) << "cycle " << cycle << " was slow to stop";
    }
}

TEST_F(LifecycleTest, GatewaysRunningSideBySideKeepSeparateState) {
    gateway_test::TestBackend backend_a{"a"};
    gateway_test::TestBackend backend_b{"b"};

    auto sink_a = std::make_shared<gateway::CapturingLogSink>();
    auto sink_b = std::make_shared<gateway::CapturingLogSink>();
    ScopedGateway first(config_for(backend_a), users_router(), sink_a);
    ScopedGateway second(config_for(backend_b), users_router(), sink_b);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    auto client_a = first.client();
    auto client_b = second.client();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(client_a.Get("/users"));
    }
    ASSERT_TRUE(client_b.Get("/users"));

    EXPECT_EQ(first.server().metrics().requests.total(), 3U);
    EXPECT_EQ(second.server().metrics().requests.total(), 1U);
    EXPECT_EQ(backend_a.request_count(), 3U);
    EXPECT_EQ(backend_b.request_count(), 1U);
}

// ==================================================================
// Request id invariants under load
// ==================================================================

/// A dead instance beside a live one, so every request retries.
class RequestIdUnderRetryTest : public gateway_test::GatewayServerTestBase {
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
        const gateway_test::TestBackend probe;
        return probe.endpoint();
    }();
};

TEST_F(RequestIdUnderRetryTest, OneIdPerClientRequestEvenWhenItRetries) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200) << "the request should have retried onto the live instance";
    ASSERT_EQ(metrics().retry_attempts.value(), 1U) << "a retry must actually have happened";

    const std::string id = response->get_header_value("X-Request-Id");
    ASSERT_EQ(id.size(), 32U);

    // Exactly one log line, carrying that id: a retry must not mint a new one.
    const std::vector<std::string> lines = log_sink().lines();
    ASSERT_EQ(lines.size(), 1U) << "one client request must produce one log line";
    EXPECT_TRUE(contains(lines.front(), id)) << lines.front();
}

TEST_F(RequestIdUnderRetryTest, ConcurrentRequestsNeverShareAnId) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 30;

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
        for (const std::string& id : thread_ids) {
            EXPECT_EQ(id.size(), 32U);
            ids.insert(id);
        }
    }

    EXPECT_EQ(total, static_cast<std::size_t>(kThreads) * kPerThread);
    EXPECT_EQ(ids.size(), total) << "ids collided under concurrency";
    EXPECT_EQ(log_sink().count(), total) << "one log line per request";
}

// ==================================================================
// Mixed concurrency stress
// ==================================================================

/// Everything at once: a healthy instance, a dead one, a flaky one, rate
/// limiting, retries, circuits and health checking.
class MixedStressTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {healthy_.endpoint(), flaky_.endpoint(), dead_endpoint_}}};
        config.backend_timeout = std::chrono::milliseconds{500};
        config.max_retries = 1;
        config.circuit_failure_threshold = 3;
        config.circuit_cooldown = std::chrono::milliseconds{100};
        config.health_check_interval = std::chrono::milliseconds{30};
        config.rate_limit_enabled = true;
        config.rate_limit_requests = 200;
        config.rate_limit_window = std::chrono::hours{1};
        return config;
    }

    gateway_test::TestBackend healthy_{"healthy"};
    gateway_test::TestBackend flaky_{"flaky"};
    const gateway::BackendEndpoint dead_endpoint_ = [] {
        const gateway_test::TestBackend probe;
        return probe.endpoint();
    }();
};

TEST_F(MixedStressTest, EveryRequestIsAnsweredAndAccountingStaysConsistent) {
    constexpr int kClients = 8;
    constexpr int kPerClient = 40;
    constexpr int kTotal = kClients * kPerClient;

    std::atomic<int> answered{0};
    std::atomic<int> unanswered{0};
    std::atomic<bool> stop_churn{false};

    // Churn the flaky instance's request and health behaviour throughout, so
    // circuits open and close and health flips while traffic is in flight.
    std::thread churn([this, &stop_churn] {
        bool bad = false;
        while (!stop_churn.load(std::memory_order_relaxed)) {
            flaky_.set_response(bad ? 503 : 200, R"({"backend":"flaky"})");
            flaky_.set_health_status(bad ? 500 : 200);
            bad = !bad;
            std::this_thread::sleep_for(std::chrono::milliseconds{15});
        }
        flaky_.set_response(200, R"({"backend":"flaky"})");
        flaky_.set_health_status(200);
    });

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int c = 0; c < kClients; ++c) {
        clients.emplace_back([this, &answered, &unanswered] {
            auto client = make_client();
            client.set_read_timeout(10, 0);
            for (int i = 0; i < kPerClient; ++i) {
                const auto response = client.Get("/users");
                if (response) {
                    ++answered;
                } else {
                    ++unanswered;
                }
            }
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }
    stop_churn.store(true, std::memory_order_relaxed);
    churn.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    // Properties, not schedules.
    EXPECT_EQ(unanswered.load(), 0) << "no request may hang or be dropped";
    EXPECT_EQ(answered.load(), kTotal);
    EXPECT_LT(elapsed, std::chrono::seconds{60}) << "no retry explosion or deadlock";

    EXPECT_EQ(metrics().requests.total(), static_cast<std::uint64_t>(kTotal))
        << "one client request counts exactly once";
    EXPECT_EQ(metrics().request_duration.count(), static_cast<std::uint64_t>(kTotal));
    EXPECT_EQ(metrics().requests_in_flight.value(), 0) << "in-flight must drain";

    // Retry accounting must stay internally consistent.
    const std::uint64_t attempts = metrics().backend_requests.value({"users"});
    const std::uint64_t retries = metrics().retry_attempts.value();
    const std::uint64_t retried = metrics().retried_requests.value();
    const std::uint64_t rejected = metrics().rate_limited.value();
    EXPECT_LE(attempts, static_cast<std::uint64_t>(kTotal) * 2U)
        << "at most max_retries + 1 attempts per request";
    EXPECT_LE(retried, retries) << "a retried request implies at least one retry attempt";
    EXPECT_EQ(attempts, static_cast<std::uint64_t>(kTotal) - rejected + retries)
        << "attempts must equal admitted requests plus retries";

    // Attempts against the dead instance are real forwards that never reach a
    // server, so the live backends account for at most the counted attempts.
    // (An equality here would only hold while health checking happens to have
    // already excluded the dead instance, which is a scheduling accident.)
    EXPECT_LE(healthy_.request_count() + flaky_.request_count(), attempts);
    EXPECT_GT(healthy_.request_count(), 0U) << "the always-healthy instance must carry traffic";
}

}  // namespace
