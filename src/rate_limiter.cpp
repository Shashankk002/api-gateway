#include "gateway/rate_limiter.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace gateway {
namespace {

double seconds_between(RateLimiter::Clock::time_point from, RateLimiter::Clock::time_point to) {
    if (to <= from) {
        return 0.0;  // A clock that did not advance must not mint tokens.
    }
    return std::chrono::duration<double>(to - from).count();
}

std::chrono::milliseconds ceil_to_ms(double seconds) {
    if (seconds <= 0.0) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
        std::ceil(seconds * 1000.0))};
}

}  // namespace

TokenBucketLimiter::TokenBucketLimiter(std::uint64_t capacity, double refill_per_second)
    : capacity_(capacity == 0 ? 1 : capacity),
      refill_per_second_(refill_per_second > 0.0 ? refill_per_second : 1.0),
      refill_span_(std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(static_cast<double>(capacity_) / refill_per_second_))) {}

RateLimitDecision TokenBucketLimiter::acquire(std::string_view key, Clock::time_point now) {
    auto& shard = buckets_.shard_for(key);
    const std::lock_guard<std::mutex> guard(shard.mutex);
    ++shard.operations;

    auto [entry, inserted] = shard.entries.try_emplace(std::string(key));
    Bucket& bucket = entry->second;
    if (inserted) {
        bucket.tokens = static_cast<double>(capacity_);
        bucket.updated = now;
    } else {
        bucket.tokens = std::min(static_cast<double>(capacity_),
                                 bucket.tokens + seconds_between(bucket.updated, now) *
                                                     refill_per_second_);
        bucket.updated = std::max(bucket.updated, now);
    }

    RateLimitDecision decision;
    decision.limit = capacity_;
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        decision.allowed = true;
    } else {
        decision.allowed = false;
        decision.retry_after = ceil_to_ms((1.0 - bucket.tokens) / refill_per_second_);
    }
    decision.remaining = static_cast<std::uint64_t>(bucket.tokens);

    if (detail::ShardedKeyMap<Bucket>::due_for_sweep(shard)) {
        sweep_locked(shard, now);
    }
    return decision;
}

void TokenBucketLimiter::sweep_locked(detail::ShardedKeyMap<Bucket>::Shard& shard,
                                      Clock::time_point now) const {
    // A bucket that has had time to refill completely is indistinguishable from
    // a fresh one, so dropping it loses nothing and bounds memory.
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        it = (now - it->second.updated >= refill_span_) ? shard.entries.erase(it) : std::next(it);
    }
}

SlidingWindowLimiter::SlidingWindowLimiter(std::uint64_t max_requests,
                                           std::chrono::milliseconds window)
    : max_requests_(max_requests == 0 ? 1 : max_requests),
      window_(window.count() > 0 ? window : std::chrono::milliseconds{1}) {}

RateLimitDecision SlidingWindowLimiter::acquire(std::string_view key, Clock::time_point now) {
    auto& shard = windows_.shard_for(key);
    const std::lock_guard<std::mutex> guard(shard.mutex);
    ++shard.operations;

    Hits& hits = shard.entries[std::string(key)];

    // Anything at or before this instant has left the trailing window.
    const Clock::time_point cutoff = now - window_;
    while (!hits.empty() && hits.front() <= cutoff) {
        hits.pop_front();
    }

    RateLimitDecision decision;
    decision.limit = max_requests_;
    if (hits.size() < max_requests_) {
        hits.push_back(now);
        decision.allowed = true;
        decision.remaining = max_requests_ - hits.size();
    } else {
        decision.allowed = false;
        decision.remaining = 0;
        // The oldest hit leaving the window is the first moment a slot frees up.
        decision.retry_after =
            std::chrono::duration_cast<std::chrono::milliseconds>(hits.front() + window_ - now) +
            std::chrono::milliseconds{1};
    }

    if (detail::ShardedKeyMap<Hits>::due_for_sweep(shard)) {
        sweep_locked(shard, now);
    }
    return decision;
}

void SlidingWindowLimiter::sweep_locked(detail::ShardedKeyMap<Hits>::Shard& shard,
                                        Clock::time_point now) const {
    const Clock::time_point cutoff = now - window_;
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        Hits& hits = it->second;
        while (!hits.empty() && hits.front() <= cutoff) {
            hits.pop_front();
        }
        it = hits.empty() ? shard.entries.erase(it) : std::next(it);
    }
}

}  // namespace gateway
