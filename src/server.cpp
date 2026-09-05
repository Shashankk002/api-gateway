#include "gateway/server.hpp"

#include <httplib.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/health.hpp"
#include "gateway/metrics.hpp"
#include "gateway/middleware.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/redis_rate_limiter.hpp"
#include "gateway/load_balancer.hpp"
#include "gateway/proxy.hpp"
#include "gateway/router.hpp"

namespace gateway {
namespace {

constexpr const char* kJsonContentType = "application/json";

// Owned by the gateway itself: never offered to the Router, so no route table
// can shadow or take over the health endpoint.
constexpr const char* kHealthPath = "/health";
constexpr const char* kMetricsPath = "/metrics";

// Matched by httplib as a regex, so it covers every path. Registered after the
// gateway's own endpoints, which therefore win.
constexpr const char* kCatchAllPattern = ".*";

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

/// Reports an error the gateway itself produced while proxying.
void respond_gateway_error(httplib::Response& response, int status, std::string_view error,
                           std::string_view reason, std::string_view service) {
    std::string body = R"({"status":"error","error":)";
    append_json_string(body, error);
    body += R"(,"reason":)";
    append_json_string(body, reason);
    body += R"(,"service":)";
    append_json_string(body, service);
    body += '}';

    response.status = status;
    response.set_content(body, kJsonContentType);
    std::cerr << "gateway: " << reason << " for service '" << service << "'\n";
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

/// Retry policy, part one: only methods that are safe to repeat. A connection
/// failure means the backend probably never saw the request, but the gateway
/// cannot tell that apart from a failure after the backend acted, so unsafe
/// methods are never replayed.
bool is_retryable_method(std::string_view method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

/// Retry policy, part two: which outcomes are transient. The same predicate
/// decides what counts as a circuit-breaker failure, so the two never disagree.
/// A backend's own 4xx, or a 500 it chose to return, is an answer rather than a
/// transport problem and is passed straight through.
bool is_transient_failure(ProxyStatus status, int backend_status) {
    if (status != ProxyStatus::kForwarded) {
        return true;  // Unreachable or timed out.
    }
    return backend_status == httplib::StatusCode::BadGateway_502 ||
           backend_status == httplib::StatusCode::ServiceUnavailable_503 ||
           backend_status == httplib::StatusCode::GatewayTimeout_504;
}

/// Copies an accepted attempt onto the response the client will receive.
void apply_attempt(const httplib::Response& attempt, httplib::Response& response) {
    response.status = attempt.status;
    response.headers = attempt.headers;
    response.body = attempt.body;
}

/// Builds the limiter the configuration asks for, or nothing when rate limiting
/// is off. "requests per window" maps onto the token bucket as a capacity of
/// that many refilled over the same window.
std::unique_ptr<RateLimiter> make_rate_limiter(const ServerConfig& config) {
    if (!config.rate_limit_enabled) {
        return nullptr;
    }
    const double per_second = static_cast<double>(config.rate_limit_requests) /
                              (static_cast<double>(config.rate_limit_window.count()) / 1000.0);

    if (config.rate_limit_mode == RateLimitMode::kRedis) {
        return std::make_unique<RedisRateLimiter>(
            config.redis_host, config.redis_port, config.redis_key_prefix,
            config.rate_limit_requests, config.rate_limit_window, config.redis_failure_policy);
    }
    if (config.rate_limit_algorithm == RateLimitAlgorithm::kSlidingWindow) {
        return std::make_unique<SlidingWindowLimiter>(config.rate_limit_requests,
                                                      config.rate_limit_window);
    }
    return std::make_unique<TokenBucketLimiter>(config.rate_limit_requests, per_second);
}

/// Advertises the caller's standing. Applied to allowed responses too, after
/// the proxy has written the backend's headers, so it is not overwritten.
void apply_rate_limit_headers(httplib::Response& response, const RateLimitDecision& decision) {
    response.set_header("RateLimit-Limit", std::to_string(decision.limit));
    response.set_header("RateLimit-Remaining", std::to_string(decision.remaining));
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

GatewayServer::GatewayServer(ServerConfig config, Router router,
                             std::shared_ptr<LogSink> log_sink)
    : config_(std::move(config)),
      metrics_(config_.backends),
      limiter_(make_rate_limiter(config_)),
      health_(config_.backends),
      breakers_(config_.backends, config_.circuit_failure_threshold, config_.circuit_cooldown),
      router_(std::move(router)),
      balancer_(config_.backends, health_, breakers_),
      proxy_(config_.backend_timeout),
      checker_(config_.backends, health_, config_.health_check_interval, config_.backend_timeout,
               &metrics_),
      http_(std::make_unique<httplib::Server>()) {
    // Outermost first: the id exists before anything logs it, and logging sees
    // whatever status the rest of the pipeline finally settles on.
    pipeline_.use(std::make_unique<RequestIdMiddleware>());
    pipeline_.use(std::make_unique<MetricsMiddleware>(metrics_, kMetricsPath));
    pipeline_.use(std::make_unique<LoggingMiddleware>(
        log_sink != nullptr ? std::move(log_sink) : std::make_shared<StderrLogSink>()));

    register_routes();
    checker_.start();
}

GatewayServer::~GatewayServer() = default;

void GatewayServer::run_pipeline(const httplib::Request& request, httplib::Response& response,
                                 const Handler& terminal) const {
    RequestContext context(request, response);
    context.client_key = client_key(request);
    pipeline_.run(context, terminal);
}

void GatewayServer::handle_service_request(RequestContext& context) const {
    const httplib::Request& request = context.request;
    httplib::Response& response = context.response;
    const RouteMatch match = router_.match(request.method, request.path);

    switch (match.status) {
        case MatchStatus::kMatched: {
            if (limiter_ == nullptr) {
                dispatch_to_service(match.route->service, request, response);
                return;
            }

            // Runs before any backend machinery: a rejected request never picks
            // an instance, never touches a circuit and never spends retry budget.
            const RateLimitDecision decision =
                limiter_->acquire(context.client_key, RateLimiter::Clock::now());
            if (!decision.allowed) {
                metrics_.rate_limited.increment();
                respond_gateway_error(response, httplib::StatusCode::TooManyRequests_429,
                                      "rate_limited", "rate_limit_exceeded",
                                      match.route->service);
                apply_rate_limit_headers(response, decision);
                if (decision.retry_after.count() > 0) {
                    const auto seconds = (decision.retry_after.count() + 999) / 1000;
                    response.set_header("Retry-After", std::to_string(seconds));
                }
                return;
            }

            dispatch_to_service(match.route->service, request, response);
            apply_rate_limit_headers(response, decision);
            return;
        }

        case MatchStatus::kMethodNotAllowed:
            response.status = httplib::StatusCode::MethodNotAllowed_405;
            response.set_header("Allow", allow_header_value(match.allowed_methods));
            response.set_content(method_not_allowed_body(match.allowed_methods),
                                 kJsonContentType);
            return;

        case MatchStatus::kNotFound:
            response.status = httplib::StatusCode::NotFound_404;
            response.set_content(kNotFoundBody, kJsonContentType);
            return;
    }
}

void GatewayServer::dispatch_to_service(const std::string& service,
                                       const httplib::Request& request,
                                       httplib::Response& response) const {
    const std::size_t max_forwards =
        is_retryable_method(request.method) ? config_.max_retries + 1U : 1U;

    // Every iteration marks one more instance as tried, so the loop ends once
    // the service runs out of instances even if the retry budget does not.
    std::vector<std::size_t> tried;
    std::size_t forwards = 0;
    bool refused_by_circuit = false;

    ProxyStatus last_status = ProxyStatus::kBackendUnreachable;
    httplib::Response last_attempt;

    while (forwards < max_forwards) {
        const LoadBalancer::Selection selection = balancer_.select(service, tried);
        if (!selection) {
            break;
        }
        tried.push_back(selection.index);

        CircuitBreaker* breaker = breakers_.find(service, selection.index);
        if (breaker != nullptr && !breaker->try_acquire()) {
            refused_by_circuit = true;
            continue;
        }

        const std::vector<std::string_view> service_label{service};
        metrics_.backend_requests.increment(service_label);
        if (forwards > 0) {
            // Everything past the first attempt is a retry.
            metrics_.retry_attempts.increment();
        }

        httplib::Response attempt;
        const auto attempt_started = std::chrono::steady_clock::now();
        const ProxyStatus status = proxy_.forward(*selection.endpoint, request, attempt);
        metrics_.backend_duration.observe(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - attempt_started)
                .count());

        const bool transient = is_transient_failure(status, attempt.status);
        if (transient) {
            metrics_.backend_failures.increment(service_label);
            if (status == ProxyStatus::kBackendTimeout) {
                metrics_.backend_timeouts.increment(service_label);
            }
        }
        if (breaker != nullptr) {
            if (transient) {
                if (breaker->record_failure()) {
                    metrics_.circuit_opens.increment(service_label);
                }
            } else {
                breaker->record_success();
            }
        }

        ++forwards;
        if (forwards == 2) {
            metrics_.retried_requests.increment();
        }
        if (!transient) {
            apply_attempt(attempt, response);
            return;
        }
        last_status = status;
        last_attempt = std::move(attempt);
    }

    if (forwards == 0) {
        // Nothing was contacted: either no instance is eligible at all, or the
        // only candidates were circuits refusing traffic.
        const auto circuit_is_blocking = [this, &service] {
            const BackendTable& table = balancer_.backends();
            const auto entry = table.find(service);
            if (entry == table.end()) {
                return false;
            }
            for (std::size_t index = 0; index < entry->second.size(); ++index) {
                if (breakers_.blocks_selection(service, index)) {
                    return true;
                }
            }
            return false;
        };

        if (refused_by_circuit || circuit_is_blocking()) {
            metrics_.circuit_rejected.increment({service});
            respond_gateway_error(response, httplib::StatusCode::ServiceUnavailable_503,
                                  "service_unavailable", "circuit_open", service);
        } else {
            respond_gateway_error(response, httplib::StatusCode::BadGateway_502, "bad_gateway",
                                  "no_backend_configured", service);
        }
        return;
    }

    switch (last_status) {
        case ProxyStatus::kBackendTimeout:
            respond_gateway_error(response, httplib::StatusCode::GatewayTimeout_504,
                                  "gateway_timeout", "backend_timeout", service);
            return;
        case ProxyStatus::kBackendUnreachable:
            respond_gateway_error(response, httplib::StatusCode::BadGateway_502, "bad_gateway",
                                  "backend_unreachable", service);
            return;
        case ProxyStatus::kForwarded:
            // The backend answered, just with a transient status. Its own
            // response is more informative than a gateway error would be.
            apply_attempt(last_attempt, response);
            return;
    }
}

std::string GatewayServer::client_key(const httplib::Request& request) {
    // The peer address the socket reports. X-Forwarded-For and friends are
    // ignored: without trusted-proxy configuration they are client-supplied and
    // would let anyone escape their own bucket.
    return request.remote_addr.empty() ? std::string("unknown") : request.remote_addr;
}

void GatewayServer::register_routes() {
    // /health stays gateway-owned and registered ahead of the catch-alls; it
    // runs through the pipeline only for the cross-cutting concerns, never
    // through routing, rate limiting or proxying.
    http_->Get(kHealthPath, [this](const httplib::Request& request, httplib::Response& response) {
        run_pipeline(request, response, [](RequestContext& context) {
            context.response.set_content(kHealthyBody, kJsonContentType);
        });
    });

    if (config_.metrics_enabled) {
        // Gateway-owned like /health: never routed, rate limited or proxied.
        http_->Get(kMetricsPath,
                   [this](const httplib::Request& request, httplib::Response& response) {
                       run_pipeline(request, response, [this](RequestContext& context) {
                           context.response.set_content(metrics_.render(),
                                                        MetricsRegistry::kContentType);
                       });
                   });
    }

    // Per-method catch-alls rather than a pre-routing handler: httplib reads the
    // request body only after routing, and the proxy needs it.
    const auto handler = [this](const httplib::Request& request, httplib::Response& response) {
        run_pipeline(request, response, [this](RequestContext& context) {
            if (context.request.path == kHealthPath ||
                (config_.metrics_enabled && context.request.path == kMetricsPath)) {
                // Gateway-owned. GET is served above; no other method is offered
                // to the router, so these can never reach a backend.
                context.response.status = httplib::StatusCode::NotFound_404;
                context.response.set_content(kNotFoundBody, kJsonContentType);
                return;
            }
            handle_service_request(context);
        });
    };
    http_->Get(kCatchAllPattern, handler);
    http_->Post(kCatchAllPattern, handler);
    http_->Put(kCatchAllPattern, handler);
    http_->Patch(kCatchAllPattern, handler);
    http_->Delete(kCatchAllPattern, handler);
    http_->Options(kCatchAllPattern, handler);

    // Safety net for statuses httplib produces on its own. Proxied responses
    // carry their backend's body and are left alone.
    http_->set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (response.status == httplib::StatusCode::NotFound_404 && response.body.empty()) {
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
    for (const auto& [service, instances] : balancer_.backends()) {
        for (const BackendEndpoint& instance : instances) {
            std::cout << "gateway: backend " << service << " -> " << instance.host << ':'
                      << instance.port << '\n';
        }
    }
    std::cout << "gateway: backend timeout " << proxy_.timeout().count() << "ms\n";
    std::cout << "gateway: max retries " << config_.max_retries << ", circuit opens after "
              << config_.circuit_failure_threshold << " failures for "
              << config_.circuit_cooldown.count() << "ms\n";
    if (limiter_ == nullptr) {
        std::cout << "gateway: rate limiting disabled\n";
    } else {
        std::cout << "gateway: rate limit "
                  << (config_.rate_limit_mode == RateLimitMode::kRedis ? "redis " : "local ")
                  << (config_.rate_limit_algorithm == RateLimitAlgorithm::kSlidingWindow
                          ? "sliding-window "
                          : "token-bucket ")
                  << config_.rate_limit_requests << " per " << config_.rate_limit_window.count()
                  << "ms per client\n";
    }
    if (config_.health_check_interval.count() > 0) {
        std::cout << "gateway: health checks every " << config_.health_check_interval.count()
                  << "ms\n";
    } else {
        std::cout << "gateway: health checks disabled\n";
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

void GatewayServer::stop() {
    http_->stop();
    checker_.stop();
}

}  // namespace gateway
