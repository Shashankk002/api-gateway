// Unit tests for backend selection. No sockets: the LoadBalancer only decides
// which instance is next.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/load_balancer.hpp"
#include "gateway_fixture.hpp"

namespace {

using gateway::BackendEndpoint;
using gateway::BackendTable;
using gateway_test::SelectionPool;

BackendEndpoint at(std::uint16_t port) { return BackendEndpoint{"127.0.0.1", port}; }

/// The ports selected for `service` over `count` consecutive calls.
std::vector<std::uint16_t> selected_ports(const gateway::LoadBalancer& balancer,
                                          std::string_view service, std::size_t count) {
    std::vector<std::uint16_t> ports;
    ports.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto chosen = balancer.select(service);
        ports.push_back(chosen ? chosen.endpoint->port : 0);
    }
    return ports;
}

TEST(LoadBalancerTest, SingleInstanceIsAlwaysSelected) {
    SelectionPool pool(BackendTable{{"users", {at(9001)}}});

    EXPECT_EQ(selected_ports(pool.balancer, "users", 4),
              (std::vector<std::uint16_t>{9001, 9001, 9001, 9001}));
}

TEST(LoadBalancerTest, TwoInstancesAlternate) {
    SelectionPool pool(BackendTable{{"users", {at(9001), at(9002)}}});

    EXPECT_EQ(selected_ports(pool.balancer, "users", 4),
              (std::vector<std::uint16_t>{9001, 9002, 9001, 9002}));
}

TEST(LoadBalancerTest, ThreeInstancesRotateAndWrapAround) {
    SelectionPool pool(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

    EXPECT_EQ(selected_ports(pool.balancer, "users", 7),
              (std::vector<std::uint16_t>{9001, 9002, 9003, 9001, 9002, 9003, 9001}));
}

TEST(LoadBalancerTest, SelectionStartsAtTheFirstConfiguredInstance) {
    SelectionPool pool(BackendTable{{"users", {at(9001), at(9002)}}});

    const auto first = pool.balancer.select("users");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.endpoint->port, 9001);
}

TEST(LoadBalancerTest, EachServiceKeepsItsOwnPosition) {
    SelectionPool pool(BackendTable{
        {"users", {at(9001), at(9002), at(9003)}},
        {"orders", {at(9010), at(9011)}},
    });

    // Interleave the services; neither may disturb the other's rotation.
    EXPECT_EQ(pool.balancer.select("users").endpoint->port, 9001);
    EXPECT_EQ(pool.balancer.select("orders").endpoint->port, 9010);
    EXPECT_EQ(pool.balancer.select("users").endpoint->port, 9002);
    EXPECT_EQ(pool.balancer.select("orders").endpoint->port, 9011);
    EXPECT_EQ(pool.balancer.select("users").endpoint->port, 9003);
    EXPECT_EQ(pool.balancer.select("orders").endpoint->port, 9010);
    EXPECT_EQ(pool.balancer.select("users").endpoint->port, 9001);
}

TEST(LoadBalancerTest, DistributionIsEvenOverManyRotations) {
    SelectionPool pool(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

    std::map<std::uint16_t, int> counts;
    for (const std::uint16_t port : selected_ports(pool.balancer, "users", 300)) {
        ++counts[port];
    }

    ASSERT_EQ(counts.size(), 3U);
    for (const auto& [port, count] : counts) {
        EXPECT_EQ(count, 100) << "port " << port;
    }
}

TEST(LoadBalancerTest, UnknownServiceSelectsNothing) {
    SelectionPool pool(BackendTable{{"users", {at(9001)}}});

    EXPECT_FALSE(pool.balancer.select("orders"));
}

TEST(LoadBalancerTest, ServiceWithNoInstancesSelectsNothing) {
    SelectionPool pool(BackendTable{{"users", {}}});

    EXPECT_FALSE(pool.balancer.select("users"));
    EXPECT_FALSE(pool.balancer.select("users")) << "an empty pool must stay safe to call";
}

TEST(LoadBalancerTest, EmptyTableSelectsNothing) {
    SelectionPool pool{BackendTable{}};

    EXPECT_FALSE(pool.balancer.select("users"));
}

TEST(LoadBalancerTest, ConcurrentSelectionStaysBalancedAndValid) {
    SelectionPool pool(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

    constexpr int kThreads = 8;
    constexpr int kPerThread = 300;  // 2400 total, a multiple of 3.
    std::atomic<int> port_9001{0};
    std::atomic<int> port_9002{0};
    std::atomic<int> port_9003{0};

    std::atomic<int> unexpected{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto chosen = pool.balancer.select("users");
                if (!chosen) {
                    ++unexpected;
                    continue;
                }
                switch (chosen.endpoint->port) {
                    case 9001: ++port_9001; break;
                    case 9002: ++port_9002; break;
                    case 9003: ++port_9003; break;
                    default: ++unexpected; break;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Every selection takes a distinct ticket from the atomic counter, so an
    // exact split holds regardless of how the threads interleave.
    constexpr int kExpected = kThreads * kPerThread / 3;
    EXPECT_EQ(unexpected.load(), 0);
    EXPECT_EQ(port_9001.load(), kExpected);
    EXPECT_EQ(port_9002.load(), kExpected);
    EXPECT_EQ(port_9003.load(), kExpected);
}

TEST(LoadBalancerTest, DefaultTableShipsAMultiInstanceService) {
    SelectionPool pool(gateway::default_backends());

    ASSERT_TRUE(pool.balancer.select("users"));
    EXPECT_GT(pool.balancer.backends().at("users").size(), 1U);
    EXPECT_FALSE(pool.balancer.select("nonexistent"));
}

}  // namespace
