#include <gtest/gtest.h>

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

}  // namespace
