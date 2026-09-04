#include "gateway/proxy.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <utility>

namespace gateway {
namespace {

// Hop-by-hop headers (RFC 9110 7.6.1), plus headers httplib regenerates for the
// outbound request: Host comes from the backend address, Content-Length from the
// forwarded body.
constexpr std::string_view kSkippedRequestHeaders[] = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te",         "trailer",    "transfer-encoding",  "upgrade",
    "host",       "content-length",
};

// Content-Length and Content-Type are set when the body is copied back.
constexpr std::string_view kSkippedResponseHeaders[] = {
    "connection", "keep-alive",     "proxy-authenticate", "proxy-authorization",
    "te",         "trailer",        "transfer-encoding",  "upgrade",
    "date",       "content-length", "content-type",
};

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

bool is_skipped(std::span<const std::string_view> skipped, std::string_view name) {
    return std::any_of(skipped.begin(), skipped.end(),
                       [name](std::string_view entry) { return equals_ignore_case(entry, name); });
}

httplib::Headers forwardable(const httplib::Headers& headers,
                             std::span<const std::string_view> skipped) {
    httplib::Headers result;
    for (const auto& [name, value] : headers) {
        if (!is_skipped(skipped, name)) {
            result.emplace(name, value);
        }
    }
    return result;
}

// httplib reports a read/write timeout as a plain transport error, so those are
// treated as timeouts here; a refused connection is reported distinctly.
ProxyStatus classify(httplib::Error error) {
    switch (error) {
        case httplib::Error::ConnectionTimeout:
        case httplib::Error::Read:
        case httplib::Error::Write:
            return ProxyStatus::kBackendTimeout;
        default:
            return ProxyStatus::kBackendUnreachable;
    }
}

}  // namespace

ReverseProxy::ReverseProxy(BackendTable backends, std::chrono::milliseconds timeout)
    : backends_(std::move(backends)), timeout_(timeout) {}

ProxyStatus ReverseProxy::forward(std::string_view service, const httplib::Request& request,
                                  httplib::Response& response) const {
    const auto backend = backends_.find(service);
    if (backend == backends_.end()) {
        return ProxyStatus::kUnknownService;
    }

    httplib::Client client(backend->second.host, backend->second.port);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(timeout_);
    client.set_write_timeout(timeout_);
    client.set_keep_alive(false);

    httplib::Request outbound;
    outbound.method = request.method;
    // request.target is the raw request-target, so path and query survive as sent.
    outbound.path = request.target.empty() ? request.path : request.target;
    outbound.headers = forwardable(request.headers, kSkippedRequestHeaders);
    outbound.body = request.body;

    const httplib::Result result = client.send(outbound);
    if (!result) {
        return classify(result.error());
    }

    response.status = result->status;
    for (const auto& [name, value] : result->headers) {
        if (!is_skipped(kSkippedResponseHeaders, name)) {
            response.set_header(name, value);
        }
    }

    const std::string content_type = result->get_header_value("Content-Type");
    if (content_type.empty()) {
        response.body = result->body;  // Nothing to advertise; do not invent a type.
    } else {
        response.set_content(result->body, content_type);
    }
    return ProxyStatus::kForwarded;
}

}  // namespace gateway
