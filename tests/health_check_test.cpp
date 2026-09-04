// Unit tests for health state and for selection over it. No sockets: health
// results are set directly, so nothing depends on a real probe.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/health.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::BackendEndpoint;
using gateway::BackendHealth;
using gateway::BackendTable;
using gateway_test::SelectionPool;

BackendEndpoint at(std::uint16_t port) { return BackendEndpoint{"127.0.0.1", port}; }

BackendTable three_instances() { return BackendTable{{"users", {at(9001), at(9002), at(9003)}}}; }

std::vector<std::uint16_t> selected_ports(const gateway::LoadBalancer& balancer,
                                          std::string_view service, std::size_t count) {
    std::vector<std::uint16_t> ports;
    ports.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const BackendEndpoint* chosen = balancer.select(service);
        ports.push_back(chosen == nullptr ? 0 : chosen->port);
    }
    return ports;
}

TEST(BackendHealthTest, EveryInstanceStartsHealthy) {
    const BackendHealth health(three_instances());

    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(health.is_healthy("users", index)) << "index " << index;
    }
}

TEST(BackendHealthTest, StateChangesAreReportedOnlyWhenTheyChangeSomething) {
    BackendHealth health(three_instances());

    EXPECT_TRUE(health.set_healthy("users", 1, false)) << "healthy -> unhealthy is a change";
    EXPECT_FALSE(health.set_healthy("users", 1, false)) << "repeating it is not";
    EXPECT_TRUE(health.set_healthy("users", 1, true)) << "unhealthy -> healthy is a change";
}

TEST(BackendHealthTest, UnknownServiceAndOutOfRangeIndexAreSafe) {
    BackendHealth health(three_instances());

    EXPECT_FALSE(health.is_healthy("orders", 0));
    EXPECT_FALSE(health.is_healthy("users", 99));
    EXPECT_FALSE(health.set_healthy("orders", 0, false));
    EXPECT_FALSE(health.set_healthy("users", 99, false));
    EXPECT_TRUE(health.flags("orders").empty());
    EXPECT_EQ(health.flags("users").size(), 3U);
}

TEST(BackendHealthTest, WaitForReturnsImmediatelyWhenAlreadySatisfied) {
    const BackendHealth health(three_instances());

    EXPECT_TRUE(health.wait_for([&health] { return health.is_healthy("users", 0); },
                                std::chrono::milliseconds{50}));
}

TEST(BackendHealthTest, WaitForTimesOutWhenThePredicateNeverHolds) {
    const BackendHealth health(three_instances());

    EXPECT_FALSE(health.wait_for([&health] { return !health.is_healthy("users", 0); },
                                 std::chrono::milliseconds{50}));
}

TEST(BackendHealthTest, WaitForIsWokenByAChangeOnAnotherThread) {
    BackendHealth health(three_instances());

    std::thread writer([&health] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        health.set_healthy("users", 2, false);
    });
    const bool observed = health.wait_for([&health] { return !health.is_healthy("users", 2); },
                                          gateway_test::kHealthWaitTimeout);
    writer.join();

    EXPECT_TRUE(observed);
}

TEST(SelectionWithHealthTest, HealthyInstancesAreAllEligible) {
    SelectionPool pool(three_instances());

    EXPECT_EQ(selected_ports(pool.balancer, "users", 6),
              (std::vector<std::uint16_t>{9001, 9002, 9003, 9001, 9002, 9003}));
}

TEST(SelectionWithHealthTest, UnhealthyInstancesAreExcluded) {
    SelectionPool pool(three_instances());
    pool.health.set_healthy("users", 1, false);

    // The remaining two must alternate evenly rather than one of them absorbing
    // the excluded instance's turn.
    EXPECT_EQ(selected_ports(pool.balancer, "users", 6),
              (std::vector<std::uint16_t>{9001, 9003, 9001, 9003, 9001, 9003}));
}

TEST(SelectionWithHealthTest, OnlyOneHealthyInstanceTakesEveryRequest) {
    SelectionPool pool(three_instances());
    pool.health.set_healthy("users", 0, false);
    pool.health.set_healthy("users", 2, false);

    EXPECT_EQ(selected_ports(pool.balancer, "users", 4),
              (std::vector<std::uint16_t>{9002, 9002, 9002, 9002}));
}

TEST(SelectionWithHealthTest, AllUnhealthyMeansNothingIsSelectable) {
    SelectionPool pool(three_instances());
    for (std::size_t index = 0; index < 3; ++index) {
        pool.health.set_healthy("users", index, false);
    }

    EXPECT_EQ(pool.balancer.select("users"), nullptr);
    EXPECT_EQ(pool.balancer.select("users"), nullptr);
}

TEST(SelectionWithHealthTest, RecoveredInstanceBecomesSelectableAgain) {
    SelectionPool pool(three_instances());
    pool.health.set_healthy("users", 1, false);
    ASSERT_EQ(selected_ports(pool.balancer, "users", 2),
              (std::vector<std::uint16_t>{9001, 9003}));

    pool.health.set_healthy("users", 1, true);

    const auto ports = selected_ports(pool.balancer, "users", 6);
    EXPECT_NE(std::find(ports.begin(), ports.end(), 9002), ports.end())
        << "the recovered instance must re-enter the rotation";
    std::map<std::uint16_t, int> counts;
    for (const std::uint16_t port : ports) {
        ++counts[port];
    }
    EXPECT_EQ(counts.size(), 3U) << "all three instances share the rotation again";
}

TEST(SelectionWithHealthTest, RecoveryFromEverythingUnhealthyWorks) {
    SelectionPool pool(three_instances());
    for (std::size_t index = 0; index < 3; ++index) {
        pool.health.set_healthy("users", index, false);
    }
    ASSERT_EQ(pool.balancer.select("users"), nullptr);

    pool.health.set_healthy("users", 2, true);

    const BackendEndpoint* chosen = pool.balancer.select("users");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->port, 9003);
}

TEST(SelectionWithHealthTest, ServicesKeepIndependentHealthAndPosition) {
    SelectionPool pool(BackendTable{
        {"users", {at(9001), at(9002)}},
        {"orders", {at(9010), at(9011)}},
    });
    pool.health.set_healthy("users", 0, false);

    // users is down to one instance; orders must be untouched.
    EXPECT_EQ(pool.balancer.select("users")->port, 9002);
    EXPECT_EQ(pool.balancer.select("orders")->port, 9010);
    EXPECT_EQ(pool.balancer.select("users")->port, 9002);
    EXPECT_EQ(pool.balancer.select("orders")->port, 9011);
    EXPECT_EQ(pool.balancer.select("orders")->port, 9010);
}

TEST(SelectionWithHealthTest, ConcurrentUpdatesAndSelectionsStayValid) {
    SelectionPool pool(three_instances());

    constexpr int kSelectors = 6;
    constexpr int kSelectionsPerThread = 2000;
    std::atomic<bool> stop_flipping{false};
    std::atomic<int> invalid{0};
    std::atomic<int> selected{0};

    // Flips health under the selectors the whole time; instance 0 always stays
    // healthy, so a selection must always be possible.
    std::thread flipper([&pool, &stop_flipping] {
        bool healthy = false;
        while (!stop_flipping.load(std::memory_order_relaxed)) {
            pool.health.set_healthy("users", 1, healthy);
            pool.health.set_healthy("users", 2, !healthy);
            healthy = !healthy;
        }
    });

    std::vector<std::thread> selectors;
    selectors.reserve(kSelectors);
    for (int t = 0; t < kSelectors; ++t) {
        selectors.emplace_back([&pool, &invalid, &selected] {
            for (int i = 0; i < kSelectionsPerThread; ++i) {
                const BackendEndpoint* chosen = pool.balancer.select("users");
                if (chosen == nullptr || chosen->port < 9001 || chosen->port > 9003) {
                    ++invalid;
                    continue;
                }
                ++selected;
            }
        });
    }
    for (std::thread& selector : selectors) {
        selector.join();
    }
    stop_flipping.store(true, std::memory_order_relaxed);
    flipper.join();

    EXPECT_EQ(invalid.load(), 0);
    EXPECT_EQ(selected.load(), kSelectors * kSelectionsPerThread);
}

}  // namespace
