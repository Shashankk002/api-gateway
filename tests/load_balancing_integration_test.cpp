// End-to-end load-balancing tests: several real backend servers registered as
// instances of one service, driven through a real gateway, asserting on which
// backend each request actually reached.

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

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

Router users_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"POST", "/users", "users"});
    return router;
}

/// One service, three instances.
class RoundRobinTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {a_.endpoint(), b_.endpoint(), c_.endpoint()}}};
        return config;
    }

    /// The backend name reported by each of `count` sequential GET /users calls.
    std::vector<std::string> backends_hit(std::size_t count) {
        auto client = make_client();
        std::vector<std::string> names;
        for (std::size_t i = 0; i < count; ++i) {
            const auto response = client.Get("/users");
            if (!response || response->status != 200) {
                names.emplace_back("<failed>");
                continue;
            }
            names.push_back(response->body);
        }
        return names;
    }

    gateway_test::TestBackend a_{"a"};
    gateway_test::TestBackend b_{"b"};
    gateway_test::TestBackend c_{"c"};
};

TEST_F(RoundRobinTest, ThreeInstancesRotateInOrderAndWrapAround) {
    const std::vector<std::string> expected{
        R"({"backend":"a"})", R"({"backend":"b"})", R"({"backend":"c"})",
        R"({"backend":"a"})", R"({"backend":"b"})", R"({"backend":"c"})",
        R"({"backend":"a"})",
    };
    EXPECT_EQ(backends_hit(7), expected);

    EXPECT_EQ(a_.request_count(), 3U);
    EXPECT_EQ(b_.request_count(), 2U);
    EXPECT_EQ(c_.request_count(), 2U);
}

TEST_F(RoundRobinTest, EveryInstanceReceivesAnEqualShareOverManyRotations) {
    ASSERT_EQ(backends_hit(30).size(), 30U);

    EXPECT_EQ(a_.request_count(), 10U);
    EXPECT_EQ(b_.request_count(), 10U);
    EXPECT_EQ(c_.request_count(), 10U);
}

TEST_F(RoundRobinTest, ForwardingStillWorksThroughTheSelectedInstance) {
    b_.set_response(201, R"({"created":true})", "application/json",
                    httplib::Headers{{"X-Backend-Version", "7"}});

    auto client = make_client();
    ASSERT_TRUE(client.Get("/users"));  // consumes instance a

    const std::string body = R"({"name":"ada"})";
    const auto response = client.Post("/users", body, "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    EXPECT_EQ(response->body, R"({"created":true})");
    EXPECT_EQ(response->get_header_value("X-Backend-Version"), "7");

    ASSERT_EQ(b_.request_count(), 1U);
    const auto received = b_.received().front();
    EXPECT_EQ(received.method, "POST");
    EXPECT_EQ(received.path, "/users");
    EXPECT_EQ(received.body, body);
}

TEST_F(RoundRobinTest, HealthNeverReachesAnyInstance) {
    auto client = make_client();
    for (int i = 0; i < 5; ++i) {
        const auto response = client.Get("/health");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    }

    EXPECT_EQ(a_.request_count(), 0U);
    EXPECT_EQ(b_.request_count(), 0U);
    EXPECT_EQ(c_.request_count(), 0U);
}

TEST_F(RoundRobinTest, RoutingErrorsStillPrecedeSelection) {
    auto client = make_client();

    const auto unknown = client.Get("/nothing-here");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);

    const auto wrong_method = client.Delete("/users");
    ASSERT_TRUE(wrong_method);
    EXPECT_EQ(wrong_method->status, 405);
    EXPECT_EQ(wrong_method->get_header_value("Allow"), "GET, POST");

    EXPECT_EQ(a_.request_count(), 0U);
    EXPECT_EQ(b_.request_count(), 0U);
    EXPECT_EQ(c_.request_count(), 0U);
}

TEST_F(RoundRobinTest, ConcurrentRequestsAreSafeAndReachEveryInstance) {
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;  // 150 total, a multiple of 3.

    std::atomic<int> succeeded{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, &succeeded] {
            auto client = make_client();
            for (int i = 0; i < kPerThread; ++i) {
                const auto response = client.Get("/users");
                if (response && response->status == 200) {
                    ++succeeded;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const std::size_t total = a_.request_count() + b_.request_count() + c_.request_count();
    EXPECT_EQ(succeeded.load(), kThreads * kPerThread);
    EXPECT_EQ(total, static_cast<std::size_t>(kThreads * kPerThread));
    // Ordering across threads is not asserted, but every instance must be used.
    EXPECT_GT(a_.request_count(), 0U);
    EXPECT_GT(b_.request_count(), 0U);
    EXPECT_GT(c_.request_count(), 0U);
}

/// Two services, so their rotations can be shown to be independent.
class IndependentServicesTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        router.add_route(Route{"GET", "/users", "users"});
        router.add_route(Route{"GET", "/orders", "orders"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {
            {"users", {users_a_.endpoint(), users_b_.endpoint(), users_c_.endpoint()}},
            {"orders", {orders_a_.endpoint(), orders_b_.endpoint()}},
        };
        return config;
    }

    gateway_test::TestBackend users_a_{"users-a"};
    gateway_test::TestBackend users_b_{"users-b"};
    gateway_test::TestBackend users_c_{"users-c"};
    gateway_test::TestBackend orders_a_{"orders-a"};
    gateway_test::TestBackend orders_b_{"orders-b"};
};

TEST_F(IndependentServicesTest, EachServiceKeepsItsOwnRotation) {
    auto client = make_client();
    const auto body_of = [&client](const char* path) {
        const auto response = client.Get(path);
        return response && response->status == 200 ? response->body : std::string{"<failed>"};
    };

    // Interleaved: /orders must not advance the /users rotation, or vice versa.
    EXPECT_EQ(body_of("/users"), R"({"backend":"users-a"})");
    EXPECT_EQ(body_of("/orders"), R"({"backend":"orders-a"})");
    EXPECT_EQ(body_of("/users"), R"({"backend":"users-b"})");
    EXPECT_EQ(body_of("/orders"), R"({"backend":"orders-b"})");
    EXPECT_EQ(body_of("/users"), R"({"backend":"users-c"})");
    EXPECT_EQ(body_of("/orders"), R"({"backend":"orders-a"})");
    EXPECT_EQ(body_of("/users"), R"({"backend":"users-a"})");
}

/// A single instance must behave exactly as it did before load balancing.
class SingleInstanceTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {only_.endpoint()}}};
        return config;
    }

    gateway_test::TestBackend only_{"only"};
};

TEST_F(SingleInstanceTest, EveryRequestGoesToTheOneInstance) {
    auto client = make_client();
    for (int i = 0; i < 5; ++i) {
        const auto response = client.Get("/users/1");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_EQ(response->body, R"({"backend":"only"})");
    }
    EXPECT_EQ(only_.request_count(), 5U);
}

/// A service that is routed but configured with an empty instance list.
class NoInstancesTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {}}};
        return config;
    }
};

TEST_F(NoInstancesTest, ServiceWithNoInstancesReturns502) {
    auto client = make_client();
    const auto response = client.Get("/users/1");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("error":"bad_gateway")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("reason":"no_backend_configured")")) << response->body;
}

TEST_F(NoInstancesTest, GatewayKeepsServingHealth) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users/1"));

    const auto response = client.Get("/health");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
}

/// One live instance alongside one whose port nothing is listening on. Retries
/// are switched off here so this keeps testing what it was written for: neither
/// the balancer nor the proxy fails over on its own. Retry-driven recovery is
/// covered in the reliability tests.
class PartiallyDeadPoolTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {live_.endpoint(), closed_endpoint_}}};
        config.max_retries = 0;
        return config;
    }

    gateway_test::TestBackend live_{"live"};
    const gateway::BackendEndpoint closed_endpoint_ = [] {
        const gateway_test::TestBackend probe;  // frees its port when destroyed
        return probe.endpoint();
    }();
};

TEST_F(PartiallyDeadPoolTest, SelectingAnUnreachableInstanceStillReturns502) {
    auto client = make_client();

    const auto first = client.Get("/users");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 200) << "instance 0 is live";

    const auto second = client.Get("/users");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 502) << "no failover without a retry budget";
    EXPECT_TRUE(contains(second->body, R"("reason":"backend_unreachable")")) << second->body;

    // Rotation continues past the failure rather than sticking.
    const auto third = client.Get("/users");
    ASSERT_TRUE(third);
    EXPECT_EQ(third->status, 200);
    EXPECT_EQ(live_.request_count(), 2U);
}

/// A pool whose selected instance answers far later than the gateway will wait.
/// Retries are switched off so the timeout itself is what the test observes.
class SlowInstanceTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override { return users_router(); }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {fast_.endpoint(), slow_.endpoint()}}};
        config.backend_timeout = std::chrono::milliseconds{200};
        config.max_retries = 0;
        return config;
    }

    gateway_test::TestBackend fast_{"fast"};
    gateway_test::TestBackend slow_{"slow"};
};

TEST_F(SlowInstanceTest, SelectingATimingOutInstanceStillReturns504) {
    slow_.set_delay(std::chrono::milliseconds{1500});

    auto client = make_client();
    client.set_read_timeout(10, 0);  // Outlast the gateway's own timeout.

    const auto first = client.Get("/users");
    ASSERT_TRUE(first);
    EXPECT_EQ(first->status, 200) << "the fast instance is selected first";

    const auto second = client.Get("/users");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 504);
    EXPECT_TRUE(contains(second->body, R"("reason":"backend_timeout")")) << second->body;
}

}  // namespace
