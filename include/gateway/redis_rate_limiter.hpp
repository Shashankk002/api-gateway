#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/rate_limiter.hpp"

struct redisContext;

namespace gateway {

/// Rate limiting whose state lives in Redis, so several gateway processes share
/// one budget.
///
/// Every decision is a single EVALSHA of a token-bucket script: the read, the
/// refill, the allow/deny and the write happen inside one Redis-side atomic
/// operation. A local pre-check followed by a Redis counter would race, so
/// there is none. The script also takes its clock from Redis TIME, so gateway
/// processes do not need synchronised wall clocks.
///
/// Redis calls happen on the calling thread with no lock held; the only lock
/// guards checking a connection in and out of the pool.
class RedisRateLimiter final : public RateLimiter {
public:
    /// `key_prefix` namespaces this gateway's keys. `requests` per `window` is
    /// read as a bucket of that capacity refilled over that window.
    RedisRateLimiter(std::string host, std::uint16_t port, std::string key_prefix,
                     std::uint64_t requests, std::chrono::milliseconds window,
                     RedisFailurePolicy failure_policy,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds{200});
    ~RedisRateLimiter() override;

    /// `now` is ignored: the script reads Redis TIME so all gateways agree.
    [[nodiscard]] RateLimitDecision acquire(std::string_view key, Clock::time_point now) override;

    /// Full Redis key for a client key, exposed so key layout can be asserted.
    [[nodiscard]] std::string redis_key(std::string_view client_key) const;

    /// Decisions that fell back to the failure policy because Redis could not
    /// be reached or answered unusably.
    [[nodiscard]] std::uint64_t failure_count() const;

private:
    static constexpr std::size_t kMaxPooledConnections = 16;

    class Connection;

    /// A pooled connection, returned on destruction. Null when Redis is down.
    class Lease {
    public:
        Lease(RedisRateLimiter& owner, std::unique_ptr<Connection> connection);
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] Connection* get() const noexcept { return connection_.get(); }
        /// Drops the connection instead of pooling it, after a protocol error.
        void discard() noexcept { discard_ = true; }

    private:
        RedisRateLimiter& owner_;
        std::unique_ptr<Connection> connection_;
        bool discard_{false};
    };

    [[nodiscard]] Lease checkout();
    void checkin(std::unique_ptr<Connection> connection);
    [[nodiscard]] RateLimitDecision on_failure();

    std::string host_;
    std::uint16_t port_;
    std::string key_prefix_;
    std::uint64_t requests_;
    std::chrono::milliseconds window_;
    RedisFailurePolicy failure_policy_;
    std::chrono::milliseconds timeout_;
    std::string script_sha_;

    mutable std::mutex mutex_;  ///< Guards the pool and the counter only.
    std::vector<std::unique_ptr<Connection>> pool_;
    std::uint64_t failures_{0};
};

/// The Lua the limiter evaluates. Exposed for documentation and tests.
[[nodiscard]] std::string_view redis_token_bucket_script();

}  // namespace gateway
