// End-to-end reverse-proxy tests. Each starts a real backend and a real
// gateway, then drives the full path: client -> gateway -> backend -> gateway
// -> client, asserting both on what the backend received and on what the client
// got back.

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <string>

#include "gateway/config.hpp"
#include "gateway/router.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::Route;
using gateway::Router;

using gateway_test::contains;

class ProxyIntegrationTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        for (const char* method : {"GET", "POST", "PUT", "DELETE"}) {
            router.add_route(Route{method, "/users", "users"});
        }
        // Routed, but deliberately absent from the backend table.
        router.add_route(Route{"GET", "/ghost", "ghost"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(ProxyIntegrationTest, GetRequestIsForwardedToTheBackend) {
    auto client = make_client();
    const auto response = client.Get("/users/123");

    ASSERT_TRUE(response) << "request failed: " << httplib::to_string(response.error());
    EXPECT_EQ(response->status, 200);

    ASSERT_EQ(backend_.request_count(), 1U);
    EXPECT_EQ(backend_.received().front().method, "GET");
}

TEST_F(ProxyIntegrationTest, OriginalPathIsPreserved) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users/123/profile"));

    ASSERT_EQ(backend_.request_count(), 1U);
    EXPECT_EQ(backend_.received().front().path, "/users/123/profile");
}

TEST_F(ProxyIntegrationTest, QueryStringIsPreserved) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users/123?verbose=true&page=2"));

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_EQ(received.path, "/users/123");
    EXPECT_EQ(received.target, "/users/123?verbose=true&page=2");
}

TEST_F(ProxyIntegrationTest, PostBodyIsForwarded) {
    auto client = make_client();
    const std::string body = R"({"name":"ada"})";
    const auto response = client.Post("/users", body, "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_EQ(received.method, "POST");
    EXPECT_EQ(received.body, body);
    EXPECT_EQ(received.header("Content-Type"), "application/json");
    EXPECT_EQ(received.header("Content-Length"), std::to_string(body.size()));
}

TEST_F(ProxyIntegrationTest, PutAndDeleteMethodsAndBodiesAreForwarded) {
    auto client = make_client();

    const std::string put_body = R"({"name":"grace"})";
    ASSERT_TRUE(client.Put("/users/1", put_body, "application/json"));
    const std::string delete_body = R"({"reason":"cleanup"})";
    ASSERT_TRUE(client.Delete("/users/1", delete_body, "application/json"));

    ASSERT_EQ(backend_.request_count(), 2U);
    const auto received = backend_.received();
    EXPECT_EQ(received[0].method, "PUT");
    EXPECT_EQ(received[0].body, put_body);
    EXPECT_EQ(received[1].method, "DELETE");
    EXPECT_EQ(received[1].body, delete_body);
}

TEST_F(ProxyIntegrationTest, ApplicationRequestHeadersAreForwarded) {
    auto client = make_client();
    const httplib::Headers headers{
        {"X-Request-Id", "abc-123"},
        {"Authorization", "Bearer token"},
        {"Accept-Language", "en-GB"},
    };
    ASSERT_TRUE(client.Get("/users", headers));

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_EQ(received.header("X-Request-Id"), "abc-123");
    EXPECT_EQ(received.header("Authorization"), "Bearer token");
    EXPECT_EQ(received.header("Accept-Language"), "en-GB");
}

TEST_F(ProxyIntegrationTest, HopByHopRequestHeadersAreNotForwarded) {
    auto client = make_client();
    const httplib::Headers headers{
        {"TE", "trailers"},
        {"Upgrade", "websocket"},
        {"Proxy-Authorization", "Basic secret"},
    };
    ASSERT_TRUE(client.Get("/users", headers));

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_FALSE(received.has_header("TE"));
    EXPECT_FALSE(received.has_header("Upgrade"));
    EXPECT_FALSE(received.has_header("Proxy-Authorization"));
    // Host is regenerated for the backend rather than passed through.
    EXPECT_EQ(received.header("Host"), "127.0.0.1:" + std::to_string(backend_.port()));
}

TEST_F(ProxyIntegrationTest, HeadersNamedByConnectionAreNotForwarded) {
    // RFC 9110 7.6.1: Connection names further headers that apply only to this
    // hop, so which ones they are is decided per request, not by a fixed list.
    auto client = make_client();
    const httplib::Headers headers{
        {"Connection", "keep-alive, X-Hop-Only"},
        {"X-Hop-Only", "must not reach the backend"},
        {"Proxy-Connection", "keep-alive"},
        {"X-Application", "kept"},
    };
    ASSERT_TRUE(client.Get("/users", headers));

    ASSERT_EQ(backend_.request_count(), 1U);
    const auto received = backend_.received().front();
    EXPECT_FALSE(received.has_header("X-Hop-Only"));
    EXPECT_FALSE(received.has_header("Proxy-Connection"));
    EXPECT_EQ(received.header("X-Application"), "kept");
}

TEST_F(ProxyIntegrationTest, HeadersNamedByTheBackendsConnectionAreNotReturned) {
    backend_.set_response(200, R"({"from":"backend"})", "application/json",
                          {{"Connection", "X-Backend-Hop"},
                           {"X-Backend-Hop", "internal"},
                           {"X-Backend-Application", "kept"}});
    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_FALSE(response->has_header("X-Backend-Hop"));
    EXPECT_EQ(response->get_header_value("X-Backend-Application"), "kept");
}

TEST_F(ProxyIntegrationTest, RequestBodyOverTheLimitIsRejectedWithoutContactingABackend) {
    // The gateway buffers the whole body so a retry can replay it, so the
    // ceiling is what keeps one client from sizing its memory use.
    auto client = make_client();
    const std::string oversized(gateway::ServerConfig::kDefaultMaxRequestBodyBytes + 1024, 'x');
    const auto response = client.Post("/users", oversized, "application/octet-stream");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 413);
    EXPECT_EQ(backend_.request_count(), 0U);
}

TEST_F(ProxyIntegrationTest, RequestBodyUnderTheLimitIsStillForwarded) {
    auto client = make_client();
    const std::string body(64 * 1024, 'y');
    const auto response = client.Post("/users", body, "application/octet-stream");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    ASSERT_EQ(backend_.request_count(), 1U);
    EXPECT_EQ(backend_.received().front().body, body);
}

TEST_F(ProxyIntegrationTest, BackendResponseStatusIsPreserved) {
    for (const int status : {200, 201, 400, 404, 418, 500}) {
        backend_.set_response(status, R"({"from":"backend"})");

        auto client = make_client();
        const auto response = client.Get("/users");

        ASSERT_TRUE(response) << "status under test: " << status;
        EXPECT_EQ(response->status, status);
        EXPECT_TRUE(contains(response->body, R"("from":"backend")"))
            << "status " << status << " body: " << response->body;
    }
}

TEST_F(ProxyIntegrationTest, BackendResponseBodyIsPreserved) {
    backend_.set_response(200, "plain payload, not JSON", "text/plain");

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->body, "plain payload, not JSON");
    EXPECT_EQ(response->get_header_value("Content-Type"), "text/plain");
}

TEST_F(ProxyIntegrationTest, BackendResponseHeadersArePreserved) {
    backend_.set_response(200, "{}", "application/json",
                          httplib::Headers{{"X-Backend-Version", "7"},
                                           {"Cache-Control", "no-store"},
                                           {"Connection", "keep-alive"}});

    auto client = make_client();
    const auto response = client.Get("/users");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->get_header_value("X-Backend-Version"), "7");
    EXPECT_EQ(response->get_header_value("Cache-Control"), "no-store");
    // The backend's hop-by-hop header must not be copied through verbatim.
    EXPECT_NE(response->get_header_value("Connection"), "keep-alive");
}

TEST_F(ProxyIntegrationTest, UnknownRouteReturns404WithoutContactingABackend) {
    auto client = make_client();
    const auto response = client.Get("/nothing-here");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_TRUE(contains(response->body, R"("error":"not_found")")) << response->body;
    EXPECT_EQ(backend_.request_count(), 0U);
}

TEST_F(ProxyIntegrationTest, UnsupportedMethodReturns405WithoutContactingABackend) {
    auto client = make_client();
    const auto response = client.Patch("/users", "{}", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 405);
    EXPECT_EQ(response->get_header_value("Allow"), "DELETE, GET, POST, PUT");
    EXPECT_EQ(backend_.request_count(), 0U);
}

TEST_F(ProxyIntegrationTest, ServiceWithNoConfiguredBackendReturns502) {
    auto client = make_client();
    const auto response = client.Get("/ghost");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("error":"bad_gateway")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("reason":"no_backend_configured")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("service":"ghost")")) << response->body;
}

TEST_F(ProxyIntegrationTest, HealthIsAnsweredByTheGatewayAndNeverProxied) {
    auto client = make_client();
    const auto response = client.Get("/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_TRUE(contains(response->body, R"("status":"healthy")")) << response->body;
    EXPECT_EQ(backend_.request_count(), 0U);
}

// A backend that is configured but not listening: its port was bound by a probe
// server that has since shut down, so connections are refused.
class UnreachableBackendTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        router.add_route(Route{"GET", "/users", "users"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {closed_endpoint_}}};
        return config;
    }

    const gateway::BackendEndpoint closed_endpoint_ = gateway_test::closed_endpoint();
};

TEST_F(UnreachableBackendTest, ConnectionFailureReturns502) {
    auto client = make_client();
    const auto response = client.Get("/users/1");

    ASSERT_TRUE(response) << "the gateway itself must still answer";
    EXPECT_EQ(response->status, 502);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("error":"bad_gateway")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("reason":"backend_unreachable")")) << response->body;
}

TEST_F(UnreachableBackendTest, GatewayKeepsServingHealthAfterABackendFailure) {
    auto client = make_client();
    ASSERT_TRUE(client.Get("/users/1"));

    const auto response = client.Get("/health");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
}

// A backend that answers far later than the gateway is willing to wait.
class SlowBackendTest : public gateway_test::GatewayServerTestBase {
protected:
    Router make_router() override {
        Router router;
        router.add_route(Route{"GET", "/users", "users"});
        return router;
    }

    gateway::ServerConfig make_config() override {
        auto config = loopback_config();
        config.backends = {{"users", {backend_.endpoint()}}};
        config.backend_timeout = std::chrono::milliseconds{200};
        return config;
    }

    gateway_test::TestBackend backend_{"users"};
};

TEST_F(SlowBackendTest, BackendTimeoutReturns504) {
    backend_.set_delay(std::chrono::milliseconds{1500});

    auto client = make_client();
    client.set_read_timeout(10, 0);  // Outlast the gateway's own 200ms timeout.
    const auto response = client.Get("/users/1");

    ASSERT_TRUE(response) << "the gateway itself must still answer";
    EXPECT_EQ(response->status, 504);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    EXPECT_TRUE(contains(response->body, R"("error":"gateway_timeout")")) << response->body;
    EXPECT_TRUE(contains(response->body, R"("reason":"backend_timeout")")) << response->body;
}

}  // namespace
