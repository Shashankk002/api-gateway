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

namespace {

using gateway::BackendEndpoint;
using gateway::BackendTable;
using gateway::LoadBalancer;

BackendEndpoint at(std::uint16_t port) { return BackendEndpoint{"127.0.0.1", port}; }

/// The ports selected for `service` over `count` consecutive calls.
std::vector<std::uint16_t> selected_ports(const LoadBalancer& balancer, std::string_view service,
                                          std::size_t count) {
    std::vector<std::uint16_t> ports;
    ports.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const BackendEndpoint* chosen = balancer.select(service);
        ports.push_back(chosen == nullptr ? 0 : chosen->port);
    }
    return ports;
}

TEST(LoadBalancerTest, SingleInstanceIsAlwaysSelected) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001)}}});

    EXPECT_EQ(selected_ports(balancer, "users", 4),
              (std::vector<std::uint16_t>{9001, 9001, 9001, 9001}));
}

TEST(LoadBalancerTest, TwoInstancesAlternate) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001), at(9002)}}});

    EXPECT_EQ(selected_ports(balancer, "users", 4),
              (std::vector<std::uint16_t>{9001, 9002, 9001, 9002}));
}

TEST(LoadBalancerTest, ThreeInstancesRotateAndWrapAround) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

    EXPECT_EQ(selected_ports(balancer, "users", 7),
              (std::vector<std::uint16_t>{9001, 9002, 9003, 9001, 9002, 9003, 9001}));
}

TEST(LoadBalancerTest, SelectionStartsAtTheFirstConfiguredInstance) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001), at(9002)}}});

    const BackendEndpoint* first = balancer.select("users");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->port, 9001);
}

TEST(LoadBalancerTest, EachServiceKeepsItsOwnPosition) {
    const LoadBalancer balancer(BackendTable{
        {"users", {at(9001), at(9002), at(9003)}},
        {"orders", {at(9010), at(9011)}},
    });

    // Interleave the services; neither may disturb the other's rotation.
    EXPECT_EQ(balancer.select("users")->port, 9001);
    EXPECT_EQ(balancer.select("orders")->port, 9010);
    EXPECT_EQ(balancer.select("users")->port, 9002);
    EXPECT_EQ(balancer.select("orders")->port, 9011);
    EXPECT_EQ(balancer.select("users")->port, 9003);
    EXPECT_EQ(balancer.select("orders")->port, 9010);
    EXPECT_EQ(balancer.select("users")->port, 9001);
}

TEST(LoadBalancerTest, DistributionIsEvenOverManyRotations) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

    std::map<std::uint16_t, int> counts;
    for (const std::uint16_t port : selected_ports(balancer, "users", 300)) {
        ++counts[port];
    }

    ASSERT_EQ(counts.size(), 3U);
    for (const auto& [port, count] : counts) {
        EXPECT_EQ(count, 100) << "port " << port;
    }
}

TEST(LoadBalancerTest, UnknownServiceSelectsNothing) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001)}}});

    EXPECT_EQ(balancer.select("orders"), nullptr);
}

TEST(LoadBalancerTest, ServiceWithNoInstancesSelectsNothing) {
    const LoadBalancer balancer(BackendTable{{"users", {}}});

    EXPECT_EQ(balancer.select("users"), nullptr);
    EXPECT_EQ(balancer.select("users"), nullptr) << "an empty pool must stay safe to call";
}

TEST(LoadBalancerTest, EmptyTableSelectsNothing) {
    const LoadBalancer balancer{BackendTable{}};

    EXPECT_EQ(balancer.select("users"), nullptr);
}

TEST(LoadBalancerTest, ConcurrentSelectionStaysBalancedAndValid) {
    const LoadBalancer balancer(BackendTable{{"users", {at(9001), at(9002), at(9003)}}});

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
                const BackendEndpoint* chosen = balancer.select("users");
                if (chosen == nullptr) {
                    ++unexpected;
                    continue;
                }
                switch (chosen->port) {
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
    const LoadBalancer balancer(gateway::default_backends());

    ASSERT_NE(balancer.select("users"), nullptr);
    EXPECT_GT(balancer.backends().at("users").size(), 1U);
    EXPECT_EQ(balancer.select("nonexistent"), nullptr);
}

}  // namespace
