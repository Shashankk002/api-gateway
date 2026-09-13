// Integration tests for the gateway-owned endpoints and error responses. They
// drive a real GatewayServer over a real TCP socket and assert only on what an
// HTTP client can observe.

#include <gtest/gtest.h>
#include <httplib.h>

#include <string>

#include "gateway_fixture.hpp"

namespace {

// Served with the gateway's built-in route table, as the shipped binary is.
class GatewayServerTest : public gateway_test::GatewayServerTestBase {};

TEST_F(GatewayServerTest, HealthEndpointReturns200) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response) << "request failed: " << httplib::to_string(response.error());
    EXPECT_EQ(response->status, 200);
}

TEST_F(GatewayServerTest, HealthEndpointReportsHealthyJson) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_NE(response->body.find("\"status\":\"healthy\""), std::string::npos)
        << "unexpected body: " << response->body;
    EXPECT_NE(response->body.find("\"service\":\"api-gateway\""), std::string::npos)
        << "unexpected body: " << response->body;
}

TEST_F(GatewayServerTest, UnknownRouteReturns404) {
    auto client = make_client();
    const auto response = client.Get("/does-not-exist");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_NE(response->body.find("\"error\":\"not_found\""), std::string::npos)
        << "unexpected body: " << response->body;
}

TEST_F(GatewayServerTest, UnsupportedMethodOnHealthReturns404) {
    auto client = make_client();
    const auto response = client.Post("/health", "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
}

TEST_F(GatewayServerTest, NoUnexpectedGatewayRoutesAreExposed) {
    auto client = make_client();
    // /metrics is a deliberate gateway-owned endpoint and is asserted separately.
    for (const char* path : {"/", "/routes", "/admin"}) {
        const auto response = client.Get(path);
        ASSERT_TRUE(response) << "request to " << path << " failed";
        EXPECT_EQ(response->status, 404) << "unexpected route exposed: " << path;
    }
}

TEST_F(GatewayServerTest, MetricsEndpointIsGatewayOwned) {
    auto client = make_client();
    const auto response = client.Get("/metrics");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("text/plain"), std::string::npos)
        << response->get_header_value("Content-Type");
}

}  // namespace
