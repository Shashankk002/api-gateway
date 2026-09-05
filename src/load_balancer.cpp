#include "gateway/load_balancer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace gateway {

LoadBalancer::LoadBalancer(BackendTable backends, const BackendHealth& health,
                           const CircuitBreakers& breakers)
    : backends_(std::move(backends)), health_(health), breakers_(breakers) {
    for (const auto& [service, instances] : backends_) {
        positions_.try_emplace(service);
    }
}

LoadBalancer::Selection LoadBalancer::select(std::string_view service,
                                             std::span<const std::size_t> exclude) const {
    const auto entry = backends_.find(service);
    if (entry == backends_.end() || entry->second.empty()) {
        return {};
    }

    const std::vector<BackendEndpoint>& instances = entry->second;
    const std::span<const std::atomic<bool>> flags = health_.flags(service);
    const auto eligible = [&](std::size_t index) {
        // A service missing from the health map counts as unrestricted rather
        // than dead; both are built from the same table.
        const bool healthy = index >= flags.size() || flags[index].load(std::memory_order_relaxed);
        return healthy && !breakers_.blocks_selection(service, index) &&
               std::find(exclude.begin(), exclude.end(), index) == exclude.end();
    };

    std::size_t eligible_count = 0;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        eligible_count += eligible(index) ? 1 : 0;
    }
    if (eligible_count == 0) {
        return {};
    }

    // relaxed: the ticket only has to be unique per call, not ordered against
    // other memory. Wrapping is harmless.
    const auto position = positions_.find(service);
    const std::size_t ticket = position->second.fetch_add(1, std::memory_order_relaxed);

    // The ticket-th eligible instance, so the rotation stays even across the
    // eligible subset instead of doubling up on whoever follows a skipped one.
    std::size_t remaining = ticket % eligible_count;
    Selection last_eligible;
    for (std::size_t index = 0; index < instances.size(); ++index) {
        if (!eligible(index)) {
            continue;
        }
        last_eligible = Selection{&instances[index], index};
        if (remaining == 0) {
            return last_eligible;
        }
        --remaining;
    }

    // Eligibility changed between the two passes; anything seen eligible will do.
    return last_eligible;
}

}  // namespace gateway
