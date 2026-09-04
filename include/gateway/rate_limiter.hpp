#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gateway {

/// Outcome of one rate-limit check. Carries only what a caller needs to answer
/// the client; nothing here is HTTP-specific.
struct RateLimitDecision {
    bool allowed{true};
    std::uint64_t limit{0};      ///< Requests permitted per window.
    std::uint64_t remaining{0};  ///< Allowance left after this decision.

    /// How long before the caller could succeed. Zero when allowed, and zero
    /// when the algorithm cannot say honestly.
    std::chrono::milliseconds retry_after{0};
};

/// Decides whether one request against a logical key may proceed.
///
/// Implementations see a key and a time, never an HTTP request. The clock is a
/// parameter so tests can advance it without sleeping; production passes
/// Clock::now().
class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    RateLimiter() = default;
    virtual ~RateLimiter() = default;
    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;

    /// Consumes one unit of allowance for `key` and reports the decision.
    [[nodiscard]] virtual RateLimitDecision acquire(std::string_view key,
                                                    Clock::time_point now) = 0;

    /// Keys currently held in memory. Always 0 for limiters that keep no local
    /// state; exposed so cleanup can be observed.
    [[nodiscard]] virtual std::size_t tracked_keys() const { return 0; }
};

namespace detail {

/// Per-key state spread over a fixed set of shards, so unrelated clients do not
/// queue behind one mutex. The shard count is fixed at construction, so a shard
/// reference stays valid for the object's lifetime.
template <typename Entry>
class ShardedKeyMap {
public:
    struct Shard {
        mutable std::mutex mutex;
        std::unordered_map<std::string, Entry> entries;
        std::uint64_t operations{0};
    };

    [[nodiscard]] Shard& shard_for(std::string_view key) {
        return shards_[std::hash<std::string_view>{}(key) % kShardCount];
    }

    [[nodiscard]] std::size_t size() const {
        std::size_t total = 0;
        for (const Shard& shard : shards_) {
            const std::lock_guard<std::mutex> guard(shard.mutex);
            total += shard.entries.size();
        }
        return total;
    }

    /// True once a shard has done enough work to be worth sweeping. Cleanup is
    /// amortised onto request handling rather than given its own thread.
    [[nodiscard]] static bool due_for_sweep(const Shard& shard) {
        return shard.operations % kSweepInterval == 0;
    }

private:
    static constexpr std::size_t kShardCount = 16;
    static constexpr std::uint64_t kSweepInterval = 512;

    std::array<Shard, kShardCount> shards_;
};

}  // namespace detail

/// Token bucket: a burst of `capacity` requests, then a steady
/// `refill_per_second`. Buckets start full and refill from elapsed time when a
/// request arrives, so no timer thread is needed and no request ever sleeps.
class TokenBucketLimiter final : public RateLimiter {
public:
    TokenBucketLimiter(std::uint64_t capacity, double refill_per_second);

    [[nodiscard]] RateLimitDecision acquire(std::string_view key, Clock::time_point now) override;
    [[nodiscard]] std::size_t tracked_keys() const override { return buckets_.size(); }

private:
    struct Bucket {
        double tokens{0.0};
        Clock::time_point updated{};
    };

    void sweep_locked(detail::ShardedKeyMap<Bucket>::Shard& shard, Clock::time_point now) const;

    std::uint64_t capacity_;
    double refill_per_second_;
    /// How long an untouched bucket takes to return to full; after that its
    /// entry carries no information and can be dropped.
    Clock::duration refill_span_;

    detail::ShardedKeyMap<Bucket> buckets_;
};

/// Sliding window: at most `max_requests` in any trailing `window`. Timestamps
/// of accepted requests are kept per key and expired as the window moves, so
/// the limit does not reset in a burst at a fixed boundary.
class SlidingWindowLimiter final : public RateLimiter {
public:
    SlidingWindowLimiter(std::uint64_t max_requests, std::chrono::milliseconds window);

    [[nodiscard]] RateLimitDecision acquire(std::string_view key, Clock::time_point now) override;
    [[nodiscard]] std::size_t tracked_keys() const override { return windows_.size(); }

private:
    /// Bounded by max_requests: a rejected request is never recorded.
    using Hits = std::deque<Clock::time_point>;

    void sweep_locked(detail::ShardedKeyMap<Hits>::Shard& shard, Clock::time_point now) const;

    std::uint64_t max_requests_;
    std::chrono::milliseconds window_;

    detail::ShardedKeyMap<Hits> windows_;
};

}  // namespace gateway
