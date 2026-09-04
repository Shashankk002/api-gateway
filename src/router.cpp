#include "gateway/router.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace gateway {
namespace {

std::string normalize_method(std::string_view method) {
    if (method.empty()) {
        throw std::invalid_argument("route method must not be empty");
    }
    std::string upper(method);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return upper;
}

std::string normalize_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("route path prefix must start with '/', got '" +
                                    std::string(prefix) + "'");
    }
    // Trim trailing slashes so that "/users/" and "/users" are one route. "/"
    // keeps its single slash and stays the root prefix.
    std::size_t end = prefix.size();
    while (end > 1 && prefix[end - 1] == '/') {
        --end;
    }
    return std::string(prefix.substr(0, end));
}

/// True when `path` sits at or below `prefix`, respecting segment boundaries:
/// "/users" covers "/users" and "/users/123" but not "/usersomething".
bool covers(std::string_view prefix, std::string_view path) {
    if (prefix == "/") {
        return path.starts_with('/');  // The root prefix covers every path.
    }
    if (!path.starts_with(prefix)) {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

}  // namespace

void Router::add_route(Route route) {
    route.method = normalize_method(route.method);
    route.path_prefix = normalize_prefix(route.path_prefix);
    if (route.service.empty()) {
        throw std::invalid_argument("route service name must not be empty for " + route.method +
                                    ' ' + route.path_prefix);
    }

    const bool already_registered =
        std::any_of(routes_.begin(), routes_.end(), [&route](const Route& existing) {
            return existing.method == route.method && existing.path_prefix == route.path_prefix;
        });
    if (already_registered) {
        throw std::invalid_argument("duplicate route " + route.method + ' ' + route.path_prefix);
    }

    routes_.push_back(std::move(route));
}

RouteMatch Router::match(std::string_view method, std::string_view path) const {
    // Step 1: resolve the path on its own, taking the longest covering prefix.
    const Route* longest = nullptr;
    for (const Route& route : routes_) {
        if (!covers(route.path_prefix, path)) {
            continue;
        }
        if (longest == nullptr || route.path_prefix.size() > longest->path_prefix.size()) {
            longest = &route;
        }
    }
    if (longest == nullptr) {
        return RouteMatch{};  // kNotFound, with no route and no allowed methods.
    }

    // Step 2: resolve the method among the routes sharing that winning prefix.
    RouteMatch result;
    for (const Route& route : routes_) {
        if (route.path_prefix != longest->path_prefix) {
            continue;
        }
        if (route.method == method) {
            result.status = MatchStatus::kMatched;
            result.route = &route;
            return result;
        }
        result.allowed_methods.push_back(route.method);
    }

    // Sorted so that the reported set does not depend on registration order.
    std::sort(result.allowed_methods.begin(), result.allowed_methods.end());
    result.status = MatchStatus::kMethodNotAllowed;
    return result;
}

Router default_service_router() {
    Router router;
    router.add_route(Route{"GET", "/users", "users"});
    router.add_route(Route{"GET", "/orders", "orders"});
    router.add_route(Route{"GET", "/products", "products"});
    return router;
}

}  // namespace gateway
