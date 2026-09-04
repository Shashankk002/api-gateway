#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gateway {

/// A single routing rule: requests whose path lies at or below `path_prefix`
/// and whose method equals `method` are attributed to the logical service
/// `service`.
///
/// Stage 2 stops at the attribution. Nothing is forwarded anywhere yet; a later
/// stage will hand `service` to a reverse proxy.
struct Route {
    std::string method;       ///< HTTP method, stored upper-case (e.g. "GET").
    std::string path_prefix;  ///< Canonical prefix, no trailing '/' (e.g. "/users").
    std::string service;      ///< Logical service name (e.g. "users").
};

/// Why Router::match() ended the way it did.
enum class MatchStatus {
    kMatched,           ///< A route claims this method and path.
    kMethodNotAllowed,  ///< The path is routed, but not for this method.
    kNotFound,          ///< No route covers this path at all.
};

/// Outcome of Router::match().
///
/// `route` and the views in `allowed_methods` borrow from the Router that
/// produced them. They stay valid until that Router is destroyed or has another
/// route added; copy anything that must outlive either event.
struct RouteMatch {
    MatchStatus status{MatchStatus::kNotFound};

    /// The selected route. Non-null exactly when `status` is kMatched.
    const Route* route{nullptr};

    /// Methods the matched prefix does accept, sorted and de-duplicated.
    /// Non-empty exactly when `status` is kMethodNotAllowed.
    std::vector<std::string_view> allowed_methods;
};

/// Resolves an incoming method and path to a logical service.
///
/// Deliberately independent of the HTTP server: it takes two strings and
/// returns a decision, so it can be unit tested without opening a socket.
///
/// ## Matching rules
///
/// A route with prefix P *covers* a path when the path equals P or continues
/// with a '/' immediately after it. So `/users` covers `/users` and
/// `/users/123`, but not `/usersomething`. The prefix `/` covers every path.
///
/// Matching then runs in two ordered steps:
///
///  1. **Resolve the path**, ignoring the method: among all routes, take the
///     one whose prefix is the longest that covers the path. If nothing covers
///     it, the result is kNotFound. Two different prefixes of equal length
///     cannot both cover the same path, so the winning prefix is unique.
///  2. **Resolve the method** among the routes sharing that winning prefix. An
///     exact method match yields kMatched; otherwise the result is
///     kMethodNotAllowed, listing that prefix's methods.
///
/// Because the path is resolved first, a longer prefix shadows a shorter one
/// even when only the shorter one has the requested method: with `GET /api` and
/// `POST /api/admin` registered, `GET /api/admin/users` is kMethodNotAllowed
/// rather than being attributed to `api`. This keeps the outcome a function of
/// the path alone, which is easier to reason about than a search that backs off
/// to shorter prefixes.
class Router {
public:
    /// Registers a route, normalising it first: the method is upper-cased and
    /// trailing slashes are trimmed from the prefix, so "/users/" and "/users"
    /// name the same route.
    ///
    /// Throws std::invalid_argument if the method or service is empty, if the
    /// prefix is empty or does not start with '/', or if a route with the same
    /// method and prefix is already registered.
    void add_route(Route route);

    /// Resolves `method` and `path` per the rules above. Method comparison is
    /// case-sensitive, as HTTP requires; registration upper-cases, so a table
    /// written with "get" still matches a real GET request.
    [[nodiscard]] RouteMatch match(std::string_view method, std::string_view path) const;

    [[nodiscard]] const std::vector<Route>& routes() const noexcept { return routes_; }
    [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }

private:
    std::vector<Route> routes_;
};

/// The gateway's built-in service route table.
///
/// Stage 2 has no route configuration format yet, so the table lives in code.
/// Loading it from configuration is a later stage's job.
[[nodiscard]] Router default_service_router();

}  // namespace gateway
