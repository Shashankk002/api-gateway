// Unit tests for the Router. No socket is opened: the Router is deliberately
// independent of the HTTP server, so it is exercised directly here.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/router.hpp"

namespace {

using gateway::MatchStatus;
using gateway::Route;
using gateway::Router;

Router make_router(const std::vector<Route>& routes) {
    Router router;
    for (const Route& route : routes) {
        router.add_route(route);
    }
    return router;
}

TEST(RouterTest, ExactPathMatchesItsRoute) {
    const Router router = make_router({{"GET", "/users", "users"}});

    const auto match = router.match("GET", "/users");

    ASSERT_EQ(match.status, MatchStatus::kMatched);
    ASSERT_NE(match.route, nullptr);
    EXPECT_EQ(match.route->service, "users");
    EXPECT_EQ(match.route->path_prefix, "/users");
}

TEST(RouterTest, PathsBelowThePrefixMatchTheSameRoute) {
    const Router router = make_router({{"GET", "/users", "users"}});

    for (const char* path : {"/users/123", "/users/123/profile", "/users/"}) {
        const auto match = router.match("GET", path);
        ASSERT_EQ(match.status, MatchStatus::kMatched) << "path: " << path;
        EXPECT_EQ(match.route->service, "users") << "path: " << path;
    }
}

TEST(RouterTest, PrefixMustEndOnASegmentBoundary) {
    const Router router = make_router({{"GET", "/users", "users"}});

    // These share a textual prefix with /users but are unrelated paths.
    for (const char* path : {"/usersomething", "/users-admin", "/usersomething/123"}) {
        const auto match = router.match("GET", path);
        EXPECT_EQ(match.status, MatchStatus::kNotFound) << "path: " << path;
        EXPECT_EQ(match.route, nullptr) << "path: " << path;
    }
}

TEST(RouterTest, LongestMatchingPrefixWins) {
    const Router router = make_router({
        {"GET", "/api", "api"},
        {"GET", "/api/admin", "admin"},
    });

    const auto match = router.match("GET", "/api/admin/users");

    ASSERT_EQ(match.status, MatchStatus::kMatched);
    EXPECT_EQ(match.route->service, "admin");
    EXPECT_EQ(match.route->path_prefix, "/api/admin");
}

TEST(RouterTest, LongestPrefixWinsRegardlessOfRegistrationOrder) {
    const Router specific_first = make_router({
        {"GET", "/api/admin", "admin"},
        {"GET", "/api", "api"},
    });
    const Router general_first = make_router({
        {"GET", "/api", "api"},
        {"GET", "/api/admin", "admin"},
    });

    EXPECT_EQ(specific_first.match("GET", "/api/admin/users").route->service, "admin");
    EXPECT_EQ(general_first.match("GET", "/api/admin/users").route->service, "admin");
}

TEST(RouterTest, ShorterPrefixStillWinsForPathsOutsideTheLongerOne) {
    const Router router = make_router({
        {"GET", "/api", "api"},
        {"GET", "/api/admin", "admin"},
    });

    const auto match = router.match("GET", "/api/public");

    ASSERT_EQ(match.status, MatchStatus::kMatched);
    EXPECT_EQ(match.route->service, "api");
}

TEST(RouterTest, MethodSelectsAmongRoutesSharingAPrefix) {
    const Router router = make_router({
        {"GET", "/users", "users-read"},
        {"POST", "/users", "users-write"},
    });

    EXPECT_EQ(router.match("GET", "/users/1").route->service, "users-read");
    EXPECT_EQ(router.match("POST", "/users/1").route->service, "users-write");
}

TEST(RouterTest, MethodsAreScopedToTheirOwnPrefix) {
    const Router router = make_router({
        {"GET", "/users", "users"},
        {"POST", "/orders", "orders"},
    });

    // POST exists in the table, but not for /users.
    const auto match = router.match("POST", "/users");

    ASSERT_EQ(match.status, MatchStatus::kMethodNotAllowed);
    EXPECT_EQ(match.allowed_methods, std::vector<std::string_view>({"GET"}));
}

TEST(RouterTest, KnownPathWithUnknownMethodIsMethodNotAllowed) {
    const Router router = make_router({{"GET", "/users", "users"}});

    const auto match = router.match("POST", "/users");

    EXPECT_EQ(match.status, MatchStatus::kMethodNotAllowed);
    EXPECT_EQ(match.route, nullptr);
}

TEST(RouterTest, MethodNotAllowedReportsEveryMethodOfThatPrefixInSortedOrder) {
    const Router router = make_router({
        {"PUT", "/users", "users-replace"},
        {"GET", "/users", "users-read"},
        {"POST", "/users", "users-write"},
    });

    const auto match = router.match("DELETE", "/users/1");

    ASSERT_EQ(match.status, MatchStatus::kMethodNotAllowed);
    EXPECT_EQ(match.allowed_methods, std::vector<std::string_view>({"GET", "POST", "PUT"}));
}

TEST(RouterTest, PathIsResolvedBeforeMethodSoALongerPrefixShadowsAShorterOne) {
    // Documented, deliberate behaviour: matching does not back off to a shorter
    // prefix just because the longest one lacks the requested method.
    const Router router = make_router({
        {"GET", "/api", "api"},
        {"POST", "/api/admin", "admin"},
    });

    const auto match = router.match("GET", "/api/admin/users");

    ASSERT_EQ(match.status, MatchStatus::kMethodNotAllowed);
    EXPECT_EQ(match.allowed_methods, std::vector<std::string_view>({"POST"}));
}

TEST(RouterTest, UnknownPathIsNotFound) {
    const Router router = make_router({{"GET", "/users", "users"}});

    const auto match = router.match("GET", "/orders");

    EXPECT_EQ(match.status, MatchStatus::kNotFound);
    EXPECT_EQ(match.route, nullptr);
    EXPECT_TRUE(match.allowed_methods.empty());
}

TEST(RouterTest, EmptyRouterMatchesNothing) {
    const Router router;

    EXPECT_TRUE(router.empty());
    EXPECT_EQ(router.match("GET", "/users").status, MatchStatus::kNotFound);
    EXPECT_EQ(router.match("GET", "/").status, MatchStatus::kNotFound);
}

TEST(RouterTest, RootPrefixCoversEveryPath) {
    const Router router = make_router({{"GET", "/", "root"}});

    for (const char* path : {"/", "/users", "/a/b/c"}) {
        const auto match = router.match("GET", path);
        ASSERT_EQ(match.status, MatchStatus::kMatched) << "path: " << path;
        EXPECT_EQ(match.route->service, "root") << "path: " << path;
    }
}

TEST(RouterTest, MoreSpecificPrefixWinsOverRoot) {
    const Router router = make_router({
        {"GET", "/", "root"},
        {"GET", "/users", "users"},
    });

    EXPECT_EQ(router.match("GET", "/users/1").route->service, "users");
    EXPECT_EQ(router.match("GET", "/other").route->service, "root");
}

TEST(RouterTest, TrailingSlashesAreNormalisedAwayOnRegistration) {
    const Router router = make_router({{"GET", "/users/", "users"}});

    ASSERT_EQ(router.routes().size(), 1U);
    EXPECT_EQ(router.routes().front().path_prefix, "/users");
    EXPECT_EQ(router.match("GET", "/users").status, MatchStatus::kMatched);
    EXPECT_EQ(router.match("GET", "/users/1").status, MatchStatus::kMatched);
}

TEST(RouterTest, MethodsAreUpperCasedOnRegistration) {
    const Router router = make_router({{"get", "/users", "users"}});

    EXPECT_EQ(router.routes().front().method, "GET");
    EXPECT_EQ(router.match("GET", "/users").status, MatchStatus::kMatched);
}

TEST(RouterTest, RejectsInvalidRouteDefinitions) {
    Router router;

    EXPECT_THROW(router.add_route(Route{"", "/users", "users"}), std::invalid_argument);
    EXPECT_THROW(router.add_route(Route{"GET", "", "users"}), std::invalid_argument);
    EXPECT_THROW(router.add_route(Route{"GET", "users", "users"}), std::invalid_argument);
    EXPECT_THROW(router.add_route(Route{"GET", "/users", ""}), std::invalid_argument);
    EXPECT_TRUE(router.empty()) << "a rejected route must not be registered";
}

TEST(RouterTest, RejectsDuplicateMethodAndPrefix) {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});

    EXPECT_THROW(router.add_route(Route{"GET", "/users", "other"}), std::invalid_argument);
    // Normalisation happens before the duplicate check.
    EXPECT_THROW(router.add_route(Route{"get", "/users/", "other"}), std::invalid_argument);

    ASSERT_EQ(router.routes().size(), 1U);
    EXPECT_EQ(router.match("GET", "/users").route->service, "users");
}

TEST(RouterTest, DefaultTableRoutesTheBuiltInServices) {
    const Router router = gateway::default_service_router();

    EXPECT_EQ(router.match("GET", "/users/123").route->service, "users");
    EXPECT_EQ(router.match("GET", "/orders").route->service, "orders");
    EXPECT_EQ(router.match("GET", "/products/9/reviews").route->service, "products");
    EXPECT_EQ(router.match("GET", "/health").status, MatchStatus::kNotFound)
        << "/health is gateway-owned and must not be a service route";
}

}  // namespace
