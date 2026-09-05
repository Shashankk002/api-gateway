#include "gateway/metrics.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <utility>

namespace gateway {
namespace {

/// Separator for the joined series key. Not producible by any label value the
/// gateway emits, so keys cannot be confused with one another.
constexpr char kKeySeparator = '\x1f';

/// Prometheus escaping for a label value: backslash, quote and newline.
void append_escaped(std::string& out, std::string_view value) {
    for (const char character : value) {
        switch (character) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            default: out += character; break;
        }
    }
}

/// Compact representation Prometheus accepts for a float sample.
std::string format_double(double value) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%g", value);
    return written > 0 ? std::string(buffer.data(), static_cast<std::size_t>(written)) : "0";
}

void append_help_and_type(std::string& out, std::string_view name, std::string_view help,
                          std::string_view type) {
    out += "# HELP ";
    out.append(name);
    out += ' ';
    out.append(help);
    out += "\n# TYPE ";
    out.append(name);
    out += ' ';
    out.append(type);
    out += '\n';
}

void render_counter(std::string& out, std::string_view name, std::string_view help,
                    std::uint64_t value) {
    append_help_and_type(out, name, help, "counter");
    out.append(name);
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

void render_gauge(std::string& out, std::string_view name, std::string_view help,
                  std::int64_t value) {
    append_help_and_type(out, name, help, "gauge");
    out.append(name);
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

void render_histogram(std::string& out, std::string_view name, std::string_view help,
                      const Histogram& histogram) {
    append_help_and_type(out, name, help, "histogram");

    const std::vector<std::uint64_t> counts = histogram.cumulative_counts();
    const std::vector<double>& bounds = histogram.bounds();
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        out.append(name);
        out += "_bucket{le=\"";
        out += format_double(bounds[i]);
        out += "\"} ";
        out += std::to_string(counts[i]);
        out += '\n';
    }
    out.append(name);
    out += "_bucket{le=\"+Inf\"} ";
    out += std::to_string(histogram.count());
    out += '\n';

    out.append(name);
    out += "_count ";
    out += std::to_string(histogram.count());
    out += '\n';
    out.append(name);
    out += "_sum ";
    out += format_double(histogram.sum());
    out += '\n';
}

}  // namespace

std::vector<double> Histogram::default_bounds() {
    return {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
}

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)), counts_(bounds_.size()) {}

void Histogram::observe(double seconds) noexcept {
    // First bound that is >= the value, matching Prometheus's "le" semantics.
    const auto bucket = std::lower_bound(bounds_.begin(), bounds_.end(), seconds);
    if (bucket != bounds_.end()) {
        counts_[static_cast<std::size_t>(bucket - bounds_.begin())].fetch_add(
            1, std::memory_order_relaxed);
    }
    count_.fetch_add(1, std::memory_order_relaxed);

    // No fetch_add for doubles that is portable here, so accumulate by CAS.
    double current = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(current, current + seconds, std::memory_order_relaxed)) {
    }
}

std::vector<std::uint64_t> Histogram::cumulative_counts() const {
    std::vector<std::uint64_t> cumulative;
    cumulative.reserve(counts_.size());
    std::uint64_t running = 0;
    for (const auto& count : counts_) {
        running += count.load(std::memory_order_relaxed);
        cumulative.push_back(running);
    }
    return cumulative;
}

LabeledCounter::LabeledCounter(std::string name, std::string help,
                               std::vector<std::string> label_names)
    : name_(std::move(name)), help_(std::move(help)), label_names_(std::move(label_names)) {}

std::string LabeledCounter::key_for(const std::vector<std::string_view>& values) const {
    std::string key;
    for (const std::string_view value : values) {
        if (!key.empty()) {
            key += kKeySeparator;
        }
        key.append(value);
    }
    return key;
}

void LabeledCounter::increment(const std::vector<std::string_view>& label_values,
                               std::uint64_t amount) {
    if (label_values.size() != label_names_.size()) {
        return;  // Programming error; never let it corrupt the exposition.
    }

    const std::string key = key_for(label_values);
    {
        const std::shared_lock<std::shared_mutex> guard(mutex_);
        const auto found = series_.find(key);
        if (found != series_.end()) {
            found->second->counter.increment(amount);
            return;
        }
    }

    const std::unique_lock<std::shared_mutex> guard(mutex_);
    auto found = series_.find(key);
    if (found == series_.end()) {
        if (series_.size() >= kMaxSeries) {
            // Backstop against unbounded cardinality: everything beyond the cap
            // is aggregated into one series instead of creating more.
            const std::vector<std::string_view> other(label_names_.size(), "other");
            auto& overflow = series_[key_for(other)];
            if (overflow == nullptr) {
                overflow = std::make_unique<Series>();
                overflow->values.assign(label_names_.size(), "other");
            }
            overflow->counter.increment(amount);
            return;
        }
        auto series = std::make_unique<Series>();
        series->values.reserve(label_values.size());
        for (const std::string_view value : label_values) {
            series->values.emplace_back(value);
        }
        found = series_.emplace(key, std::move(series)).first;
    }
    found->second->counter.increment(amount);
}

std::uint64_t LabeledCounter::value(const std::vector<std::string_view>& label_values) const {
    const std::string key = key_for(label_values);
    const std::shared_lock<std::shared_mutex> guard(mutex_);
    const auto found = series_.find(key);
    return found == series_.end() ? 0 : found->second->counter.value();
}

std::uint64_t LabeledCounter::total() const {
    const std::shared_lock<std::shared_mutex> guard(mutex_);
    std::uint64_t sum = 0;
    for (const auto& [key, series] : series_) {
        sum += series->counter.value();
    }
    return sum;
}

std::size_t LabeledCounter::series_count() const {
    const std::shared_lock<std::shared_mutex> guard(mutex_);
    return series_.size();
}

void LabeledCounter::render(std::string& out) const {
    append_help_and_type(out, name_, help_, "counter");

    const std::shared_lock<std::shared_mutex> guard(mutex_);
    for (const auto& [key, series] : series_) {
        out += name_;
        out += '{';
        for (std::size_t i = 0; i < label_names_.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            out += label_names_[i];
            out += "=\"";
            append_escaped(out, series->values[i]);
            out += '"';
        }
        out += "} ";
        out += std::to_string(series->counter.value());
        out += '\n';
    }
}

MetricsRegistry::MetricsRegistry(const BackendTable& backends)
    : requests("gateway_requests_total", "Client requests handled, by method and final status.",
               {"method", "status"}),
      circuit_opens("gateway_circuit_opens_total",
                    "Times a backend instance's circuit moved into the open state.", {"service"}),
      circuit_rejected("gateway_circuit_rejected_requests_total",
                       "Requests answered 503 because every circuit was refusing traffic.",
                       {"service"}),
      backend_requests("gateway_backend_requests_total",
                       "Attempts made against a backend instance, retries included.", {"service"}),
      backend_failures("gateway_backend_failures_total",
                       "Backend attempts that failed transiently.", {"service"}),
      backend_timeouts("gateway_backend_timeouts_total", "Backend attempts that timed out.",
                       {"service"}),
      health_failures("gateway_backend_health_failures_total",
                      "Backend instances that a health probe moved to unhealthy.", {"service"}),
      health_recoveries("gateway_backend_health_recoveries_total",
                        "Backend instances that a health probe moved back to healthy.",
                        {"service"}) {
    // Touch every configured service so a scrape shows it at zero rather than
    // omitting it until the first event.
    for (const auto& [service, instances] : backends) {
        const std::vector<std::string_view> labels{service};
        circuit_opens.increment(labels, 0);
        circuit_rejected.increment(labels, 0);
        backend_requests.increment(labels, 0);
        backend_failures.increment(labels, 0);
        backend_timeouts.increment(labels, 0);
        health_failures.increment(labels, 0);
        health_recoveries.increment(labels, 0);
    }
}

std::string_view MetricsRegistry::normalize_method(std::string_view method) {
    // httplib already rejects anything outside its own method set, so this is
    // defence in depth that also keeps the label set fixed and documented.
    static constexpr std::string_view kKnown[] = {"GET",     "HEAD",    "POST",  "PUT",  "DELETE",
                                                  "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    const auto found = std::find(std::begin(kKnown), std::end(kKnown), method);
    return found != std::end(kKnown) ? *found : std::string_view("other");
}

void MetricsRegistry::observe_request(std::string_view method, int status, double seconds) {
    const std::string status_text = std::to_string(status);
    requests.increment({normalize_method(method), status_text});
    request_duration.observe(seconds);
}

std::string MetricsRegistry::render() const {
    std::string out;
    out.reserve(4096);

    requests.render(out);
    render_histogram(out, "gateway_request_duration_seconds",
                     "Client request latency in seconds.", request_duration);
    render_gauge(out, "gateway_requests_in_flight", "Client requests currently being handled.",
                 requests_in_flight.value());

    render_counter(out, "gateway_rate_limited_requests_total",
                   "Requests rejected by the rate limiter.", rate_limited.value());
    render_counter(out, "gateway_retry_attempts_total",
                   "Backend attempts beyond the first for a single request.",
                   retry_attempts.value());
    render_counter(out, "gateway_retried_requests_total",
                   "Requests that needed at least one retry.", retried_requests.value());

    circuit_opens.render(out);
    circuit_rejected.render(out);

    backend_requests.render(out);
    backend_failures.render(out);
    backend_timeouts.render(out);
    render_histogram(out, "gateway_backend_duration_seconds",
                     "Backend attempt latency in seconds.", backend_duration);

    health_failures.render(out);
    health_recoveries.render(out);
    return out;
}

MetricsMiddleware::MetricsMiddleware(MetricsRegistry& metrics, std::string excluded_path)
    : metrics_(metrics), excluded_path_(std::move(excluded_path)) {}

void MetricsMiddleware::handle(RequestContext& context, const Next& next) {
    if (context.request.path == excluded_path_) {
        next();  // Scraping must not move the numbers being scraped.
        return;
    }

    /// Keeps the gauge balanced across early returns and exceptions alike.
    struct InFlightGuard {
        explicit InFlightGuard(Gauge& gauge) : gauge_(gauge) { gauge_.increment(); }
        ~InFlightGuard() { gauge_.decrement(); }
        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
        Gauge& gauge_;
    };

    const InFlightGuard guard(metrics_.requests_in_flight);
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_seconds = [started] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };

    try {
        next();
    } catch (...) {
        metrics_.observe_request(context.request.method,
                                 httplib::StatusCode::InternalServerError_500, elapsed_seconds());
        throw;
    }

    // A handled route that set no status is httplib's implicit 200.
    const int status =
        context.response.status > 0 ? context.response.status : httplib::StatusCode::OK_200;
    metrics_.observe_request(context.request.method, status, elapsed_seconds());
}

}  // namespace gateway
