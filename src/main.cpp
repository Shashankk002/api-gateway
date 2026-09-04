#include <exception>
#include <iostream>

#include "gateway/config.hpp"
#include "gateway/server.hpp"

int main(int argc, char** argv) {
    try {
        const gateway::ServerConfig config = gateway::load_config(argc, argv);
        gateway::GatewayServer server(config);
        return server.run() ? 0 : 1;
    } catch (const std::invalid_argument& error) {
        std::cerr << "gateway: " << error.what() << "\n"
                  << "usage: api-gateway [--host <address>] [--port <1-65535>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "gateway: fatal error: " << error.what() << '\n';
        return 1;
    }
}
