#include "gateway/proxy.hpp"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gateway {
namespace {

// Hop-by-hop (RFC 9110 7.6.1), plus headers httplib regenerates: Host from the
// backend address, Content-Length from the forwarded body. Proxy-Connection is
// not in the RFC but is hop-by-hop wherever it is used.
constexpr std::string_view kSkippedRequestHeaders[] = {
    "connection", "keep-alive",       "proxy-authenticate", "proxy-authorization",
    "te",         "trailer",          "transfer-encoding",  "upgrade",
    "host",       "content-length",   "proxy-connection",
};

// Content-Length and Content-Type are set when the body is copied back.
constexpr std::string_view kSkippedResponseHeaders[] = {
    "connection", "keep-alive",     "proxy-authenticate", "proxy-authorization",
    "te",         "trailer",        "transfer-encoding",  "upgrade",
    "date",       "content-length", "content-type",       "proxy-connection",
};

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

/// RFC 9110 7.6.1: Connection also *names* further headers that apply only to
/// this hop, so the set is per message and cannot live in the constant lists
/// above. A sender that lists a header it did not send is harmless here.
std::vector<std::string> connection_named(const httplib::Headers& headers) {
    std::vector<std::string> named;
    const auto range = headers.equal_range("Connection");
    for (auto entry = range.first; entry != range.second; ++entry) {
        std::string_view remaining = entry->second;
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            std::string_view token = remaining.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
                token.remove_prefix(1);
            }
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
                token.remove_suffix(1);
            }
            if (!token.empty()) {
                named.emplace_back(token);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
    }
    return named;
}

bool is_skipped(std::span<const std::string_view> skipped,
                std::span<const std::string> also_skipped, std::string_view name) {
    const auto matches = [name](std::string_view entry) {
        return equals_ignore_case(entry, name);
    };
    return std::any_of(skipped.begin(), skipped.end(), matches) ||
           std::any_of(also_skipped.begin(), also_skipped.end(), matches);
}

httplib::Headers forwardable(const httplib::Headers& headers,
                             std::span<const std::string_view> skipped,
                             std::span<const std::string> also_skipped) {
    httplib::Headers result;
    for (const auto& [name, value] : headers) {
        if (!is_skipped(skipped, also_skipped, name)) {
            result.emplace(name, value);
        }
    }
    return result;
}

// httplib reports a read/write timeout as a plain transport error, so those map
// to a timeout here; a refused connection is reported distinctly.
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

ReverseProxy::ReverseProxy(std::chrono::milliseconds timeout) : timeout_(timeout) {}

ProxyStatus ReverseProxy::forward(const BackendEndpoint& backend, const httplib::Request& request,
                                  httplib::Response& response) const {
    httplib::Client client(backend.host, backend.port);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(timeout_);
    client.set_write_timeout(timeout_);
    client.set_keep_alive(false);

    httplib::Request outbound;
    outbound.method = request.method;
    // request.target is the raw request-target, so path and query survive as sent.
    outbound.path = request.target.empty() ? request.path : request.target;
    outbound.headers =
        forwardable(request.headers, kSkippedRequestHeaders, connection_named(request.headers));
    outbound.body = request.body;

    const httplib::Result result = client.send(outbound);
    if (!result) {
        return classify(result.error());
    }

    response.status = result->status;
    const std::vector<std::string> named = connection_named(result->headers);
    for (const auto& [name, value] : result->headers) {
        if (!is_skipped(kSkippedResponseHeaders, named, name)) {
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
