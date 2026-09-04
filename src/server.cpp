#include "gateway/server.hpp"

#include <httplib.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gateway/router.hpp"

namespace gateway {
namespace {

constexpr const char* kJsonContentType = "application/json";

// Owned by the gateway itself: never offered to the Router, so no route table
// can shadow or take over the health endpoint.
constexpr const char* kHealthPath = "/health";

// These payloads are fixed, so they are stored as literals rather than
// assembled at runtime. The gateway has no JSON dependency.
constexpr const char* kHealthyBody = R"({"status":"healthy","service":"api-gateway"})";
constexpr const char* kNotFoundBody = R"({"status":"error","error":"not_found"})";

/// Appends `value` to `out` as a quoted JSON string. Routed responses echo the
/// request path, which is attacker-controlled, so it must be escaped.
void append_json_string(std::string& out, std::string_view value) {
    static constexpr char kHexDigits[] = "0123456789abcdef";

    out += '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    out += "\\u00";
                    out += kHexDigits[(byte >> 4) & 0x0F];
                    out += kHexDigits[byte & 0x0F];
                } else {
                    out += character;
                }
                break;
        }
    }
    out += '"';
}

/// Body for a request the Router attributed to a service. Stage 2 forwards
/// nothing, so the response simply reports which service was selected and why.
std::string routed_body(const Route& route, const httplib::Request& request) {
    std::string body = R"({"status":"routed","service":)";
    append_json_string(body, route.service);
    body += R"(,"matched_prefix":)";
    append_json_string(body, route.path_prefix);
    body += R"(,"method":)";
    append_json_string(body, request.method);
    body += R"(,"path":)";
    append_json_string(body, request.path);
    body += '}';
    return body;
}

std::string method_not_allowed_body(const std::vector<std::string_view>& allowed_methods) {
    std::string body = R"({"status":"error","error":"method_not_allowed","allowed":[)";
    for (std::size_t i = 0; i < allowed_methods.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        append_json_string(body, allowed_methods[i]);
    }
    body += "]}";
    return body;
}

/// RFC 9110 requires a 405 response to carry an Allow header.
std::string allow_header_value(const std::vector<std::string_view>& allowed_methods) {
    std::string value;
    for (const std::string_view method : allowed_methods) {
        if (!value.empty()) {
            value += ", ";
        }
        value.append(method);
    }
    return value;
}

}  // namespace

GatewayServer::GatewayServer(ServerConfig config)
    : GatewayServer(std::move(config), default_service_router()) {}

GatewayServer::GatewayServer(ServerConfig config, Router router)
    : config_(std::move(config)),
      router_(std::move(router)),
      http_(std::make_unique<httplib::Server>()) {
    register_routes();
}

GatewayServer::~GatewayServer() = default;

void GatewayServer::handle_service_request(const httplib::Request& request,
                                           httplib::Response& response) const {
    const RouteMatch match = router_.match(request.method, request.path);

    switch (match.status) {
        case MatchStatus::kMatched:
            response.status = httplib::StatusCode::OK_200;
            response.set_content(routed_body(*match.route, request), kJsonContentType);
            return;

        case MatchStatus::kMethodNotAllowed:
            response.status = httplib::StatusCode::MethodNotAllowed_405;
            response.set_header("Allow", allow_header_value(match.allowed_methods));
            response.set_content(method_not_allowed_body(match.allowed_methods),
                                 kJsonContentType);
            return;

        case MatchStatus::kNotFound:
            // Leave the body empty; the shared error handler below writes the
            // gateway's standard 404 payload, so there is one source of truth.
            response.status = httplib::StatusCode::NotFound_404;
            return;
    }
}

void GatewayServer::register_routes() {
    http_->Get(kHealthPath, [](const httplib::Request&, httplib::Response& response) {
        response.set_content(kHealthyBody, kJsonContentType);
    });

    // Every path except the gateway's own endpoints is resolved by the Router.
    // Running before httplib's own dispatch keeps route matching in one place.
    http_->set_pre_routing_handler(
        [this](const httplib::Request& request, httplib::Response& response) {
            if (request.path == kHealthPath) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            handle_service_request(request, response);
            return httplib::Server::HandlerResponse::Handled;
        });

    // Anything the gateway does not know about, including unmatched methods on
    // its own endpoints.
    http_->set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.status == httplib::StatusCode::NotFound_404) {
            response.set_content(kNotFoundBody, kJsonContentType);
        }
    });

    http_->set_exception_handler(
        [](const httplib::Request& request, httplib::Response& response, std::exception_ptr) {
            std::cerr << "gateway: unhandled exception while serving " << request.method << ' '
                      << request.path << '\n';
            response.status = httplib::StatusCode::InternalServerError_500;
            response.set_content(R"({"status":"error","error":"internal_error"})",
                                 kJsonContentType);
        });
}

bool GatewayServer::bind(std::uint16_t port) {
    bound_port_ = 0;

    // httplib exposes two entry points: one for a fixed port, one for an
    // OS-assigned port that reports back which one it got.
    if (port == 0) {
        const int assigned = http_->bind_to_any_port(config_.host);
        if (assigned < 0) {
            return false;
        }
        bound_port_ = static_cast<std::uint16_t>(assigned);
        return true;
    }

    if (!http_->bind_to_port(config_.host, port)) {
        return false;
    }
    bound_port_ = port;
    return true;
}

bool GatewayServer::serve() {
    if (bound_port_ == 0) {
        return false;
    }
    return http_->listen_after_bind();
}

bool GatewayServer::run() {
    if (!bind(config_.port)) {
        std::cerr << "gateway: failed to bind " << config_.host << ':' << config_.port << '\n';
        return false;
    }
    for (const Route& route : router_.routes()) {
        std::cout << "gateway: route " << route.method << ' ' << route.path_prefix << " -> "
                  << route.service << '\n';
    }
    // std::endl: flush so the readiness line appears immediately even when
    // stdout is redirected to a file or pipe.
    std::cout << "gateway: listening on " << config_.host << ':' << bound_port_ << std::endl;
    return serve();
}

bool GatewayServer::wait_until_ready() {
    http_->wait_until_ready();
    return http_->is_running();
}

void GatewayServer::stop() { http_->stop(); }

}  // namespace gateway
