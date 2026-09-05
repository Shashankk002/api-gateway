#include "gateway/health.hpp"

#include <httplib.h>

#include "gateway/metrics.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace gateway {
namespace {

// The backend's own health endpoint, unrelated to the gateway's /health.
constexpr const char* kBackendHealthPath = "/health";

bool is_success_status(int status) { return status >= 200 && status < 300; }

}  // namespace

BackendHealth::BackendHealth(const BackendTable& backends) {
    for (const auto& [service, instances] : backends) {
        std::vector<std::atomic<bool>> flags(instances.size());
        for (std::atomic<bool>& flag : flags) {
            flag.store(true, std::memory_order_relaxed);
        }
        states_.emplace(service, std::move(flags));
    }
}

bool BackendHealth::is_healthy(std::string_view service, std::size_t index) const {
    const auto entry = states_.find(service);
    if (entry == states_.end() || index >= entry->second.size()) {
        return false;
    }
    return entry->second[index].load(std::memory_order_relaxed);
}

bool BackendHealth::set_healthy(std::string_view service, std::size_t index, bool healthy) {
    const auto entry = states_.find(service);
    if (entry == states_.end() || index >= entry->second.size()) {
        return false;
    }
    if (entry->second[index].exchange(healthy, std::memory_order_relaxed) == healthy) {
        return false;
    }

    // Taken and released so a waiter cannot evaluate its predicate and begin
    // waiting between the exchange above and the notify below.
    { const std::lock_guard<std::mutex> guard(mutex_); }
    changed_.notify_all();
    return true;
}

std::span<const std::atomic<bool>> BackendHealth::flags(std::string_view service) const {
    const auto entry = states_.find(service);
    if (entry == states_.end()) {
        return {};
    }
    return std::span<const std::atomic<bool>>(entry->second.data(), entry->second.size());
}

bool BackendHealth::wait_for(const std::function<bool()>& predicate,
                             std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&predicate] { return predicate(); });
}

HealthChecker::HealthChecker(BackendTable backends, BackendHealth& health,
                             std::chrono::milliseconds interval, std::chrono::milliseconds timeout,
                             MetricsRegistry* metrics)
    : backends_(std::move(backends)),
      health_(health),
      metrics_(metrics),
      interval_(interval),
      timeout_(timeout) {}

HealthChecker::~HealthChecker() { stop(); }

void HealthChecker::start() {
    if (interval_.count() <= 0 || thread_.joinable()) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void HealthChecker::stop() {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HealthChecker::run() {
    while (true) {
        sweep();

        // Interruptible wait, so stop() need not outlast the interval.
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, interval_, [this] { return stopping_; })) {
            return;
        }
    }
}

void HealthChecker::sweep() {
    // Probed in parallel so one slow instance cannot hold up the sweep. Threads
    // are transient and bounded by the instance count. Each probe records its
    // own result immediately, so a slow instance delays nobody else's update.
    struct Transition {
        const std::string* service;
        const BackendEndpoint* instance;
        bool healthy;
    };
    std::mutex transitions_mutex;
    std::vector<Transition> transitions;

    std::vector<std::thread> probes;
    for (const auto& [service, instances] : backends_) {
        for (std::size_t index = 0; index < instances.size(); ++index) {
            probes.emplace_back([this, &service, &instances, index, &transitions_mutex,
                                 &transitions] {
                const BackendEndpoint& instance = instances[index];
                const bool healthy = probe(instance);
                if (health_.set_healthy(service, index, healthy)) {
                    const std::lock_guard<std::mutex> guard(transitions_mutex);
                    transitions.push_back(Transition{&service, &instance, healthy});
                }
            });
        }
    }
    for (std::thread& probe_thread : probes) {
        probe_thread.join();
    }

    // Logged here rather than in the probes, so only this thread writes to cerr.
    for (const Transition& transition : transitions) {
        if (metrics_ != nullptr) {
            const std::vector<std::string_view> labels{*transition.service};
            if (transition.healthy) {
                metrics_->health_recoveries.increment(labels);
            } else {
                metrics_->health_failures.increment(labels);
            }
        }
        std::cerr << "gateway: backend " + *transition.service + " " +
                         transition.instance->host + ":" +
                         std::to_string(transition.instance->port) +
                         (transition.healthy ? " is healthy\n" : " is unhealthy\n");
    }
}

bool HealthChecker::probe(const BackendEndpoint& backend) const {
    httplib::Client client(backend.host, backend.port);
    client.set_connection_timeout(timeout_);
    client.set_read_timeout(timeout_);
    client.set_write_timeout(timeout_);
    client.set_keep_alive(false);

    const httplib::Result result = client.Get(kBackendHealthPath);
    return static_cast<bool>(result) && is_success_status(result->status);
}

}  // namespace gateway
