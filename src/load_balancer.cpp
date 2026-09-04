#include "gateway/load_balancer.hpp"

#include <atomic>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace gateway {

LoadBalancer::LoadBalancer(BackendTable backends, const BackendHealth& health)
    : backends_(std::move(backends)), health_(health) {
    for (const auto& [service, instances] : backends_) {
        positions_.try_emplace(service);
    }
}

const BackendEndpoint* LoadBalancer::select(std::string_view service) const {
    const auto entry = backends_.find(service);
    if (entry == backends_.end() || entry->second.empty()) {
        return nullptr;
    }

    const std::vector<BackendEndpoint>& instances = entry->second;
    const std::span<const std::atomic<bool>> flags = health_.flags(service);
    const auto healthy = [&flags](std::size_t index) {
        // A service missing from the health map is treated as unrestricted
        // rather than dead; the two are built from the same table.
        return index >= flags.size() || flags[index].load(std::memory_order_relaxed);
    };

    std::size_t healthy_count = 0;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        healthy_count += healthy(index) ? 1 : 0;
    }
    if (healthy_count == 0) {
        return nullptr;
    }

    // relaxed: the counter only has to be unique per call, not ordered against
    // other memory. Letting it wrap is harmless.
    const auto position = positions_.find(service);
    const std::size_t ticket = position->second.fetch_add(1, std::memory_order_relaxed);

    // Take the ticket-th healthy instance, so the rotation stays even across the
    // healthy subset instead of doubling up on whoever follows an unhealthy one.
    std::size_t remaining = ticket % healthy_count;
    const BackendEndpoint* last_healthy = nullptr;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        if (!healthy(index)) {
            continue;
        }
        last_healthy = &instances[index];
        if (remaining == 0) {
            return last_healthy;
        }
        --remaining;
    }

    // Health flipped between the two passes; any instance seen healthy will do.
    return last_healthy;
}

}  // namespace gateway
