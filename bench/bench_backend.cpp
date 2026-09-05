// Minimal HTTP backend for benchmarking.
//
// Deliberately does as little as possible per request, so that a gateway
// benchmark measures gateway overhead rather than backend work: one preformatted
// response, no logging, no per-request allocation beyond what cpp-httplib does
// itself, and no request recording. The test suite's TestBackend is unsuitable
// here because it stores every request it receives.
//
// Not part of the gateway library or binary.

#include <httplib.h>

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    int port{9101};
    std::size_t body_size{0};  ///< 0 keeps the default small JSON body.
};

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "bench-backend: " << message << "\n"
              << "usage: bench-backend [--host <addr>] [--port <1-65535>] "
                 "[--body-size <bytes>]\n";
    std::exit(2);
}

long parse_number(std::string_view text, std::string_view flag, long min, long max) {
    long value = 0;
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < min || value > max) {
        fail(std::string(flag) + ": expected " + std::to_string(min) + "-" + std::to_string(max));
    }
    return value;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&](std::string_view flag) -> std::string_view {
            if (++i >= argc) {
                fail(std::string(flag) + " requires a value");
            }
            return argv[i];
        };
        if (arg == "--host") {
            options.host = value(arg);
        } else if (arg == "--port") {
            options.port = static_cast<int>(parse_number(value(arg), arg, 1, 65535));
        } else if (arg == "--body-size") {
            options.body_size = static_cast<std::size_t>(parse_number(value(arg), arg, 0, 1 << 20));
        } else {
            fail("unknown argument '" + std::string(arg) + "'");
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    const std::string body = options.body_size == 0
                                 ? std::string(R"({"backend":"bench"})")
                                 : std::string(options.body_size, 'x');
    const std::string health = R"({"status":"healthy","service":"bench-backend"})";

    httplib::Server server;
    // Keep-alive is what a load generator will use; the default cap would force
    // a reconnect every 100 requests and measure connection setup instead.
    server.set_keep_alive_max_count(1000000);
    server.set_keep_alive_timeout(60);

    server.Get("/health", [&health](const httplib::Request&, httplib::Response& response) {
        response.set_content(health, "application/json");
    });
    server.Get(".*", [&body](const httplib::Request&, httplib::Response& response) {
        response.set_content(body, "application/json");
    });

    std::cout << "bench-backend: listening on " << options.host << ':' << options.port
              << " body=" << body.size() << "B" << std::endl;

    if (!server.listen(options.host, options.port)) {
        std::cerr << "bench-backend: failed to bind " << options.host << ':' << options.port
                  << '\n';
        return 1;
    }
    return 0;
}
