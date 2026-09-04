// Redis-backed rate limiting.
//
// The failure-policy tests point at a closed port and always run. The rest need
// a real Redis and skip loudly without one; start one with:
//
//   docker run --rm -p 6379:6379 redis:7-alpine
//
// Override the endpoint with GATEWAY_REDIS_TEST_HOST / GATEWAY_REDIS_TEST_PORT.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/rate_limiter.hpp"
#include "gateway/redis_rate_limiter.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::RateLimitDecision;
using gateway::RateLimiter;
using gateway::RedisFailurePolicy;
using gateway::RedisRateLimiter;

std::string redis_host() {
    const char* host = std::getenv("GATEWAY_REDIS_TEST_HOST");
    return (host != nullptr && *host != '\0') ? host : "127.0.0.1";
}

std::uint16_t redis_port() {
    const char* port = std::getenv("GATEWAY_REDIS_TEST_PORT");
    return (port != nullptr && *port != '\0')
               ? static_cast<std::uint16_t>(std::stoi(port))
               : static_cast<std::uint16_t>(6379);
}

/// A prefix unique to this run, so repeated runs never inherit old buckets.
std::string unique_prefix() {
    return "gwtest:" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

/// A port nothing is listening on: a TestBackend binds it, then releases it.
std::uint16_t closed_port() {
    const gateway_test::TestBackend probe;
    return probe.port();
}

// ------------------------------------------------------- always runnable

TEST(RedisFailurePolicyTest, FailOpenAllowsWhenRedisIsUnreachable) {
    RedisRateLimiter limiter(redis_host(), closed_port(), unique_prefix(), 1,
                             std::chrono::seconds{60}, RedisFailurePolicy::kFailOpen,
                             std::chrono::milliseconds{100});

    for (int i = 0; i < 5; ++i) {
        const RateLimitDecision decision = limiter.acquire("client", RateLimiter::Clock::now());
        EXPECT_TRUE(decision.allowed) << "fail-open favours availability";
    }
    EXPECT_EQ(limiter.failure_count(), 5U) << "the fallback must be counted, not hidden";
}

TEST(RedisFailurePolicyTest, FailClosedRejectsWhenRedisIsUnreachable) {
    RedisRateLimiter limiter(redis_host(), closed_port(), unique_prefix(), 100,
                             std::chrono::seconds{60}, RedisFailurePolicy::kFailClosed,
                             std::chrono::milliseconds{100});

    const RateLimitDecision decision = limiter.acquire("client", RateLimiter::Clock::now());

    EXPECT_FALSE(decision.allowed) << "fail-closed favours protection";
    EXPECT_GT(decision.retry_after.count(), 0) << "a rejected caller should be told to back off";
    EXPECT_EQ(limiter.failure_count(), 1U);
}

TEST(RedisFailurePolicyTest, RedisLimiterKeepsNoLocalState) {
    RedisRateLimiter limiter(redis_host(), closed_port(), unique_prefix(), 1,
                             std::chrono::seconds{60}, RedisFailurePolicy::kFailOpen,
                             std::chrono::milliseconds{100});
    (void)limiter.acquire("client", RateLimiter::Clock::now());

    // Falling back to local counting would silently break the shared limit.
    EXPECT_EQ(limiter.tracked_keys(), 0U);
}

TEST(RedisKeyTest, KeysNamespacePolicyAndClientSeparately) {
    const std::string prefix = "gw:rl";
    const RedisRateLimiter tight(redis_host(), closed_port(), prefix, 10,
                                 std::chrono::seconds{60}, RedisFailurePolicy::kFailOpen);
    const RedisRateLimiter loose(redis_host(), closed_port(), prefix, 20,
                                 std::chrono::seconds{60}, RedisFailurePolicy::kFailOpen);

    EXPECT_EQ(tight.redis_key("1.2.3.4"), "gw:rl:tb:10-60000:1.2.3.4");
    EXPECT_NE(tight.redis_key("1.2.3.4"), loose.redis_key("1.2.3.4"))
        << "different limits must not share a bucket";
    EXPECT_NE(tight.redis_key("1.2.3.4"), tight.redis_key("5.6.7.8"))
        << "different clients must not share a bucket";
}

TEST(RedisKeyTest, ScriptIsSelfContainedAndUsesRedisTime) {
    const std::string_view script = gateway::redis_token_bucket_script();

    EXPECT_NE(script.find("redis.call('TIME')"), std::string_view::npos)
        << "the shared clock must come from Redis, not the gateway host";
    EXPECT_NE(script.find("EXPIRE"), std::string_view::npos) << "keys must not live forever";
}

// -------------------------------------------------- require a live Redis

/// Skips the whole suite when no Redis answers, so the default test run needs
/// no external infrastructure.
class RedisIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        prefix_ = unique_prefix();
        RedisRateLimiter probe(redis_host(), redis_port(), prefix_, 1000,
                               std::chrono::seconds{60}, RedisFailurePolicy::kFailOpen,
                               std::chrono::milliseconds{300});
        (void)probe.acquire("probe", RateLimiter::Clock::now());
        if (probe.failure_count() != 0) {
            GTEST_SKIP() << "no Redis at " << redis_host() << ':' << redis_port()
                         << " - start one with: docker run --rm -p 6379:6379 redis:7-alpine";
        }
    }

    /// A limiter standing in for one gateway process.
    [[nodiscard]] std::unique_ptr<RedisRateLimiter> make_gateway(
        std::uint64_t requests, std::chrono::milliseconds window,
        RedisFailurePolicy policy = RedisFailurePolicy::kFailOpen) const {
        return std::make_unique<RedisRateLimiter>(redis_host(), redis_port(), prefix_, requests,
                                                  window, policy);
    }

    std::string prefix_;
};

TEST_F(RedisIntegrationTest, LimitIsEnforcedAcrossSeparateGatewayInstances) {
    auto gateway_a = make_gateway(6, std::chrono::hours{1});
    auto gateway_b = make_gateway(6, std::chrono::hours{1});

    int allowed = 0;
    for (int i = 0; i < 5; ++i) {
        allowed += gateway_a->acquire("shared-client", RateLimiter::Clock::now()).allowed ? 1 : 0;
        allowed += gateway_b->acquire("shared-client", RateLimiter::Clock::now()).allowed ? 1 : 0;
    }

    EXPECT_EQ(allowed, 6) << "the two gateways must share one budget, not get one each";
}

TEST_F(RedisIntegrationTest, DecisionsAreAtomicUnderConcurrency) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 40;
    constexpr std::uint64_t kLimit = 100;

    // Each thread gets its own limiter, as separate processes would.
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([this, &allowed] {
            auto limiter = make_gateway(kLimit, std::chrono::hours{1});
            for (int i = 0; i < kPerThread; ++i) {
                if (limiter->acquire("busy-client", RateLimiter::Clock::now()).allowed) {
                    ++allowed;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(allowed.load(), static_cast<int>(kLimit))
        << "a non-atomic check-then-increment would overshoot";
}

TEST_F(RedisIntegrationTest, RemainingAndRetryAfterAreReported) {
    auto limiter = make_gateway(3, std::chrono::hours{1});

    const RateLimitDecision first = limiter->acquire("reporting", RateLimiter::Clock::now());
    EXPECT_TRUE(first.allowed);
    EXPECT_EQ(first.limit, 3U);
    EXPECT_EQ(first.remaining, 2U);

    for (int i = 0; i < 2; ++i) {
        (void)limiter->acquire("reporting", RateLimiter::Clock::now());
    }

    const RateLimitDecision rejected = limiter->acquire("reporting", RateLimiter::Clock::now());
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(rejected.remaining, 0U);
    EXPECT_GT(rejected.retry_after.count(), 0);
}

TEST_F(RedisIntegrationTest, AllowanceRefillsWithRedisTime) {
    // Two per second, so a short wait buys tokens back.
    auto limiter = make_gateway(2, std::chrono::seconds{1});

    ASSERT_TRUE(limiter->acquire("refill", RateLimiter::Clock::now()).allowed);
    ASSERT_TRUE(limiter->acquire("refill", RateLimiter::Clock::now()).allowed);
    ASSERT_FALSE(limiter->acquire("refill", RateLimiter::Clock::now()).allowed);

    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    EXPECT_TRUE(limiter->acquire("refill", RateLimiter::Clock::now()).allowed)
        << "the bucket should have refilled";
}

TEST_F(RedisIntegrationTest, ClientsAndPoliciesDoNotShareBuckets) {
    auto tight = make_gateway(1, std::chrono::hours{1});
    auto loose = make_gateway(5, std::chrono::hours{1});

    ASSERT_TRUE(tight->acquire("alice", RateLimiter::Clock::now()).allowed);
    EXPECT_FALSE(tight->acquire("alice", RateLimiter::Clock::now()).allowed);

    EXPECT_TRUE(tight->acquire("bob", RateLimiter::Clock::now()).allowed)
        << "a different client has its own bucket";
    EXPECT_TRUE(loose->acquire("alice", RateLimiter::Clock::now()).allowed)
        << "a different policy has its own bucket";
}

TEST_F(RedisIntegrationTest, NoFailuresAreRecordedWhenRedisIsHealthy) {
    auto limiter = make_gateway(10, std::chrono::hours{1});
    for (int i = 0; i < 10; ++i) {
        (void)limiter->acquire("healthy", RateLimiter::Clock::now());
    }

    EXPECT_EQ(limiter->failure_count(), 0U) << "no fallback should have been needed";
}

}  // namespace
