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
}

TEST_F(ConfigTest, FirstBackendFlagReplacesTheBuiltInTable) {
    const auto config = load({"--backend", "users=10.0.0.1:9001"});

    ASSERT_EQ(config.backends.size(), 1U);
    EXPECT_EQ(config.backends.at("users").host, "10.0.0.1");
    EXPECT_EQ(config.backends.at("users").port, 9001);
}

TEST_F(ConfigTest, FurtherBackendFlagsAddToTheTable) {
    const auto config =
        load({"--backend", "users=10.0.0.1:9001", "--backend", "orders=10.0.0.2:9002"});

    ASSERT_EQ(config.backends.size(), 2U);
    EXPECT_EQ(config.backends.at("orders").host, "10.0.0.2");
}

TEST_F(ConfigTest, BackendAcceptsAnHttpUrlForm) {
    const auto config = load({"--backend", "users=http://127.0.0.1:9001/"});

    EXPECT_EQ(config.backends.at("users").host, "127.0.0.1");
    EXPECT_EQ(config.backends.at("users").port, 9001);
}

TEST_F(ConfigTest, BackendsEnvironmentVariableIsCommaSeparated) {
    ::setenv("GATEWAY_BACKENDS", "users=127.0.0.1:9001,orders=127.0.0.1:9002", 1);

    const auto config = load({});

    ASSERT_EQ(config.backends.size(), 2U);
    EXPECT_EQ(config.backends.at("users").port, 9001);
    EXPECT_EQ(config.backends.at("orders").port, 9002);
}

TEST_F(ConfigTest, BackendFlagOverridesTheSameServiceFromTheEnvironment) {
    ::setenv("GATEWAY_BACKENDS", "users=127.0.0.1:9001", 1);

    const auto config = load({"--backend", "users=127.0.0.1:9999"});

    ASSERT_EQ(config.backends.size(), 1U);
    EXPECT_EQ(config.backends.at("users").port, 9999);
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

}  // namespace
