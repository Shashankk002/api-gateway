#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace httplib {
class Request;
class Response;
}  // namespace httplib

namespace gateway {

/// Per-request state shared between middleware and the terminal handler.
///
/// Built fresh for every request and never touched by another thread, so it
/// needs no synchronisation. Holds references rather than copies: the request
/// body is never duplicated.
struct RequestContext {
    RequestContext(const httplib::Request& http_request, httplib::Response& http_response)
        : request(http_request), response(http_response) {}

    RequestContext(const RequestContext&) = delete;
    RequestContext& operator=(const RequestContext&) = delete;

    const httplib::Request& request;
    httplib::Response& response;

    /// Correlation id for this request. Assigned by RequestIdMiddleware.
    std::string request_id;

    /// Who the request is attributed to for rate limiting, as the gateway
    /// determined it. Never taken from a client-supplied header.
    std::string client_key;
};

/// The terminal step of a pipeline: everything the gateway does for a request
/// once cross-cutting concerns have had their turn.
using Handler = std::function<void(RequestContext&)>;

/// The remainder of the pipeline, from one middleware's point of view.
using Next = std::function<void()>;

/// One cross-cutting concern.
///
/// A middleware runs before `next`, may skip calling it to end the request
/// early, and may inspect or adjust the response after it returns. Instances
/// are shared by concurrent requests, so any state they hold must be either
/// immutable or synchronised.
class Middleware {
public:
    Middleware() = default;
    virtual ~Middleware() = default;
    Middleware(const Middleware&) = delete;
    Middleware& operator=(const Middleware&) = delete;

    virtual void handle(RequestContext& context, const Next& next) = 0;
};

/// Composes middleware around a terminal handler, outermost first.
///
/// The pipeline itself is immutable once built: it stores no per-request state,
/// so one instance serves every concurrent request.
class Pipeline {
public:
    /// Adds a middleware. Later additions run further in.
    void use(std::unique_ptr<Middleware> middleware);

    /// Runs the chain for one request, ending in `terminal`.
    void run(RequestContext& context, const Handler& terminal) const;

    [[nodiscard]] std::size_t size() const noexcept { return middleware_.size(); }

private:
    std::vector<std::unique_ptr<Middleware>> middleware_;
};

/// Gives every request a correlation id and returns it to the client.
///
/// The id is always generated here. An inbound X-Request-Id is ignored: without
/// trusted-proxy configuration it is client-supplied, and the gateway already
/// declines to trust such headers for the rate-limit key. The id is for
/// correlation only and carries no authority.
class RequestIdMiddleware final : public Middleware {
public:
    static constexpr const char* kHeader = "X-Request-Id";

    void handle(RequestContext& context, const Next& next) override;

    /// A fresh 128-bit id as 32 lowercase hex characters. Thread-safe: each
    /// thread draws from its own generator.
    [[nodiscard]] static std::string generate();
};

/// Where request log lines go. Kept to one method so tests can capture output
/// without the logging implementation growing a framework.
class LogSink {
public:
    LogSink() = default;
    virtual ~LogSink() = default;
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    virtual void write(std::string_view line) = 0;
};

/// Writes to std::cerr, the channel the gateway already logs on.
class StderrLogSink final : public LogSink {
public:
    void write(std::string_view line) override;
};

/// Records one line per request: id, method, target, final status, duration.
///
/// Only the request line is logged. Headers and bodies are never touched, so
/// credentials cannot leak through it, and the target is truncated so one
/// enormous URL cannot dominate the log.
class LoggingMiddleware final : public Middleware {
public:
    static constexpr std::size_t kMaxLoggedTarget = 256;

    explicit LoggingMiddleware(std::shared_ptr<LogSink> sink);

    void handle(RequestContext& context, const Next& next) override;

private:
    std::shared_ptr<LogSink> sink_;
};

/// A sink that keeps lines in memory, for tests.
class CapturingLogSink final : public LogSink {
public:
    void write(std::string_view line) override;

    [[nodiscard]] std::vector<std::string> lines() const;
    [[nodiscard]] std::size_t count() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

}  // namespace gateway
