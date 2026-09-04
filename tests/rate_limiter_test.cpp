// Unit tests for the local rate-limit algorithms. The clock is a parameter, so
// refill and expiry are exercised by advancing time rather than sleeping.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/rate_limiter.hpp"

namespace {

using gateway::RateLimitDecision;
using gateway::RateLimiter;
using gateway::SlidingWindowLimiter;
using gateway::TokenBucketLimiter;

using Clock = RateLimiter::Clock;

constexpr Clock::time_point kStart{};

Clock::time_point after(std::chrono::milliseconds offset) { return kStart + offset; }

/// Accepted decisions out of `count` consecutive attempts at one instant.
std::size_t allowed_in_a_row(RateLimiter& limiter, std::string_view key, std::size_t count,
                             Clock::time_point now) {
    std::size_t allowed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        allowed += limiter.acquire(key, now).allowed ? 1 : 0;
    }
    return allowed;
}

// ---------------------------------------------------------------- token bucket

TEST(TokenBucketTest, StartsFullAndReportsTheConfiguredLimit) {
    TokenBucketLimiter limiter(5, 1.0);

    const RateLimitDecision first = limiter.acquire("a", kStart);

    EXPECT_TRUE(first.allowed);
    EXPECT_EQ(first.limit, 5U);
    EXPECT_EQ(first.remaining, 4U) << "a full bucket minus the token just taken";
}

TEST(TokenBucketTest, ConsumesOneTokenPerRequestUntilEmpty) {
    TokenBucketLimiter limiter(3, 1.0);

    EXPECT_EQ(allowed_in_a_row(limiter, "a", 3, kStart), 3U);

    const RateLimitDecision rejected = limiter.acquire("a", kStart);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(rejected.remaining, 0U);
    EXPECT_GT(rejected.retry_after.count(), 0) << "an empty bucket must say when to come back";
}

TEST(TokenBucketTest, RefillsWithElapsedTime) {
    TokenBucketLimiter limiter(4, 2.0);  // Two tokens per second.
    ASSERT_EQ(allowed_in_a_row(limiter, "a", 4, kStart), 4U);
    ASSERT_FALSE(limiter.acquire("a", kStart).allowed);

    // Half a second buys exactly one token.
    EXPECT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{500})).allowed);
    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{500})).allowed);

    // A further second buys two more.
    EXPECT_EQ(allowed_in_a_row(limiter, "a", 3, after(std::chrono::milliseconds{1500})), 2U);
}

TEST(TokenBucketTest, RefillIsCappedAtCapacity) {
    TokenBucketLimiter limiter(3, 10.0);
    ASSERT_EQ(allowed_in_a_row(limiter, "a", 3, kStart), 3U);

    // An hour of refill at ten per second must still leave only three tokens.
    EXPECT_EQ(allowed_in_a_row(limiter, "a", 10, after(std::chrono::hours{1})), 3U);
}

TEST(TokenBucketTest, FractionalRefillAccumulatesRatherThanRounding) {
    TokenBucketLimiter limiter(1, 1.0);
    ASSERT_TRUE(limiter.acquire("a", kStart).allowed);

    // Four 250ms steps each add a quarter token; only the last one crosses 1.
    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{250})).allowed);
    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{500})).allowed);
    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{750})).allowed);
    EXPECT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{1000})).allowed);
}

TEST(TokenBucketTest, RetryAfterShrinksAsTheBucketRefills) {
    TokenBucketLimiter limiter(1, 1.0);
    ASSERT_TRUE(limiter.acquire("a", kStart).allowed);

    const auto immediately = limiter.acquire("a", kStart).retry_after;
    const auto later = limiter.acquire("a", after(std::chrono::milliseconds{600})).retry_after;

    EXPECT_GT(immediately.count(), 0);
    EXPECT_LT(later.count(), immediately.count());
}

TEST(TokenBucketTest, KeysAreIndependent) {
    TokenBucketLimiter limiter(2, 1.0);

    ASSERT_EQ(allowed_in_a_row(limiter, "alice", 2, kStart), 2U);

    EXPECT_FALSE(limiter.acquire("alice", kStart).allowed);
    EXPECT_TRUE(limiter.acquire("bob", kStart).allowed) << "bob has his own bucket";
}

TEST(TokenBucketTest, ClockGoingBackwardsDoesNotMintTokens) {
    TokenBucketLimiter limiter(2, 1.0);
    ASSERT_EQ(allowed_in_a_row(limiter, "a", 2, after(std::chrono::seconds{10})), 2U);

    EXPECT_FALSE(limiter.acquire("a", kStart).allowed) << "an earlier timestamp must not refill";
}

TEST(TokenBucketTest, IdleKeysAreEventuallyDropped) {
    TokenBucketLimiter limiter(2, 100.0);  // Refills fully in 20ms.

    for (int i = 0; i < 600; ++i) {
        (void)limiter.acquire("client-" + std::to_string(i), kStart);
    }
    ASSERT_GT(limiter.tracked_keys(), 0U);

    // Sweeping is amortised onto later requests, so drive some through.
    for (int i = 0; i < 2000; ++i) {
        (void)limiter.acquire("sweeper", after(std::chrono::seconds{60}));
    }

    EXPECT_LT(limiter.tracked_keys(), 600U) << "refilled buckets should not be kept forever";
}

TEST(TokenBucketTest, ConcurrentAcquisitionsNeverExceedCapacity) {
    constexpr std::size_t kCapacity = 200;
    // No refill worth speaking of during the test, so capacity is a hard ceiling.
    TokenBucketLimiter limiter(kCapacity, 0.000001);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::atomic<std::size_t> allowed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, &allowed] {
            for (int i = 0; i < kPerThread; ++i) {
                if (limiter.acquire("shared", Clock::now()).allowed) {
                    ++allowed;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(allowed.load(), kCapacity) << "exactly the capacity may pass, no more and no fewer";
}

// -------------------------------------------------------------- sliding window

TEST(SlidingWindowTest, AllowsRequestsBelowTheLimit) {
    SlidingWindowLimiter limiter(3, std::chrono::seconds{1});

    const RateLimitDecision first = limiter.acquire("a", kStart);
    EXPECT_TRUE(first.allowed);
    EXPECT_EQ(first.limit, 3U);
    EXPECT_EQ(first.remaining, 2U);
}

TEST(SlidingWindowTest, RejectsAtTheLimit) {
    SlidingWindowLimiter limiter(3, std::chrono::seconds{1});
    ASSERT_EQ(allowed_in_a_row(limiter, "a", 3, kStart), 3U);

    const RateLimitDecision rejected = limiter.acquire("a", kStart);
    EXPECT_FALSE(rejected.allowed);
    EXPECT_EQ(rejected.remaining, 0U);
    EXPECT_GT(rejected.retry_after.count(), 0);
}

TEST(SlidingWindowTest, OldRequestsExpireOutOfTheWindow) {
    SlidingWindowLimiter limiter(2, std::chrono::seconds{1});
    ASSERT_EQ(allowed_in_a_row(limiter, "a", 2, kStart), 2U);
    ASSERT_FALSE(limiter.acquire("a", kStart).allowed);

    // Once the original pair has aged out, the full allowance is back.
    EXPECT_EQ(allowed_in_a_row(limiter, "a", 2, after(std::chrono::milliseconds{1001})), 2U);
}

TEST(SlidingWindowTest, WindowSlidesRatherThanResettingAtABoundary) {
    SlidingWindowLimiter limiter(2, std::chrono::seconds{1});
    ASSERT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{0})).allowed);
    ASSERT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{900})).allowed);

    // At 1001ms only the first hit has aged out, so exactly one slot is free.
    // A fixed window would have reset and allowed both.
    EXPECT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{1001})).allowed);
    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{1001})).allowed);
}

TEST(SlidingWindowTest, ARequestExactlyOneWindowOldHasExpired) {
    SlidingWindowLimiter limiter(1, std::chrono::seconds{1});
    ASSERT_TRUE(limiter.acquire("a", kStart).allowed);

    EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{999})).allowed);
    EXPECT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{1000})).allowed)
        << "the trailing window is half-open, so a hit at exactly -window is gone";
}

TEST(SlidingWindowTest, RetryAfterPointsPastTheOldestHit) {
    SlidingWindowLimiter limiter(1, std::chrono::seconds{1});
    ASSERT_TRUE(limiter.acquire("a", kStart).allowed);

    const auto retry = limiter.acquire("a", after(std::chrono::milliseconds{400})).retry_after;

    // The hit at 0 leaves the window at 1000ms, which is 600ms away.
    EXPECT_GE(retry.count(), 600);
    EXPECT_LE(retry.count(), 700);
}

TEST(SlidingWindowTest, RejectedRequestsDoNotCountAgainstTheWindow) {
    SlidingWindowLimiter limiter(1, std::chrono::seconds{1});
    ASSERT_TRUE(limiter.acquire("a", kStart).allowed);

    // Hammering while rejected must not push the recovery time out.
    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(limiter.acquire("a", after(std::chrono::milliseconds{500})).allowed);
    }
    EXPECT_TRUE(limiter.acquire("a", after(std::chrono::milliseconds{1000})).allowed);
}

TEST(SlidingWindowTest, KeysAreIndependent) {
    SlidingWindowLimiter limiter(1, std::chrono::seconds{1});

    EXPECT_TRUE(limiter.acquire("alice", kStart).allowed);
    EXPECT_FALSE(limiter.acquire("alice", kStart).allowed);
    EXPECT_TRUE(limiter.acquire("bob", kStart).allowed);
}

TEST(SlidingWindowTest, StaleKeysAreCleanedUp) {
    SlidingWindowLimiter limiter(1, std::chrono::milliseconds{10});

    for (int i = 0; i < 600; ++i) {
        (void)limiter.acquire("client-" + std::to_string(i), kStart);
    }
    ASSERT_GT(limiter.tracked_keys(), 0U);

    for (int i = 0; i < 2000; ++i) {
        (void)limiter.acquire("sweeper", after(std::chrono::seconds{60}));
    }

    EXPECT_LT(limiter.tracked_keys(), 600U) << "expired windows should not be kept forever";
}

TEST(SlidingWindowTest, ConcurrentAcquisitionsNeverExceedTheLimit) {
    constexpr std::size_t kLimit = 200;
    // A window far longer than the test, so the limit is a hard ceiling.
    SlidingWindowLimiter limiter(kLimit, std::chrono::hours{1});

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::atomic<std::size_t> allowed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, &allowed] {
            for (int i = 0; i < kPerThread; ++i) {
                if (limiter.acquire("shared", Clock::now()).allowed) {
                    ++allowed;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(allowed.load(), kLimit);
}

TEST(SlidingWindowTest, IndependentKeysDoNotSerialiseOnEachOther) {
    SlidingWindowLimiter limiter(1000000, std::chrono::hours{1});

    constexpr int kThreads = 8;
    std::atomic<std::size_t> allowed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&limiter, &allowed, t] {
            const std::string key = "client-" + std::to_string(t);
            for (int i = 0; i < 500; ++i) {
                if (limiter.acquire(key, Clock::now()).allowed) {
                    ++allowed;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(allowed.load(), 8U * 500U);
    EXPECT_EQ(limiter.tracked_keys(), 8U);
}

}  // namespace
