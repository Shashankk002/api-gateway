// Minimal HTTP backend for the Docker Compose demo.
//
// It exists only so the gateway has something to route to, load balance across,
// health check and fail over from. It is not part of the gateway library or
// binary, and it is built only when API_GATEWAY_BUILD_DEMO_BACKEND is on.

#include <httplib.h>

#include <csignal>
#include <cstdlib>
#include <string>

namespace {

httplib::Server* g_server = nullptr;

// Docker stops a container with SIGTERM, and httplib's listen() returns only
// once stop() is called. Without this the container would sit until the stop
// timeout expired and then be killed.
extern "C" void handle_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

const char* env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : fallback;
}

}  // namespace

int main() {
    const std::string name = env_or("BACKEND_NAME", "backend");
    const int port = std::atoi(env_or("BACKEND_PORT", "9000"));

    const std::string body = R"({"backend":")" + name + R"("})";
    const std::string health = R"({"status":"healthy","backend":")" + name + R"("})";

    httplib::Server server;
    g_server = &server;
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    server.Get("/health", [&health](const httplib::Request&, httplib::Response& response) {
        response.set_content(health, "application/json");
    });
    server.Get(".*", [&body, &name](const httplib::Request&, httplib::Response& response) {
        response.set_header("X-Backend", name);
        response.set_content(body, "application/json");
    });

    return server.listen("0.0.0.0", port) ? 0 : 1;
}
