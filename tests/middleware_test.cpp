// Unit tests for the pipeline, request ids and logging. A minimal httplib
// Request/Response pair stands in for a real connection; no socket is opened.

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <string>
#include <thread>
#include <vector>

#include "gateway/middleware.hpp"

namespace {

using gateway::CapturingLogSink;
using gateway::Handler;
using gateway::LoggingMiddleware;
using gateway::Middleware;
using gateway::Next;
using gateway::Pipeline;
using gateway::RequestContext;
using gateway::RequestIdMiddleware;

/// Appends markers around `next`, so ordering is observable from the outside.
class TracingMiddleware final : public Middleware {
public:
    TracingMiddleware(std::string name, std::vector<std::string>& trace)
        : name_(std::move(name)), trace_(trace) {}

    void handle(RequestContext& context, const Next& next) override {
        trace_.push_back(name_ + ":before");
        next();
        trace_.push_back(name_ + ":after");
        context.response.set_header("X-Seen-By-" + name_, "yes");
    }

private:
    std::string name_;
    std::vector<std::string>& trace_;
};

/// Answers without calling `next`, so nothing further runs.
class ShortCircuitMiddleware final : public Middleware {
public:
    explicit ShortCircuitMiddleware(std::vector<std::string>& trace) : trace_(trace) {}

    void handle(RequestContext& context, const Next&) override {
        trace_.emplace_back("short-circuit");
        context.response.status = 403;
        context.response.set_content(R"({"stopped":true})", "application/json");
    }

private:
    std::vector<std::string>& trace_;
};

/// Rewrites the status the inner chain produced.
class StatusRewritingMiddleware final : public Middleware {
public:
    void handle(RequestContext& context, const Next& next) override {
        next();
        if (context.response.status == 418) {
            context.response.status = 200;
        }
    }
};

struct Exchange {
    httplib::Request request;
    httplib::Response response;
    RequestContext context{request, response};
};

TEST(PipelineTest, EmptyPipelineRunsOnlyTheTerminalHandler) {
    const Pipeline pipeline;
    Exchange exchange;
    int terminal_calls = 0;

    pipeline.run(exchange.context, [&terminal_calls](RequestContext& context) {
        ++terminal_calls;
        context.response.status = 200;
    });

    EXPECT_EQ(pipeline.size(), 0U);
    EXPECT_EQ(terminal_calls, 1);
    EXPECT_EQ(exchange.response.status, 200);
}

TEST(PipelineTest, MiddlewareRunsOutsideInThenInsideOut) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    pipeline.use(std::make_unique<TracingMiddleware>("A", trace));
    pipeline.use(std::make_unique<TracingMiddleware>("B", trace));

    Exchange exchange;
    pipeline.run(exchange.context, [&trace](RequestContext&) { trace.emplace_back("terminal"); });

    EXPECT_EQ(trace, (std::vector<std::string>{"A:before", "B:before", "terminal", "B:after",
                                               "A:after"}));
}

TEST(PipelineTest, TerminalHandlerRunsExactlyOnce) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    for (int i = 0; i < 4; ++i) {
        pipeline.use(std::make_unique<TracingMiddleware>("m" + std::to_string(i), trace));
    }

    Exchange exchange;
    int terminal_calls = 0;
    pipeline.run(exchange.context, [&terminal_calls](RequestContext&) { ++terminal_calls; });

    EXPECT_EQ(terminal_calls, 1);
    EXPECT_EQ(pipeline.size(), 4U);
}

TEST(PipelineTest, EarlyTerminationSkipsEverythingDownstream) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    pipeline.use(std::make_unique<TracingMiddleware>("outer", trace));
    pipeline.use(std::make_unique<ShortCircuitMiddleware>(trace));
    pipeline.use(std::make_unique<TracingMiddleware>("inner", trace));

    Exchange exchange;
    bool terminal_ran = false;
    pipeline.run(exchange.context, [&terminal_ran](RequestContext&) { terminal_ran = true; });

    EXPECT_FALSE(terminal_ran) << "the terminal handler must not run after a short circuit";
    EXPECT_EQ(trace, (std::vector<std::string>{"outer:before", "short-circuit", "outer:after"}))
        << "the inner middleware must be skipped entirely";
    EXPECT_EQ(exchange.response.status, 403);
}

TEST(PipelineTest, OuterMiddlewareStillRunsAfterAShortCircuit) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    pipeline.use(std::make_unique<TracingMiddleware>("outer", trace));
    pipeline.use(std::make_unique<ShortCircuitMiddleware>(trace));

    Exchange exchange;
    pipeline.run(exchange.context, [](RequestContext&) {});

    EXPECT_EQ(exchange.response.get_header_value("X-Seen-By-outer"), "yes");
}

TEST(PipelineTest, MiddlewareCanObserveAndModifyTheDownstreamResponse) {
    Pipeline pipeline;
    pipeline.use(std::make_unique<StatusRewritingMiddleware>());

    Exchange exchange;
    pipeline.run(exchange.context, [](RequestContext& context) { context.response.status = 418; });

    EXPECT_EQ(exchange.response.status, 200) << "the middleware should have rewritten it";
}

TEST(PipelineTest, ExceptionsFromTheTerminalHandlerPropagate) {
    Pipeline pipeline;
    pipeline.use(std::make_unique<StatusRewritingMiddleware>());

    Exchange exchange;
    EXPECT_THROW(pipeline.run(exchange.context,
                              [](RequestContext&) { throw std::runtime_error("boom"); }),
                 std::runtime_error);
}

TEST(PipelineTest, ContextCarriesStateBetweenMiddlewareAndTerminal) {
    Pipeline pipeline;
    pipeline.use(std::make_unique<RequestIdMiddleware>());

    Exchange exchange;
    std::string seen_by_terminal;
    pipeline.run(exchange.context, [&seen_by_terminal](RequestContext& context) {
        seen_by_terminal = context.request_id;
    });

    EXPECT_FALSE(seen_by_terminal.empty());
    EXPECT_EQ(seen_by_terminal, exchange.response.get_header_value("X-Request-Id"));
}

// ------------------------------------------------------------- request ids

TEST(RequestIdTest, GeneratedIdsAreHexAndFixedWidth) {
    const std::string id = RequestIdMiddleware::generate();

    EXPECT_EQ(id.size(), 32U) << "128 bits as hex";
    for (const char c : id) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "unexpected char: " << c;
    }
}

TEST(RequestIdTest, EveryRequestGetsAnIdReturnedInTheResponse) {
    Pipeline pipeline;
    pipeline.use(std::make_unique<RequestIdMiddleware>());

    std::set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        Exchange exchange;
        pipeline.run(exchange.context, [](RequestContext&) {});
        const std::string id = exchange.response.get_header_value("X-Request-Id");
        ASSERT_FALSE(id.empty());
        ids.insert(id);
    }

    EXPECT_EQ(ids.size(), 100U) << "sequential requests must not reuse ids";
}

TEST(RequestIdTest, AnInboundRequestIdIsNotAdopted) {
    Pipeline pipeline;
    pipeline.use(std::make_unique<RequestIdMiddleware>());

    Exchange exchange;
    exchange.request.set_header("X-Request-Id", "client-supplied-value");
    pipeline.run(exchange.context, [](RequestContext&) {});

    EXPECT_NE(exchange.response.get_header_value("X-Request-Id"), "client-supplied-value")
        << "a client-supplied id must not be echoed back as the gateway's";
    EXPECT_EQ(exchange.response.get_header_value("X-Request-Id").size(), 32U);
}

TEST(RequestIdTest, ConcurrentRequestsGetDistinctIds) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::vector<std::string>> per_thread(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&per_thread, t] {
            per_thread[static_cast<std::size_t>(t)].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                per_thread[static_cast<std::size_t>(t)].push_back(
                    RequestIdMiddleware::generate());
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::set<std::string> unique;
    for (const auto& ids : per_thread) {
        unique.insert(ids.begin(), ids.end());
    }

    EXPECT_EQ(unique.size(), static_cast<std::size_t>(kThreads) * kPerThread)
        << "ids collided across threads";
}

// ----------------------------------------------------------------- logging

TEST(LoggingMiddlewareTest, LogsIdMethodTargetAndStatus) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<RequestIdMiddleware>());
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    Exchange exchange;
    exchange.request.method = "GET";
    exchange.request.target = "/users/42?page=2";
    std::string request_id;
    pipeline.run(exchange.context, [&request_id](RequestContext& context) {
        request_id = context.request_id;
        context.response.status = 200;
    });

    ASSERT_EQ(sink->count(), 1U);
    const std::string line = sink->lines().front();
    EXPECT_NE(line.find(request_id), std::string::npos) << line;
    EXPECT_NE(line.find("GET"), std::string::npos) << line;
    EXPECT_NE(line.find("/users/42?page=2"), std::string::npos) << line;
    EXPECT_NE(line.find("-> 200"), std::string::npos) << line;
    EXPECT_NE(line.find("ms"), std::string::npos) << line;
}

TEST(LoggingMiddlewareTest, LogsFailedOutcomesToo) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    for (const int status : {404, 405, 429, 502, 503, 504}) {
        Exchange exchange;
        exchange.request.method = "GET";
        exchange.request.target = "/x";
        pipeline.run(exchange.context,
                     [status](RequestContext& context) { context.response.status = status; });
    }

    ASSERT_EQ(sink->count(), 6U);
    const std::vector<std::string> lines = sink->lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        EXPECT_NE(lines[i].find("-> "), std::string::npos) << lines[i];
    }
    EXPECT_NE(lines[0].find("-> 404"), std::string::npos) << lines[0];
    EXPECT_NE(lines[5].find("-> 504"), std::string::npos) << lines[5];
}

TEST(LoggingMiddlewareTest, AHandledRequestWithNoExplicitStatusLogsAs200) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    Exchange exchange;
    exchange.request.method = "GET";
    exchange.request.target = "/health";
    pipeline.run(exchange.context,
                 [](RequestContext& context) { context.response.set_content("{}", "application/json"); });

    ASSERT_EQ(sink->count(), 1U);
    EXPECT_NE(sink->lines().front().find("-> 200"), std::string::npos)
        << sink->lines().front();
}

TEST(LoggingMiddlewareTest, RecordsTheOutcomeWhenTheHandlerThrowsAndRethrows) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    Exchange exchange;
    exchange.request.method = "GET";
    exchange.request.target = "/boom";

    EXPECT_THROW(pipeline.run(exchange.context,
                              [](RequestContext&) { throw std::runtime_error("boom"); }),
                 std::runtime_error)
        << "the exception must not be swallowed";

    ASSERT_EQ(sink->count(), 1U);
    EXPECT_NE(sink->lines().front().find("-> 500"), std::string::npos)
        << sink->lines().front();
}

TEST(LoggingMiddlewareTest, LongTargetsAreTruncated) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    Exchange exchange;
    exchange.request.method = "GET";
    exchange.request.target = "/" + std::string(4000, 'a');
    pipeline.run(exchange.context, [](RequestContext& context) { context.response.status = 200; });

    ASSERT_EQ(sink->count(), 1U);
    const std::string line = sink->lines().front();
    EXPECT_LT(line.size(), LoggingMiddleware::kMaxLoggedTarget + 200U) << "line was not truncated";
    EXPECT_NE(line.find("..."), std::string::npos) << "truncation should be visible";
}

TEST(LoggingMiddlewareTest, CredentialsAreNeverLogged) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    Exchange exchange;
    exchange.request.method = "POST";
    exchange.request.target = "/users";
    exchange.request.set_header("Authorization", "Bearer super-secret-token");
    exchange.request.set_header("Cookie", "session=secret-cookie");
    exchange.request.body = R"({"password":"secret-body"})";
    pipeline.run(exchange.context, [](RequestContext& context) { context.response.status = 201; });

    ASSERT_EQ(sink->count(), 1U);
    const std::string line = sink->lines().front();
    EXPECT_EQ(line.find("super-secret-token"), std::string::npos) << line;
    EXPECT_EQ(line.find("secret-cookie"), std::string::npos) << line;
    EXPECT_EQ(line.find("secret-body"), std::string::npos) << line;
}

TEST(LoggingMiddlewareTest, ConcurrentRequestsEachProduceExactlyOneLine) {
    auto sink = std::make_shared<CapturingLogSink>();
    Pipeline pipeline;
    pipeline.use(std::make_unique<RequestIdMiddleware>());
    pipeline.use(std::make_unique<LoggingMiddleware>(sink));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pipeline] {
            for (int i = 0; i < kPerThread; ++i) {
                Exchange exchange;
                exchange.request.method = "GET";
                exchange.request.target = "/x";
                pipeline.run(exchange.context,
                             [](RequestContext& context) { context.response.status = 200; });
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(sink->count(), static_cast<std::size_t>(kThreads) * kPerThread);
}

}  // namespace
