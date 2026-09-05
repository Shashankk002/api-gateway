#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/health.hpp"

namespace gateway {

/// Round-robin selection over the instances of a service that are currently
/// eligible, each service advancing its own position. Safe to call
/// concurrently. Does no I/O and never probes or retries; it only reads state
/// the HealthChecker and the circuit breakers maintain.
///
/// An instance is eligible when it is healthy and its circuit is not refusing
/// traffic. The two are independent: health reflects probing, a circuit
/// reflects how real requests have been failing.
class LoadBalancer {
public:
    /// The index identifies the instance for this object's lifetime, which is
    /// what ties circuit state to the right endpoint.
    struct Selection {
        const BackendEndpoint* endpoint{nullptr};
        std::size_t index{0};

        [[nodiscard]] explicit operator bool() const noexcept { return endpoint != nullptr; }
    };

    /// `health` and `breakers` must outlive this object.
    LoadBalancer(BackendTable backends, const BackendHealth& health,
                 const CircuitBreakers& breakers);

    /// Next eligible instance, skipping any index in `exclude`. Falsy when none
    /// is eligible. The pointee stays valid for this object's lifetime.
    [[nodiscard]] Selection select(std::string_view service,
                                   std::span<const std::size_t> exclude = {}) const;

    [[nodiscard]] const BackendTable& backends() const noexcept { return backends_; }

private:
    BackendTable backends_;
    const BackendHealth& health_;
    const CircuitBreakers& breakers_;

    // Populated in the constructor and never resized, so selection only touches
    // the atomics. Mutable because advancing a counter is not a configuration
    // change.
    mutable std::map<std::string, std::atomic<std::size_t>, std::less<>> positions_;
};

}  // namespace gateway
