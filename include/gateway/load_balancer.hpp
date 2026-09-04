#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "gateway/config.hpp"

namespace gateway {

/// Picks which instance of a service should serve the next request.
///
/// Selection is round robin, each service advancing its own position, and is
/// safe to call concurrently. This type answers only "which instance"; it does
/// no I/O, knows nothing about routes or HTTP, and never checks backend health.
class LoadBalancer {
public:
    explicit LoadBalancer(BackendTable backends);

    /// The next instance for `service`, or nullptr when the service is unknown
    /// or has no instances. The pointee stays valid for this object's lifetime.
    [[nodiscard]] const BackendEndpoint* select(std::string_view service) const;

    [[nodiscard]] const BackendTable& backends() const noexcept { return backends_; }

private:
    BackendTable backends_;

    // One counter per service, so services never share a position. Populated in
    // the constructor and only ever read afterwards, so the map itself is not
    // mutated during selection; mutable because advancing a counter does not
    // change the configuration.
    mutable std::map<std::string, std::atomic<std::size_t>, std::less<>> positions_;
};

}  // namespace gateway
