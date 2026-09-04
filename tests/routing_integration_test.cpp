// Integration tests for routing: a real GatewayServer, a real socket, and only
// client-observable assertions.

#include <gtest/gtest.h>
#include <httplib.h>

#include <string>

#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::Route;
using gateway::Router;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A table chosen to exercise prefix boundaries, longest-prefix selection and
// method handling over real HTTP. Each service has its own backend, so the
// service that was selected is observable from which backend answered.
class RoutingIntegrationTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        router.add_route(Route{"GET", "/users", "users"});
        router.add_route(Route{"POST", "/users", "users-write"});
        router.add_route(Route{"GET", "/api", "api"});
        router.add_route(Route{"GET", "/api/admin", "admin"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {
            {"users", {users_.endpoint()}},
            {"users-write", {users_write_.endpoint()}},
            {"api", {api_.endpoint()}},
            {"admin", {admin_.endpoint()}},
        };
        return config;
    }

    gateway_test::TestBackend users_{"users"};
    gateway_test::TestBackend users_write_{"users-write"};
    gateway_test::TestBackend api_{"api"};
    gateway_test::TestBackend admin_{"admin"};
};

TEST_F(RoutingIntegrationTest, HealthStillReturns200) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
}

TEST_F(RoutingIntegrationTest, ExactPathReachesTheSelectedServiceBackend) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response) << "request failed: " << httplib::to_string(response.error());
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("backend":"users")")) << response->body;

    ASSERT_EQ(users_.request_count(), 1U);
    EXPECT_EQ(users_.received().front().path, "/users");
    EXPECT_EQ(users_write_.request_count(), 0U);
}

TEST_F(RoutingIntegrationTest, PathBelowThePrefixReachesTheSameServiceBackend) {
    auto client = make_client();
    const auto response = client.Get("/users/123");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("backend":"users")")) << response->body;

    ASSERT_EQ(users_.request_count(), 1U);
    EXPECT_EQ(users_.received().front().path, "/users/123");
}

TEST_F(RoutingIntegrationTest, PathSharingATextualPrefixReturns404) {
    auto client = make_client();
    const auto response = client.Get("/usersomething");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_TRUE(contains(response->body, R"("error":"not_found")")) << response->body;
    EXPECT_EQ(users_.request_count(), 0U) << "a 404 must not reach a backend";
}

TEST_F(RoutingIntegrationTest, UnknownPathReturns404) {
    auto client = make_client();
    const auto response = client.Get("/unknown");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("error":"not_found")")) << response->body;
}

TEST_F(RoutingIntegrationTest, KnownPathWithUnsupportedMethodReturns405) {
    auto client = make_client();
    const auto response = client.Delete("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 405);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_EQ(response->get_header_value("Allow"), "GET, POST");
    EXPECT_TRUE(contains(response->body, R"("error":"method_not_allowed")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("allowed":["GET","POST"])")) << response->body;
    EXPECT_EQ(users_.request_count(), 0U) << "a 405 must not reach a backend";
    EXPECT_EQ(users_write_.request_count(), 0U);
}

TEST_F(RoutingIntegrationTest, UnsupportedMethodBelowThePrefixAlsoReturns405) {
    auto client = make_client();
    const auto response = client.Delete("/users/123");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 405);
}

TEST_F(RoutingIntegrationTest, EachMethodSelectsItsOwnServiceBackend) {
    auto client = make_client();

    const auto get_response = client.Get("/users/1");
    ASSERT_TRUE(get_response);
    EXPECT_EQ(get_response->status, 200);
    EXPECT_TRUE(contains(get_response->body, R"("backend":"users")")) << get_response->body;

    const auto post_response = client.Post("/users/1", "", "application/json");
    ASSERT_TRUE(post_response);
    EXPECT_EQ(post_response->status, 200);
    EXPECT_TRUE(contains(post_response->body, R"("backend":"users-write")"))
        << post_response->body;

    EXPECT_EQ(users_.request_count(), 1U);
    EXPECT_EQ(users_write_.request_count(), 1U);
}

TEST_F(RoutingIntegrationTest, LongestPrefixWinsOverHttp) {
    auto client = make_client();

    const auto admin = client.Get("/api/admin/users");
    ASSERT_TRUE(admin);
    EXPECT_EQ(admin->status, 200);
    EXPECT_TRUE(contains(admin->body, R"("backend":"admin")")) << admin->body;

    const auto api = client.Get("/api/public");
    ASSERT_TRUE(api);
    EXPECT_EQ(api->status, 200);
    EXPECT_TRUE(contains(api->body, R"("backend":"api")")) << api->body;

    EXPECT_EQ(admin_.request_count(), 1U);
    EXPECT_EQ(api_.request_count(), 1U);
}

// A catch-all route table, to prove the gateway's own endpoint is never handed
// to a service no matter how broad the table is.
class CatchAllRoutingTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        router.add_route(Route{"GET", "/", "root"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"root", {root_.endpoint()}}};
        return config;
    }

    gateway_test::TestBackend root_{"root"};
};

TEST_F(CatchAllRoutingTest, HealthRemainsGatewayOwnedUnderACatchAllRoute) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    EXPECT_EQ(root_.request_count(), 0U) << "/health must never reach a backend";
}

TEST_F(CatchAllRoutingTest, RootRouteCoversArbitraryPaths) {
    auto client = make_client();
    const auto response = client.Get("/anything/at/all");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("backend":"root")")) << response->body;
    ASSERT_EQ(root_.request_count(), 1U);
    EXPECT_EQ(root_.received().front().path, "/anything/at/all");
}

// The route table the shipped binary serves, pointed at test backends.
class DefaultRouterIntegrationTest : public gateway_test::GatewayServerTestBase {
protected:
    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {
            {"users", {users_.endpoint()}},
            {"orders", {orders_.endpoint()}},
            {"products", {products_.endpoint()}},
        };
        return config;
    }

    gateway_test::TestBackend users_{"users"};
    gateway_test::TestBackend orders_{"orders"};
    gateway_test::TestBackend products_{"products"};
};

TEST_F(DefaultRouterIntegrationTest, BuiltInServiceRoutesAreServed) {
    auto client = make_client();

    for (const auto& [path, service] : {std::pair{"/users/123", "users"},
                                        std::pair{"/orders", "orders"},
                                        std::pair{"/products/9", "products"}}) {
        const auto response = client.Get(path);
        ASSERT_TRUE(response) << "request to " << path << " failed";
        EXPECT_EQ(response->status, 200) << "path: " << path;
        EXPECT_TRUE(contains(response->body, std::string(R"("backend":")") + service + '"'))
            << "path: " << path << " body: " << response->body;
    }
}

TEST_F(DefaultRouterIntegrationTest, BuiltInTableRejectsWritesWith405) {
    auto client = make_client();
    const auto response = client.Post("/users", "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 405);
    EXPECT_EQ(response->get_header_value("Allow"), "GET");
}

}  // namespace
