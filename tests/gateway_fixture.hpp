#pragma once

#include <gtest/gtest.h>
#include <httplib.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "gateway/config.hpp"
#include "gateway/router.hpp"
#include "gateway/server.hpp"

namespace gateway_test {

/// Base fixture for integration tests: runs a real GatewayServer on an
/// OS-assigned loopback port for the duration of one test, and hands out
/// clients pointed at it.
///
/// Subclasses override make_router() to choose the route table under test.
class GatewayServerTestBase : public ::testing::Test {
protected:
    /// The route table the server under test should serve. Defaults to the
    /// gateway's built-in table.
    virtual gateway::Router make_router() { return gateway::default_service_router(); }

    void SetUp() override {
        gateway::ServerConfig config;
        config.host = "127.0.0.1";
        server_ = std::make_unique<gateway::GatewayServer>(config, make_router());

        // Port 0 lets the OS pick a free port, so tests never collide.
        ASSERT_TRUE(server_->bind(0)) << "could not bind an ephemeral port";
        port_ = server_->bound_port();
        ASSERT_NE(port_, 0);

        serve_thread_ = std::thread([this] { (void)server_->serve(); });
        ASSERT_TRUE(server_->wait_until_ready());
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (serve_thread_.joinable()) {
            serve_thread_.join();
        }
    }

    httplib::Client make_client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(2, 0);
        return client;
    }

private:
    std::unique_ptr<gateway::GatewayServer> server_;
    std::thread serve_thread_;
    std::uint16_t port_{0};
};

}  // namespace gateway_test
