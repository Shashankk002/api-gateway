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
        ::unsetenv("GATEWAY_MAX_RETRIES");
        ::unsetenv("GATEWAY_CIRCUIT_FAILURE_THRESHOLD");
        ::unsetenv("GATEWAY_CIRCUIT_COOLDOWN_MS");
        ::unsetenv("GATEWAY_RATE_LIMIT");
        ::unsetenv("GATEWAY_RATE_LIMIT_ALGORITHM");
        ::unsetenv("GATEWAY_RATE_LIMIT_REQUESTS");
        ::unsetenv("GATEWAY_RATE_LIMIT_WINDOW_MS");
        ::unsetenv("GATEWAY_RATE_LIMIT_MODE");
        ::unsetenv("GATEWAY_REDIS_HOST");
        ::unsetenv("GATEWAY_REDIS_PORT");
        ::unsetenv("GATEWAY_REDIS_KEY_PREFIX");
        ::unsetenv("GATEWAY_REDIS_FAILURE_POLICY");
        ::unsetenv("GATEWAY_METRICS");
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

TEST_F(ConfigTest, ReliabilityDefaultsAreConservative) {
    const auto config = load({});

    EXPECT_EQ(config.max_retries, gateway::ServerConfig::kDefaultMaxRetries);
    EXPECT_LE(config.max_retries, 2U) << "the default retry budget must stay small";
    EXPECT_EQ(config.circuit_failure_threshold,
              gateway::ServerConfig::kDefaultCircuitFailureThreshold);
    EXPECT_GE(config.circuit_failure_threshold, 1U);
    EXPECT_EQ(config.circuit_cooldown, gateway::ServerConfig::kDefaultCircuitCooldown);
    EXPECT_GT(config.circuit_cooldown.count(), 0);
}

TEST_F(ConfigTest, ReliabilitySettingsAreConfigurableByFlag) {
    const auto config = load({"--max-retries", "3", "--circuit-failure-threshold", "7",
                              "--circuit-cooldown-ms", "1500"});

    EXPECT_EQ(config.max_retries, 3U);
    EXPECT_EQ(config.circuit_failure_threshold, 7U);
    EXPECT_EQ(config.circuit_cooldown, std::chrono::milliseconds{1500});
}

TEST_F(ConfigTest, ReliabilitySettingsAreConfigurableByEnvironment) {
    ::setenv("GATEWAY_MAX_RETRIES", "2", 1);
    ::setenv("GATEWAY_CIRCUIT_FAILURE_THRESHOLD", "9", 1);
    ::setenv("GATEWAY_CIRCUIT_COOLDOWN_MS", "2500", 1);

    EXPECT_EQ(load({}).max_retries, 2U);
    EXPECT_EQ(load({}).circuit_failure_threshold, 9U);
    EXPECT_EQ(load({}).circuit_cooldown, std::chrono::milliseconds{2500});

    // Flags still win over the environment.
    EXPECT_EQ(load({"--max-retries", "0"}).max_retries, 0U);
}

TEST_F(ConfigTest, ZeroRetriesAndZeroCooldownAreAccepted) {
    EXPECT_EQ(load({"--max-retries", "0"}).max_retries, 0U);
    EXPECT_EQ(load({"--circuit-cooldown-ms", "0"}).circuit_cooldown,
              std::chrono::milliseconds{0});
}

TEST_F(ConfigTest, RejectsInvalidReliabilitySettings) {
    EXPECT_THROW((void)load({"--max-retries", "11"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--max-retries", "many"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--max-retries", "-1"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--max-retries"}), std::invalid_argument);

    // A zero threshold would open a circuit before any request was made.
    EXPECT_THROW((void)load({"--circuit-failure-threshold", "0"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--circuit-failure-threshold", "1001"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--circuit-failure-threshold"}), std::invalid_argument);

    EXPECT_THROW((void)load({"--circuit-cooldown-ms", "3600001"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--circuit-cooldown-ms", "99999999999999999999"}),
                 std::invalid_argument);
    EXPECT_THROW((void)load({"--circuit-cooldown-ms"}), std::invalid_argument);
}

TEST_F(ConfigTest, RateLimitingIsOffByDefault) {
    const auto config = load({});

    EXPECT_FALSE(config.rate_limit_enabled) << "throttling must be an explicit decision";
    EXPECT_EQ(config.rate_limit_algorithm, gateway::RateLimitAlgorithm::kTokenBucket);
    EXPECT_EQ(config.rate_limit_mode, gateway::RateLimitMode::kLocal);
    EXPECT_EQ(config.rate_limit_requests, gateway::ServerConfig::kDefaultRateLimitRequests);
    EXPECT_EQ(config.rate_limit_window, gateway::ServerConfig::kDefaultRateLimitWindow);
    EXPECT_EQ(config.redis_failure_policy, gateway::RedisFailurePolicy::kFailOpen);
    EXPECT_EQ(config.redis_port, gateway::ServerConfig::kDefaultRedisPort);
}

TEST_F(ConfigTest, RateLimitSettingsAreConfigurableByFlag) {
    const auto config =
        load({"--rate-limit", "on", "--rate-limit-algorithm", "sliding-window",
              "--rate-limit-requests", "50", "--rate-limit-window-ms", "5000",
              "--rate-limit-mode", "redis", "--redis-host", "10.0.0.9", "--redis-port", "6380",
              "--redis-key-prefix", "edge:rl", "--redis-failure-policy", "closed"});

    EXPECT_TRUE(config.rate_limit_enabled);
    EXPECT_EQ(config.rate_limit_algorithm, gateway::RateLimitAlgorithm::kSlidingWindow);
    EXPECT_EQ(config.rate_limit_requests, 50U);
    EXPECT_EQ(config.rate_limit_window, std::chrono::milliseconds{5000});
    EXPECT_EQ(config.rate_limit_mode, gateway::RateLimitMode::kRedis);
    EXPECT_EQ(config.redis_host, "10.0.0.9");
    EXPECT_EQ(config.redis_port, 6380);
    EXPECT_EQ(config.redis_key_prefix, "edge:rl");
    EXPECT_EQ(config.redis_failure_policy, gateway::RedisFailurePolicy::kFailClosed);
}

TEST_F(ConfigTest, RateLimitSettingsAreConfigurableByEnvironment) {
    ::setenv("GATEWAY_RATE_LIMIT", "true", 1);
    ::setenv("GATEWAY_RATE_LIMIT_ALGORITHM", "sliding-window", 1);
    ::setenv("GATEWAY_RATE_LIMIT_REQUESTS", "25", 1);
    ::setenv("GATEWAY_RATE_LIMIT_MODE", "redis", 1);
    ::setenv("GATEWAY_REDIS_FAILURE_POLICY", "closed", 1);

    const auto config = load({});
    EXPECT_TRUE(config.rate_limit_enabled);
    EXPECT_EQ(config.rate_limit_algorithm, gateway::RateLimitAlgorithm::kSlidingWindow);
    EXPECT_EQ(config.rate_limit_requests, 25U);
    EXPECT_EQ(config.rate_limit_mode, gateway::RateLimitMode::kRedis);
    EXPECT_EQ(config.redis_failure_policy, gateway::RedisFailurePolicy::kFailClosed);

    // Flags still win over the environment.
    EXPECT_FALSE(load({"--rate-limit", "off"}).rate_limit_enabled);
}

TEST_F(ConfigTest, MetricsAreEnabledByDefaultAndCanBeTurnedOff) {
    EXPECT_TRUE(load({}).metrics_enabled) << "an unobservable gateway is hard to operate";
    EXPECT_FALSE(load({"--metrics", "off"}).metrics_enabled);

    ::setenv("GATEWAY_METRICS", "false", 1);
    EXPECT_FALSE(load({}).metrics_enabled);
    EXPECT_TRUE(load({"--metrics", "on"}).metrics_enabled) << "flags win over the environment";
}

TEST_F(ConfigTest, RejectsInvalidMetricsSetting) {
    EXPECT_THROW((void)load({"--metrics", "sometimes"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--metrics"}), std::invalid_argument);
}

TEST_F(ConfigTest, RejectsInvalidRateLimitSettings) {
    EXPECT_THROW((void)load({"--rate-limit", "maybe"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-algorithm", "leaky-bucket"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-mode", "memcached"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--redis-failure-policy", "maybe"}), std::invalid_argument);

    // A zero limit would reject everything; a zero window would divide by zero.
    EXPECT_THROW((void)load({"--rate-limit-requests", "0"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-requests", "1000001"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-window-ms", "0"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-window-ms", "3600001"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--rate-limit-requests", "99999999999999999999"}),
                 std::invalid_argument);
    EXPECT_THROW((void)load({"--redis-port", "0"}), std::invalid_argument);
    EXPECT_THROW((void)load({"--redis-port", "70000"}), std::invalid_argument);
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
