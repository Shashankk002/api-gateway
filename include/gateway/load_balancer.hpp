#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <span>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway/health.hpp"

namespace gateway {

/// Picks which instance of a service should serve the next request.
///
/// Selection is round robin over the instances that are currently eligible,
/// each service advancing its own position, and is safe to call concurrently.
/// This type answers only "which instance": it does no I/O, knows nothing about
/// routes or HTTP, and never performs a health check or a request itself. It
/// reads state the HealthChecker and the circuit breakers maintain.
///
/// An instance is eligible when it is marked healthy and its circuit is not
/// refusing traffic. The two are independent: health reflects what probing
/// observed, a circuit reflects how real requests have been failing.
class LoadBalancer {
public:
    /// A chosen instance and its position in the service's configured list. The
    /// index identifies the instance for as long as this object lives, which is
    /// what ties circuit state to the right endpoint.
    struct Selection {
        const BackendEndpoint* endpoint{nullptr};
        std::size_t index{0};

        [[nodiscard]] explicit operator bool() const noexcept { return endpoint != nullptr; }
    };

    /// `health` and `breakers` must outlive this object and describe the same
    /// services.
    LoadBalancer(BackendTable backends, const BackendHealth& health,
                 const CircuitBreakers& breakers);

    /// The next eligible instance for `service`, skipping any index listed in
    /// `exclude`. Falsy when the service is unknown, has no instances, or has
    /// none currently eligible. The pointee stays valid for this object's
    /// lifetime.
    [[nodiscard]] Selection select(std::string_view service,
                                   std::span<const std::size_t> exclude = {}) const;

    [[nodiscard]] const BackendTable& backends() const noexcept { return backends_; }

private:
    BackendTable backends_;
    const BackendHealth& health_;
    const CircuitBreakers& breakers_;

    // One counter per service, so services never share a position. Populated in
    // the constructor and only ever read afterwards, so the map itself is not
    // mutated during selection; mutable because advancing a counter does not
    // change the configuration.
    mutable std::map<std::string, std::atomic<std::size_t>, std::less<>> positions_;
};

}  // namespace gateway
