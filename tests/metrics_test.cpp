// Unit tests for the metric primitives and the Prometheus exposition. No
// sockets: values are recorded directly.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/metrics.hpp"

namespace {

using gateway::BackendEndpoint;
using gateway::BackendTable;
using gateway::Counter;
using gateway::Gauge;
using gateway::Histogram;
using gateway::LabeledCounter;
using gateway::MetricsRegistry;

BackendTable one_service() {
    return BackendTable{{"users", {BackendEndpoint{"127.0.0.1", 9001}}}};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// The value of the first sample line whose prefix matches, or -1.
long long sample(const std::string& exposition, const std::string& prefix) {
    std::size_t position = 0;
    while (position < exposition.size()) {
        const std::size_t end = exposition.find('\n', position);
        const std::string line = exposition.substr(position, end - position);
        if (line.rfind(prefix, 0) == 0 && line.size() > prefix.size() &&
            line[prefix.size()] == ' ') {
            return std::stoll(line.substr(prefix.size() + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    return -1;
}

TEST(CounterTest, StartsAtZeroAndAccumulates) {
    Counter counter;
    EXPECT_EQ(counter.value(), 0U);

    counter.increment();
    counter.increment(5);
    EXPECT_EQ(counter.value(), 6U);
}

TEST(CounterTest, ConcurrentIncrementsAreExact) {
    Counter counter;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&counter] {
            for (int i = 0; i < kPerThread; ++i) {
                counter.increment();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(counter.value(), static_cast<std::uint64_t>(kThreads) * kPerThread);
}

TEST(GaugeTest, GoesUpAndDownAndReturnsToZero) {
    Gauge gauge;
    gauge.increment();
    gauge.increment();
    EXPECT_EQ(gauge.value(), 2);

    gauge.decrement();
    gauge.decrement();
    EXPECT_EQ(gauge.value(), 0);
}

TEST(GaugeTest, BalancedConcurrentUseEndsAtZero) {
    Gauge gauge;
    constexpr int kThreads = 8;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&gauge] {
            for (int i = 0; i < 2000; ++i) {
                gauge.increment();
                gauge.decrement();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(gauge.value(), 0);
}

TEST(HistogramTest, SelectsBucketsByUpperBound) {
    Histogram histogram(std::vector<double>{0.1, 1.0});

    histogram.observe(0.05);  // <= 0.1
    histogram.observe(0.1);   // <= 0.1, boundary is inclusive
    histogram.observe(0.5);   // <= 1.0
    histogram.observe(7.0);   // only +Inf

    const std::vector<std::uint64_t> cumulative = histogram.cumulative_counts();
    ASSERT_EQ(cumulative.size(), 2U);
    EXPECT_EQ(cumulative[0], 2U);
    EXPECT_EQ(cumulative[1], 3U) << "buckets must be cumulative";
    EXPECT_EQ(histogram.count(), 4U) << "+Inf holds every observation";
}

TEST(HistogramTest, TracksCountAndSum) {
    Histogram histogram;
    histogram.observe(0.25);
    histogram.observe(0.75);

    EXPECT_EQ(histogram.count(), 2U);
    EXPECT_NEAR(histogram.sum(), 1.0, 1e-9);
}

TEST(HistogramTest, DefaultBoundsAreTheDocumentedGatewayBuckets) {
    EXPECT_EQ(Histogram::default_bounds(),
              (std::vector<double>{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0}));
}

TEST(HistogramTest, ConcurrentObservationsAreExact) {
    Histogram histogram;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&histogram] {
            for (int i = 0; i < kPerThread; ++i) {
                histogram.observe(0.001);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const auto expected = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(histogram.count(), expected);
    EXPECT_EQ(histogram.cumulative_counts().front(), expected);
    EXPECT_NEAR(histogram.sum(), static_cast<double>(expected) * 0.001, 0.1);
}

TEST(LabeledCounterTest, SeriesAreIndependent) {
    LabeledCounter counter("gateway_requests_total", "help", {"method", "status"});

    counter.increment({"GET", "200"});
    counter.increment({"GET", "200"});
    counter.increment({"POST", "404"});

    EXPECT_EQ(counter.value({"GET", "200"}), 2U);
    EXPECT_EQ(counter.value({"POST", "404"}), 1U);
    EXPECT_EQ(counter.value({"PUT", "200"}), 0U);
    EXPECT_EQ(counter.total(), 3U);
    EXPECT_EQ(counter.series_count(), 2U);
}

TEST(LabeledCounterTest, MismatchedLabelCountIsIgnored) {
    LabeledCounter counter("gateway_requests_total", "help", {"method", "status"});

    counter.increment({"GET"});
    EXPECT_EQ(counter.series_count(), 0U) << "a malformed call must not create a series";
}

TEST(LabeledCounterTest, SeriesCreationIsCappedSoLabelsCannotGrowUnbounded) {
    LabeledCounter counter("gateway_requests_total", "help", {"status"});

    for (int i = 0; i < 500; ++i) {
        counter.increment({std::to_string(i)});
    }

    EXPECT_LE(counter.series_count(), LabeledCounter::kMaxSeries + 1)
        << "the cap must bound the series count";
    EXPECT_EQ(counter.total(), 500U) << "capped increments must be aggregated, not dropped";

    std::string exposition;
    counter.render(exposition);
    EXPECT_TRUE(contains(exposition, R"(status="other")")) << "overflow needs a visible series";
}

TEST(LabeledCounterTest, LabelValuesAreEscapedForPrometheus) {
    LabeledCounter counter("gateway_test_total", "help", {"service"});
    counter.increment({R"(we"ird\value)"});
    counter.increment({"line\nbreak"});

    std::string exposition;
    counter.render(exposition);

    EXPECT_TRUE(contains(exposition, R"(service="we\"ird\\value")")) << exposition;
    EXPECT_TRUE(contains(exposition, R"(service="line\nbreak")")) << exposition;
    EXPECT_EQ(exposition.find("\nbreak"), std::string::npos)
        << "a raw newline would break the exposition";
}

TEST(LabeledCounterTest, ConcurrentIncrementsAcrossSeriesAreExact) {
    LabeledCounter counter("gateway_test_total", "help", {"method"});
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&counter, t] {
            const std::string method = (t % 2 == 0) ? "GET" : "POST";
            for (int i = 0; i < kPerThread; ++i) {
                counter.increment({method});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(counter.total(), static_cast<std::uint64_t>(kThreads) * kPerThread);
    EXPECT_EQ(counter.value({"GET"}), static_cast<std::uint64_t>(kThreads / 2) * kPerThread);
    EXPECT_EQ(counter.series_count(), 2U);
}

TEST(MetricsRegistryTest, UnknownMethodsFoldIntoASingleLabel) {
    EXPECT_EQ(MetricsRegistry::normalize_method("GET"), "GET");
    EXPECT_EQ(MetricsRegistry::normalize_method("PATCH"), "PATCH");
    EXPECT_EQ(MetricsRegistry::normalize_method("BREW"), "other");
    EXPECT_EQ(MetricsRegistry::normalize_method(""), "other");
}

TEST(MetricsRegistryTest, ConfiguredServicesAppearAtZeroBeforeAnyTraffic) {
    const MetricsRegistry metrics(one_service());
    const std::string exposition = metrics.render();

    EXPECT_TRUE(contains(exposition, R"(gateway_backend_requests_total{service="users"} 0)"))
        << exposition;
    EXPECT_TRUE(contains(exposition, R"(gateway_circuit_opens_total{service="users"} 0)"))
        << exposition;
}

TEST(MetricsRegistryTest, ObserveRequestUpdatesTheCounterAndHistogram) {
    MetricsRegistry metrics(one_service());

    metrics.observe_request("GET", 200, 0.02);
    metrics.observe_request("GET", 200, 0.03);
    metrics.observe_request("POST", 429, 0.001);

    EXPECT_EQ(metrics.requests.value({"GET", "200"}), 2U);
    EXPECT_EQ(metrics.requests.value({"POST", "429"}), 1U);
    EXPECT_EQ(metrics.request_duration.count(), 3U);
    EXPECT_NEAR(metrics.request_duration.sum(), 0.051, 1e-9);
}

TEST(MetricsRegistryTest, ExpositionHasHelpTypeAndHistogramParts) {
    MetricsRegistry metrics(one_service());
    metrics.observe_request("GET", 200, 0.02);

    const std::string exposition = metrics.render();

    EXPECT_TRUE(contains(exposition, "# HELP gateway_requests_total ")) << exposition;
    EXPECT_TRUE(contains(exposition, "# TYPE gateway_requests_total counter")) << exposition;
    EXPECT_TRUE(contains(exposition, R"(gateway_requests_total{method="GET",status="200"} 1)"))
        << exposition;

    EXPECT_TRUE(contains(exposition, "# TYPE gateway_request_duration_seconds histogram"));
    EXPECT_TRUE(contains(exposition, R"(gateway_request_duration_seconds_bucket{le="0.025"} 1)"))
        << exposition;
    EXPECT_TRUE(contains(exposition, R"(gateway_request_duration_seconds_bucket{le="+Inf"} 1)"))
        << exposition;
    EXPECT_TRUE(contains(exposition, "gateway_request_duration_seconds_count 1"));
    EXPECT_TRUE(contains(exposition, "gateway_request_duration_seconds_sum "));

    EXPECT_TRUE(contains(exposition, "# TYPE gateway_requests_in_flight gauge"));
}

TEST(MetricsRegistryTest, EveryCounterNameEndsInTotalAndGaugesDoNot) {
    const MetricsRegistry metrics(one_service());
    const std::string exposition = metrics.render();

    std::size_t position = 0;
    int checked = 0;
    while ((position = exposition.find("# TYPE ", position)) != std::string::npos) {
        const std::size_t end = exposition.find('\n', position);
        const std::string line = exposition.substr(position, end - position);
        const std::size_t name_start = std::string("# TYPE ").size();
        const std::size_t space = line.find(' ', name_start);
        const std::string name = line.substr(name_start, space - name_start);
        const std::string type = line.substr(space + 1);

        if (type == "counter") {
            EXPECT_EQ(name.substr(name.size() - 6), "_total") << name;
            ++checked;
        } else if (type == "gauge") {
            EXPECT_NE(name.substr(name.size() - 6), "_total") << name;
            ++checked;
        }
        position = end;
    }
    EXPECT_GT(checked, 5) << "the exposition should contain several counters and gauges";
}

/// Structural check of the whole exposition. The output was also validated
/// against the reference prometheus_client parser by hand; this keeps the same
/// invariants under test without adding a Python dependency to the suite.
TEST(MetricsRegistryTest, ExpositionIsStructurallyValid) {
    MetricsRegistry metrics(one_service());
    metrics.observe_request("GET", 200, 0.02);
    metrics.observe_request("POST", 503, 3.0);
    metrics.backend_duration.observe(0.4);
    metrics.rate_limited.increment();

    const std::string exposition = metrics.render();

    std::string declared_name;
    std::string declared_type;
    int families = 0;
    int samples = 0;

    std::size_t position = 0;
    while (position < exposition.size()) {
        const std::size_t end = exposition.find('\n', position);
        ASSERT_NE(end, std::string::npos) << "the exposition must end with a newline";
        const std::string line = exposition.substr(position, end - position);
        position = end + 1;

        ASSERT_FALSE(line.empty()) << "blank lines are not part of the format";

        if (line.rfind("# HELP ", 0) == 0) {
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            const std::size_t name_start = std::string("# TYPE ").size();
            const std::size_t space = line.find(' ', name_start);
            ASSERT_NE(space, std::string::npos) << line;
            declared_name = line.substr(name_start, space - name_start);
            declared_type = line.substr(space + 1);
            EXPECT_TRUE(declared_type == "counter" || declared_type == "gauge" ||
                        declared_type == "histogram")
                << "unknown type: " << declared_type;
            ++families;
            continue;
        }
        ASSERT_NE(line.find('#'), 0U) << "unexpected comment: " << line;

        // A sample is "<name>[{labels}] <value>".
        const std::size_t value_space = line.rfind(' ');
        ASSERT_NE(value_space, std::string::npos) << line;
        const std::string name_and_labels = line.substr(0, value_space);
        const std::string value = line.substr(value_space + 1);
        EXPECT_FALSE(value.empty()) << line;
        EXPECT_NO_THROW((void)std::stod(value)) << "unparseable value in: " << line;

        const std::string sample_name = name_and_labels.substr(0, name_and_labels.find('{'));
        EXPECT_EQ(sample_name.rfind(declared_name, 0), 0U)
            << "sample " << sample_name << " does not belong to family " << declared_name;
        if (name_and_labels.find('{') != std::string::npos) {
            EXPECT_EQ(name_and_labels.back(), '}') << line;
        }
        ++samples;
    }

    EXPECT_EQ(families, 14) << "every metric family must declare its type";
    EXPECT_GT(samples, families) << "families should carry samples";

    // Histogram invariants the reference parser enforces.
    for (const std::string& name :
         {std::string("gateway_request_duration_seconds"),
          std::string("gateway_backend_duration_seconds")}) {
        const long long inf = sample(exposition, name + R"(_bucket{le="+Inf"})");
        const long long count = sample(exposition, name + "_count");
        EXPECT_GE(inf, 0) << name;
        EXPECT_EQ(inf, count) << "the +Inf bucket must equal _count for " << name;
    }
}

TEST(MetricsRegistryTest, InFlightGaugeIsExposed) {
    MetricsRegistry metrics(one_service());
    metrics.requests_in_flight.increment();
    metrics.requests_in_flight.increment();

    EXPECT_EQ(sample(metrics.render(), "gateway_requests_in_flight"), 2);

    metrics.requests_in_flight.decrement();
    metrics.requests_in_flight.decrement();
    EXPECT_EQ(sample(metrics.render(), "gateway_requests_in_flight"), 0);
}

TEST(MetricsRegistryTest, RenderingConcurrentlyWithUpdatesIsSafe) {
    MetricsRegistry metrics(one_service());
    std::atomic<bool> stop{false};

    std::thread writer([&metrics, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            metrics.observe_request("GET", 200, 0.01);
            metrics.backend_requests.increment({"users"});
        }
    });

    std::size_t renders = 0;
    for (int i = 0; i < 200; ++i) {
        renders += metrics.render().empty() ? 0 : 1;
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    EXPECT_EQ(renders, 200U) << "a scrape must always produce output";
}

}  // namespace
