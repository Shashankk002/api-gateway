#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "gateway/config.hpp"

namespace {

gateway::ServerConfig load(const std::vector<const char*>& args) {
    // argv[0] is the program name, which load_config skips.
    std::vector<const char*> argv{"api-gateway"};
    argv.insert(argv.end(), args.begin(), args.end());
    return gateway::load_config(static_cast<int>(argv.size()), argv.data());
}

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override { clear_environment(); }
    void TearDown() override { clear_environment(); }

    static void clear_environment() {
        ::unsetenv("GATEWAY_HOST");
        ::unsetenv("GATEWAY_PORT");
        ::unsetenv("GATEWAY_BACKENDS");
        ::unsetenv("GATEWAY_BACKEND_TIMEOUT_MS");
        ::unsetenv("GATEWAY_HEALTH_CHECK_INTERVAL_MS");
    }
};

TEST_F(ConfigTest, DefaultsToPort8080) {
    const auto config = load({});
    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.host, "0.0.0.0");
}

TEST_F(ConfigTest, PortFlagOverridesDefault) {
    const auto config = load({"--port", "9090"});
    EXPECT_EQ(config.port, 9090);
}

TEST_F(ConfigTest, HostFlagOverridesDefault) {
    const auto config = load({"--host", "127.0.0.1"});
    EXPECT_EQ(config.host, "127.0.0.1");
}

TEST_F(ConfigTest, EnvironmentOverridesDefault) {
    ::setenv("GATEWAY_PORT", "9100", 1);
    ::setenv("GATEWAY_HOST", "127.0.0.1", 1);

    const auto config = load({});
    EXPECT_EQ(config.port, 9100);
    EXPECT_EQ(config.host, "127.0.0.1");
}

TEST_F(ConfigTest, FlagOverridesEnvironment) {
    ::setenv("GATEWAY_PORT", "9100", 1);

    const auto config = load({"--port", "9200"});
    EXPECT_EQ(config.port, 9200);
}

TEST_F(ConfigTest, RejectsNonNumericPort) {
    EXPECT_THROW((void)load({"--port", "http"}), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsOutOfRangePort) {
    EXPECT_THROW((void)load({"--port", "70000"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--port", "0"}), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsMissingPortValue) {
    EXPECT_THROW((void)load({"--port"}), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsUnknownArgument) {
    EXPECT_THROW((void)load({"--proxy"}), std::invalid_argument);
}

TEST_F(ConfigTest, DefaultsToTheBuiltInBackendTable) {
    const auto config = load({});

    EXPECT_EQ(config.backends, gateway::default_backends());
    EXPECT_EQ(config.backend_timeout, gateway::ServerConfig::kDefaultBackendTimeout);
    // At least one built-in service must be multi-instance for the default
    // deployment to exercise load balancing.
    EXPECT_GT(config.backends.at("users").size(), 1U);
}

TEST_F(ConfigTest, FirstBackendFlagReplacesTheBuiltInTable) {
    const auto config = load({"--backend", "users=10.0.0.1:9001"});

    ASSERT_EQ(config.backends.size(), 1U);
    ASSERT_EQ(config.backends.at("users").size(), 1U);
    EXPECT_EQ(config.backends.at("users").front().host, "10.0.0.1");
    EXPECT_EQ(config.backends.at("users").front().port, 9001);
}

TEST_F(ConfigTest, FurtherBackendFlagsAddToTheTable) {
    const auto config =
        load({"--backend", "users=10.0.0.1:9001", "--backend", "orders=10.0.0.2:9002"});

    ASSERT_EQ(config.backends.size(), 2U);
    EXPECT_EQ(config.backends.at("orders").front().host, "10.0.0.2");
}

TEST_F(ConfigTest, RepeatingAServiceAddsInstancesInOrder) {
    const auto config = load({"--backend", "users=127.0.0.1:9001", "--backend",
                              "users=127.0.0.1:9002", "--backend", "users=127.0.0.1:9003"});

    ASSERT_EQ(config.backends.size(), 1U);
    const auto& instances = config.backends.at("users");
    ASSERT_EQ(instances.size(), 3U);
    EXPECT_EQ(instances[0].port, 9001);
    EXPECT_EQ(instances[1].port, 9002);
    EXPECT_EQ(instances[2].port, 9003);
}

TEST_F(ConfigTest, BackendAcceptsAnHttpUrlForm) {
    const auto config = load({"--backend", "users=http://127.0.0.1:9001/"});

    EXPECT_EQ(config.backends.at("users").front().host, "127.0.0.1");
    EXPECT_EQ(config.backends.at("users").front().port, 9001);
}

TEST_F(ConfigTest, BackendsEnvironmentVariableIsCommaSeparated) {
    ::setenv("GATEWAY_BACKENDS", "users=127.0.0.1:9001,users=127.0.0.1:9002,orders=127.0.0.1:9010",
             1);

    const auto config = load({});

    ASSERT_EQ(config.backends.size(), 2U);
    ASSERT_EQ(config.backends.at("users").size(), 2U);
    EXPECT_EQ(config.backends.at("users")[0].port, 9001);
    EXPECT_EQ(config.backends.at("users")[1].port, 9002);
    EXPECT_EQ(config.backends.at("orders").front().port, 9010);
}

TEST_F(ConfigTest, BackendFlagsAddToInstancesFromTheEnvironment) {
    ::setenv("GATEWAY_BACKENDS", "users=127.0.0.1:9001", 1);

    const auto config = load({"--backend", "users=127.0.0.1:9002"});

    ASSERT_EQ(config.backends.size(), 1U);
    ASSERT_EQ(config.backends.at("users").size(), 2U);
    EXPECT_EQ(config.backends.at("users")[0].port, 9001);
    EXPECT_EQ(config.backends.at("users")[1].port, 9002);
}

TEST_F(ConfigTest, RejectsMalformedBackendDefinitions) {
    EXPECT_THROW((void)load({"--backend", "users"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend", "=127.0.0.1:9001"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend", "users=127.0.0.1"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend", "users=127.0.0.1:abc"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend"}), std::invalid_argument);
}

TEST_F(ConfigTest, BackendTimeoutIsConfigurable) {
    EXPECT_EQ(load({"--backend-timeout-ms", "250"}).backend_timeout,
              std::chrono::milliseconds{250});

    ::setenv("GATEWAY_BACKEND_TIMEOUT_MS", "750", 1);
    EXPECT_EQ(load({}).backend_timeout, std::chrono::milliseconds{750});
    EXPECT_EQ(load({"--backend-timeout-ms", "100"}).backend_timeout,
              std::chrono::milliseconds{100});
}

TEST_F(ConfigTest, RejectsNonPositiveOrMalformedBackendTimeout) {
    EXPECT_THROW((void)load({"--backend-timeout-ms", "0"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend-timeout-ms", "soon"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--backend-timeout-ms"}), std::invalid_argument);
}

TEST_F(ConfigTest, HealthCheckIntervalDefaultsToTheBuiltInValue) {
    EXPECT_EQ(load({}).health_check_interval,
              gateway::ServerConfig::kDefaultHealthCheckInterval);
    EXPECT_GT(gateway::ServerConfig::kDefaultHealthCheckInterval.count(), 0);
}

TEST_F(ConfigTest, HealthCheckIntervalIsConfigurable) {
    EXPECT_EQ(load({"--health-check-interval-ms", "250"}).health_check_interval,
              std::chrono::milliseconds{250});

    ::setenv("GATEWAY_HEALTH_CHECK_INTERVAL_MS", "750", 1);
    EXPECT_EQ(load({}).health_check_interval, std::chrono::milliseconds{750});
    EXPECT_EQ(load({"--health-check-interval-ms", "100"}).health_check_interval,
              std::chrono::milliseconds{100});
}

TEST_F(ConfigTest, ZeroHealthCheckIntervalDisablesChecking) {
    EXPECT_EQ(load({"--health-check-interval-ms", "0"}).health_check_interval,
              std::chrono::milliseconds{0});
}

TEST_F(ConfigTest, RejectsMalformedOrUnreasonableHealthCheckInterval) {
    EXPECT_THROW((void)load({"--health-check-interval-ms", "often"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--health-check-interval-ms", "-1"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--health-check-interval-ms", "3600001"}), std::invalid_argument);
    // Beyond any integer type the parser could hold.
    EXPECT_THROW((void)load({"--health-check-interval-ms", "999999999999999999999"}),
                 std::invalid_argument);
    EXPECT_THROW((void)load({"--health-check-interval-ms"}), std::invalid_argument);
}

}  // namespace
