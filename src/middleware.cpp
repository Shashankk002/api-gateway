#include "gateway/middleware.hpp"

#include <httplib.h>

#include <atomic>
#include <iostream>
#include <random>
#include <utility>

namespace gateway {
namespace {

/// Status httplib will actually send: a handled route that set no status
/// defaults to 200.
int effective_status(const httplib::Response& response) {
    return response.status > 0 ? response.status : httplib::StatusCode::OK_200;
}

void append_hex(std::string& out, std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += kDigits[(value >> shift) & 0xF];
    }
}

}  // namespace

void Pipeline::use(std::unique_ptr<Middleware> middleware) {
    if (middleware != nullptr) {
        middleware_.push_back(std::move(middleware));
    }
}

void Pipeline::run(RequestContext& context, const Handler& terminal) const {
    // Walks the chain by index. Nothing is stored on the pipeline, so concurrent
    // requests share only the immutable middleware list.
    const auto invoke = [this, &context, &terminal](auto& self, std::size_t index) -> void {
        if (index >= middleware_.size()) {
            terminal(context);
            return;
        }
        middleware_[index]->handle(context, [&self, index] { self(self, index + 1); });
    };
    invoke(invoke, 0);
}

std::string RequestIdMiddleware::generate() {
    // Per-thread generator: no lock, and no two threads share a stream. The
    // counter keeps seeds distinct even if random_device repeats.
    static std::atomic<std::uint64_t> sequence{0};
    thread_local std::mt19937_64 engine([] {
        std::random_device device;
        const std::uint64_t entropy = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        return entropy ^ (sequence.fetch_add(1, std::memory_order_relaxed) * 0x9e3779b97f4a7c15ULL);
    }());

    std::string id;
    id.reserve(32);
    append_hex(id, engine());
    append_hex(id, engine());
    return id;
}

void RequestIdMiddleware::handle(RequestContext& context, const Next& next) {
    context.request_id = generate();
    next();
    // Set afterwards: the proxy replaces the whole header map when it copies a
    // backend response, so anything written before next() would be lost.
    context.response.set_header(kHeader, context.request_id);
}

void StderrLogSink::write(std::string_view line) {
    // One insertion of one composed string, as the health checker does.
    std::cerr << std::string(line) + "\n";
}

LoggingMiddleware::LoggingMiddleware(std::shared_ptr<LogSink> sink) : sink_(std::move(sink)) {}

void LoggingMiddleware::handle(RequestContext& context, const Next& next) {
    const auto started = std::chrono::steady_clock::now();

    const auto log = [this, &context, started](int status) {
        if (sink_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);

        std::string_view target = context.request.target;
        const bool truncated = target.size() > kMaxLoggedTarget;
        if (truncated) {
            target = target.substr(0, kMaxLoggedTarget);
        }

        std::string line = "gateway: request ";
        line += context.request_id;
        line += ' ';
        line += context.request.method;
        line += ' ';
        line.append(target);
        if (truncated) {
            line += "...";
        }
        line += " -> ";
        line += std::to_string(status);
        line += ' ';
        line += std::to_string(elapsed.count());
        line += "ms";
        sink_->write(line);
    };

    try {
        next();
    } catch (...) {
        // Record the outcome, then let httplib's exception handler produce the
        // gateway's normal JSON 500.
        log(httplib::StatusCode::InternalServerError_500);
        throw;
    }
    log(effective_status(context.response));
}

void CapturingLogSink::write(std::string_view line) {
    const std::lock_guard<std::mutex> guard(mutex_);
    lines_.emplace_back(line);
}

std::vector<std::string> CapturingLogSink::lines() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return lines_;
}

std::size_t CapturingLogSink::count() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return lines_.size();
}

void CapturingLogSink::clear() {
    const std::lock_guard<std::mutex> guard(mutex_);
    lines_.clear();
}

}  // namespace gateway
