// Unit tests for the circuit-breaker state machine and the per-instance
// registry. No sockets: outcomes are recorded directly.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include "gateway/circuit_breaker.hpp"
#include "gateway/config.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::BackendEndpoint;
using gateway::BackendTable;
using gateway::CircuitBreaker;
using gateway::CircuitBreakers;
using gateway::CircuitState;

// Long enough that an open circuit stays open for the whole test.
constexpr std::chrono::milliseconds kLongCooldown{60000};
// Zero makes OPEN -> HALF_OPEN happen on the very next acquire, so the
// transition can be tested without waiting on a clock.
constexpr std::chrono::milliseconds kNoCooldown{0};

/// Drives `count` acquired failures through a breaker.
void fail(CircuitBreaker& breaker, int count) {
    for (int i = 0; i < count; ++i) {
        (void)breaker.try_acquire();
        breaker.record_failure();
    }
}

TEST(CircuitBreakerTest, StartsClosedAndAdmitsRequests) {
    CircuitBreaker breaker(3, kLongCooldown);

    EXPECT_EQ(breaker.state(), CircuitState::kClosed);
    EXPECT_TRUE(breaker.try_acquire());
    EXPECT_FALSE(breaker.blocks_selection());
}

TEST(CircuitBreakerTest, SuccessKeepsItClosed) {
    CircuitBreaker breaker(3, kLongCooldown);

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(breaker.try_acquire());
        breaker.record_success();
    }
    EXPECT_EQ(breaker.state(), CircuitState::kClosed);
    EXPECT_EQ(breaker.consecutive_failures(), 0U);
}

TEST(CircuitBreakerTest, FailuresBelowThresholdKeepItClosed) {
    CircuitBreaker breaker(3, kLongCooldown);

    fail(breaker, 2);

    EXPECT_EQ(breaker.state(), CircuitState::kClosed);
    EXPECT_EQ(breaker.consecutive_failures(), 2U);
    EXPECT_TRUE(breaker.try_acquire());
}

TEST(CircuitBreakerTest, ReachingTheThresholdOpensIt) {
    CircuitBreaker breaker(3, kLongCooldown);

    fail(breaker, 3);

    EXPECT_EQ(breaker.state(), CircuitState::kOpen);
}

TEST(CircuitBreakerTest, OpenRejectsRequestsDuringCooldown) {
    CircuitBreaker breaker(1, kLongCooldown);
    fail(breaker, 1);
    ASSERT_EQ(breaker.state(), CircuitState::kOpen);

    EXPECT_FALSE(breaker.try_acquire());
    EXPECT_FALSE(breaker.try_acquire());
    EXPECT_TRUE(breaker.blocks_selection()) << "selection must skip a cooling circuit";
}

TEST(CircuitBreakerTest, SuccessResetsTheFailureCounter) {
    CircuitBreaker breaker(3, kLongCooldown);

    fail(breaker, 2);
    ASSERT_EQ(breaker.consecutive_failures(), 2U);

    ASSERT_TRUE(breaker.try_acquire());
    breaker.record_success();
    EXPECT_EQ(breaker.consecutive_failures(), 0U);

    // The counter restarted, so two more failures still do not open it.
    fail(breaker, 2);
    EXPECT_EQ(breaker.state(), CircuitState::kClosed);
}

TEST(CircuitBreakerTest, ElapsedCooldownAdmitsOneProbeIntoHalfOpen) {
    CircuitBreaker breaker(1, kNoCooldown);
    fail(breaker, 1);
    ASSERT_EQ(breaker.state(), CircuitState::kOpen);

    EXPECT_FALSE(breaker.blocks_selection()) << "an expired cooldown must not block selection";
    EXPECT_TRUE(breaker.try_acquire());
    EXPECT_EQ(breaker.state(), CircuitState::kHalfOpen);

    // Only one probe at a time while the outcome is unknown.
    EXPECT_FALSE(breaker.try_acquire());
}

TEST(CircuitBreakerTest, CooldownIsHonouredBeforeTheProbe) {
    CircuitBreaker breaker(1, std::chrono::milliseconds{80});
    fail(breaker, 1);

    EXPECT_FALSE(breaker.try_acquire()) << "still cooling down";

    std::this_thread::sleep_for(std::chrono::milliseconds{160});
    EXPECT_TRUE(breaker.try_acquire());
    EXPECT_EQ(breaker.state(), CircuitState::kHalfOpen);
}

TEST(CircuitBreakerTest, SuccessfulProbeClosesTheCircuit) {
    CircuitBreaker breaker(2, kNoCooldown);
    fail(breaker, 2);
    ASSERT_TRUE(breaker.try_acquire());
    ASSERT_EQ(breaker.state(), CircuitState::kHalfOpen);

    breaker.record_success();

    EXPECT_EQ(breaker.state(), CircuitState::kClosed);
    EXPECT_EQ(breaker.consecutive_failures(), 0U);
    EXPECT_TRUE(breaker.try_acquire()) << "a closed circuit admits normal traffic again";
}

TEST(CircuitBreakerTest, FailedProbeReopensTheCircuit) {
    CircuitBreaker breaker(2, kLongCooldown);
    fail(breaker, 2);
    ASSERT_EQ(breaker.state(), CircuitState::kOpen);

    // A breaker with a long cooldown cannot probe yet; use one that can.
    CircuitBreaker probeable(2, kNoCooldown);
    fail(probeable, 2);
    ASSERT_TRUE(probeable.try_acquire());
    ASSERT_EQ(probeable.state(), CircuitState::kHalfOpen);

    probeable.record_failure();

    EXPECT_EQ(probeable.state(), CircuitState::kOpen) << "a failed probe reopens immediately";
}

TEST(CircuitBreakerTest, HalfOpenAdmitsAnotherProbeAfterTheFirstResolves) {
    CircuitBreaker breaker(1, kNoCooldown);
    fail(breaker, 1);

    ASSERT_TRUE(breaker.try_acquire());
    breaker.record_failure();  // Reopens; cooldown is zero, so probing may resume.

    EXPECT_TRUE(breaker.try_acquire());
    EXPECT_EQ(breaker.state(), CircuitState::kHalfOpen);
}

TEST(CircuitBreakerTest, ConcurrentUseIsRaceFreeAndAdmitsOneProbeAtATime) {
    CircuitBreaker breaker(1, kNoCooldown);
    fail(breaker, 1);
    ASSERT_EQ(breaker.state(), CircuitState::kOpen);

    constexpr int kThreads = 8;
    std::atomic<int> admitted{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&breaker, &admitted] {
            if (breaker.try_acquire()) {
                ++admitted;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(admitted.load(), 1) << "exactly one probe may be in flight";
}

TEST(CircuitBreakerTest, ConcurrentMixedOutcomesLeaveConsistentState) {
    CircuitBreaker breaker(1000, kLongCooldown);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&breaker, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (!breaker.try_acquire()) {
                    continue;
                }
                if ((t + i) % 2 == 0) {
                    breaker.record_success();
                } else {
                    breaker.record_failure();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // The threshold is far above any run of consecutive failures a fair mix can
    // produce, so the only requirement is that the state stayed coherent.
    const CircuitState state = breaker.state();
    EXPECT_TRUE(state == CircuitState::kClosed || state == CircuitState::kOpen ||
                state == CircuitState::kHalfOpen);
    EXPECT_LE(breaker.consecutive_failures(), 1000U);
}

TEST(CircuitBreakersTest, EachInstanceHasItsOwnBreaker) {
    const BackendTable table{
        {"users", {BackendEndpoint{"127.0.0.1", 9001}, BackendEndpoint{"127.0.0.1", 9002}}},
        {"orders", {BackendEndpoint{"127.0.0.1", 9010}}},
    };
    const CircuitBreakers breakers(table, 1, kLongCooldown);

    CircuitBreaker* first = breakers.find("users", 0);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->try_acquire());
    first->record_failure();

    EXPECT_TRUE(breakers.blocks_selection("users", 0));
    EXPECT_FALSE(breakers.blocks_selection("users", 1)) << "a sibling instance is unaffected";
    EXPECT_FALSE(breakers.blocks_selection("orders", 0)) << "another service is unaffected";
}

TEST(CircuitBreakersTest, UnknownServiceAndIndexAreSafe) {
    const CircuitBreakers breakers(BackendTable{{"users", {BackendEndpoint{"127.0.0.1", 9001}}}}, 1,
                                   kLongCooldown);

    EXPECT_EQ(breakers.find("orders", 0), nullptr);
    EXPECT_EQ(breakers.find("users", 7), nullptr);
    EXPECT_FALSE(breakers.blocks_selection("orders", 0));
    EXPECT_FALSE(breakers.blocks_selection("users", 7));
}

TEST(CircuitBreakersTest, OpenCircuitRemovesAnInstanceFromSelection) {
    gateway_test::SelectionPool pool(
        BackendTable{{"users",
                      {BackendEndpoint{"127.0.0.1", 9001}, BackendEndpoint{"127.0.0.1", 9002},
                       BackendEndpoint{"127.0.0.1", 9003}}}},
        1, kLongCooldown);

    CircuitBreaker* second = pool.breakers.find("users", 1);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(second->try_acquire());
    second->record_failure();

    // The remaining two alternate evenly, exactly as an unhealthy instance would.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(pool.next_port("users"), 9001);
        EXPECT_EQ(pool.next_port("users"), 9003);
    }
}

TEST(CircuitBreakersTest, HalfOpenInstanceStaysSelectableSoItCanBeProbed) {
    gateway_test::SelectionPool pool(
        BackendTable{{"users", {BackendEndpoint{"127.0.0.1", 9001}}}}, 1, kNoCooldown);

    CircuitBreaker* only = pool.breakers.find("users", 0);
    ASSERT_NE(only, nullptr);
    ASSERT_TRUE(only->try_acquire());
    only->record_failure();
    ASSERT_EQ(only->state(), CircuitState::kOpen);

    // Cooldown has already expired, so selection must offer it up again;
    // otherwise the circuit could never recover.
    EXPECT_EQ(pool.next_port("users"), 9001);
}

TEST(CircuitBreakersTest, CircuitAndHealthAreIndependent) {
    gateway_test::SelectionPool pool(
        BackendTable{{"users", {BackendEndpoint{"127.0.0.1", 9001},
                                BackendEndpoint{"127.0.0.1", 9002}}}},
        1, kLongCooldown);

    // Instance 0 is unhealthy, instance 1 has an open circuit: nothing is left.
    pool.health.set_healthy("users", 0, false);
    CircuitBreaker* second = pool.breakers.find("users", 1);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(second->try_acquire());
    second->record_failure();

    EXPECT_EQ(pool.next_port("users"), 0);

    // Closing the circuit brings instance 1 back without touching health state.
    second->record_success();
    EXPECT_EQ(pool.next_port("users"), 9002);
    EXPECT_FALSE(pool.health.is_healthy("users", 0)) << "health state was not modified";
}

}  // namespace
