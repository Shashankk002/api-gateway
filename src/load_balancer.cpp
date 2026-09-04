#include "gateway/load_balancer.hpp"

#include <utility>

namespace gateway {

LoadBalancer::LoadBalancer(BackendTable backends) : backends_(std::move(backends)) {
    for (const auto& [service, instances] : backends_) {
        positions_.try_emplace(service);
    }
}

const BackendEndpoint* LoadBalancer::select(std::string_view service) const {
    const auto entry = backends_.find(service);
    if (entry == backends_.end() || entry->second.empty()) {
        return nullptr;
    }

    // relaxed: the counter only has to be unique per call, not ordered against
    // other memory. Letting it wrap is harmless.
    const auto position = positions_.find(service);
    const std::size_t ticket = position->second.fetch_add(1, std::memory_order_relaxed);
    return &entry->second[ticket % entry->second.size()];
}

}  // namespace gateway
