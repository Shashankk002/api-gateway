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
// method handling over real HTTP.
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
};

TEST_F(RoutingIntegrationTest, HealthStillReturns200) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
}

TEST_F(RoutingIntegrationTest, ExactPathReportsTheSelectedService) {
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response) << "request failed: " << httplib::to_string(response.error());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("status":"routed")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("service":"users")")) << response->body;
}

TEST_F(RoutingIntegrationTest, PathBelowThePrefixReportsTheSameService) {
    auto client = make_client();
    const auto response = client.Get("/users/123");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("service":"users")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("matched_prefix":"/users")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("path":"/users/123")")) << response->body;
}

TEST_F(RoutingIntegrationTest, PathSharingATextualPrefixReturns404) {
    auto client = make_client();
    const auto response = client.Get("/usersomething");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_TRUE(contains(response->body, R"("error":"not_found")")) << response->body;
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
}

TEST_F(RoutingIntegrationTest, UnsupportedMethodBelowThePrefixAlsoReturns405) {
    auto client = make_client();
    const auto response = client.Delete("/users/123");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 405);
}

TEST_F(RoutingIntegrationTest, EachMethodSelectsItsOwnService) {
    auto client = make_client();

    const auto get_response = client.Get("/users/1");
    ASSERT_TRUE(get_response);
    EXPECT_EQ(get_response->status, 200);
    EXPECT_TRUE(contains(get_response->body, R"("service":"users")")) << get_response->body;

    const auto post_response = client.Post("/users/1", "", "application/json");
    ASSERT_TRUE(post_response);
    EXPECT_EQ(post_response->status, 200);
    EXPECT_TRUE(contains(post_response->body, R"("service":"users-write")"))
        << post_response->body;
}

TEST_F(RoutingIntegrationTest, LongestPrefixWinsOverHttp) {
    auto client = make_client();

    const auto admin = client.Get("/api/admin/users");
    ASSERT_TRUE(admin);
    EXPECT_EQ(admin->status, 200);
    EXPECT_TRUE(contains(admin->body, R"("service":"admin")")) << admin->body;
    EXPECT_TRUE(contains(admin->body, R"("matched_prefix":"/api/admin")")) << admin->body;

    const auto api = client.Get("/api/public");
    ASSERT_TRUE(api);
    EXPECT_EQ(api->status, 200);
    EXPECT_TRUE(contains(api->body, R"("service":"api")")) << api->body;
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
};

TEST_F(CatchAllRoutingTest, HealthRemainsGatewayOwnedUnderACatchAllRoute) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    EXPECT_FALSE(contains(response->body, R"("status":"routed")")) << response->body;
}

TEST_F(CatchAllRoutingTest, RootRouteCoversArbitraryPaths) {
    auto client = make_client();
    const auto response = client.Get("/anything/at/all");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("service":"root")")) << response->body;
}

// The table the shipped binary serves.
class DefaultRouterIntegrationTest : public gateway_test::GatewayServerTestBase {};

TEST_F(DefaultRouterIntegrationTest, BuiltInServiceRoutesAreServed) {
    auto client = make_client();

    for (const auto& [path, service] : {std::pair{"/users/123", "users"},
                                        std::pair{"/orders", "orders"},
                                        std::pair{"/products/9", "products"}}) {
        const auto response = client.Get(path);
        ASSERT_TRUE(response) << "request to " << path << " failed";
        EXPECT_EQ(response->status, 200) << "path: " << path;
        EXPECT_TRUE(contains(response->body, std::string(R"("service":")") + service + '"'))
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
