#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gateway {

/// Requests using `method` whose path lies at or below `path_prefix` belong to
/// the logical service `service`.
struct Route {
    std::string method;       ///< Upper-case, e.g. "GET".
    std::string path_prefix;  ///< No trailing '/', e.g. "/users".
    std::string service;
};

enum class MatchStatus {
    kMatched,
    kMethodNotAllowed,
    kNotFound,
};

struct RouteMatch {
    MatchStatus status{MatchStatus::kNotFound};

    /// Set exactly when kMatched. Borrows from the Router and stays valid until
    /// that Router is destroyed or gains another route.
    const Route* route{nullptr};

    /// Set exactly when kMethodNotAllowed. Sorted, and borrows as `route` does.
    std::vector<std::string_view> allowed_methods;
};

/// Resolves a method and path to a logical service. Does no I/O and knows
/// nothing about HTTP, so it is unit testable without a socket.
///
/// A prefix P covers a path when the path equals P or continues with '/'
/// immediately after it, so "/users" covers "/users/123" but not
/// "/usersomething"; "/" covers everything. Matching resolves the path first,
/// taking the longest covering prefix, and only then the method. That ordering
/// means a longer prefix shadows a shorter one even when only the shorter has
/// the requested method: with `GET /api` and `POST /api/admin`, a
/// `GET /api/admin/users` is kMethodNotAllowed rather than falling back to
/// `api`. Keeping the outcome a function of the path alone is easier to reason
/// about than a search that backs off.
class Router {
public:
    /// Normalises before storing: method upper-cased, trailing slashes trimmed.
    /// Throws std::invalid_argument on an empty method or service, a prefix not
    /// starting with '/', or a duplicate method+prefix.
    void add_route(Route route);

    /// Method comparison is case-sensitive, as HTTP requires.
    [[nodiscard]] RouteMatch match(std::string_view method, std::string_view path) const;

    [[nodiscard]] const std::vector<Route>& routes() const noexcept { return routes_; }
    [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }

private:
    std::vector<Route> routes_;
};

/// Built-in route table. There is no route configuration format yet.
[[nodiscard]] Router default_service_router();

}  // namespace gateway
