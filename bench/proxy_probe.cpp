// Attribution probe for the outbound proxy path (Stage 11D).
//
// Benchmark-only: it links the gateway library and calls the real
// ReverseProxy::forward, then re-creates the same sequence step by step so the
// phases can be timed individually. cpp-httplib performs resolve, connect,
// send and receive inside one send() call, so those cannot be split from
// outside; they are bounded here with independent syscall-level measurements
// instead. Nothing here runs in the gateway.

#include <httplib.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/config.hpp"
#include "gateway/proxy.hpp"

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kIterations = 4000;
constexpr int kWarmup = 400;

struct Stats {
    double mean_us{0};
    double p50_us{0};
    double p99_us{0};
};

/// Runs `work` repeatedly and reports the per-iteration cost.
template <typename Work>
Stats measure(Work work, int iterations = kIterations) {
    for (int i = 0; i < kWarmup; ++i) {
        work();
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        work();
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());

    Stats stats;
    for (const double sample : samples) {
        stats.mean_us += sample;
    }
    stats.mean_us /= static_cast<double>(samples.size());
    stats.p50_us = samples[samples.size() / 2];
    stats.p99_us = samples[static_cast<std::size_t>(static_cast<double>(samples.size()) * 0.99)];
    return stats;
}

void report(const char* label, const Stats& stats) {
    std::printf("  %-46s mean=%8.1fus  p50=%8.1fus  p99=%9.1fus\n", label, stats.mean_us,
                stats.p50_us, stats.p99_us);
}

/// Minimal HTTP/1.1 client, benchmark-only. Just enough to send a GET and read
/// the complete response, so it can be compared against cpp-httplib's client
/// doing the same work. Not an HTTP implementation and never used in the gateway.
class RawClient {
public:
    RawClient(const std::string& host, const std::string& port, const std::string& extra = "") {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved_) != 0) {
            resolved_ = nullptr;
        }
        wire_ = "GET /users/1 HTTP/1.1\r\nHost: " + host + ':' + port +
                "\r\nAccept: */*\r\nUser-Agent: proxy-probe\r\n" + extra + "\r\n";
        buffer_.resize(16384);
    }

    ~RawClient() {
        disconnect();
        if (resolved_ != nullptr) {
            freeaddrinfo(resolved_);
        }
    }

    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    [[nodiscard]] bool connect() {
        if (resolved_ == nullptr) {
            return false;
        }
        fd_ = ::socket(resolved_->ai_family, resolved_->ai_socktype, resolved_->ai_protocol);
        if (fd_ < 0) {
            return false;
        }
        if (::connect(fd_, resolved_->ai_addr, resolved_->ai_addrlen) != 0) {
            disconnect();
            return false;
        }
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /// Sends one request and reads the whole response, headers and body.
    [[nodiscard]] bool exchange() {
        if (::send(fd_, wire_.data(), wire_.size(), 0) < 0) {
            return false;
        }
        std::size_t filled = 0;
        std::size_t header_end = std::string::npos;
        std::size_t expected = 0;

        while (true) {
            const ssize_t got = ::recv(fd_, buffer_.data() + filled, buffer_.size() - filled, 0);
            if (got <= 0) {
                return false;
            }
            filled += static_cast<std::size_t>(got);

            if (header_end == std::string::npos) {
                const std::string_view view(buffer_.data(), filled);
                const std::size_t marker = view.find("\r\n\r\n");
                if (marker == std::string_view::npos) {
                    continue;
                }
                header_end = marker + 4;
                const std::size_t at = view.find("Content-Length:");
                expected = at == std::string_view::npos
                               ? 0
                               : static_cast<std::size_t>(std::atoi(view.data() + at + 15));
            }
            if (filled >= header_end + expected) {
                return true;
            }
        }
    }

private:
    addrinfo* resolved_{nullptr};
    int fd_{-1};
    std::string wire_;
    std::vector<char> buffer_;
};

std::string g_host = "127.0.0.1";
int g_port = 9101;

/// The request the gateway would forward.
httplib::Request make_request() {
    httplib::Request request;
    request.method = "GET";
    request.path = "/users/1";
    request.target = "/users/1";
    request.set_header("Accept", "*/*");
    request.set_header("User-Agent", "proxy-probe");
    return request;
}

}  // namespace

int main(int argc, char** argv) {
    std::string phase;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            g_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            g_port = std::atoi(argv[++i]);
        } else if (arg == "--phase" && i + 1 < argc) {
            phase = argv[++i];
        }
    }

    // Single-phase modes exist so connection reuse can be confirmed by watching
    // TIME_WAIT while only one pattern is running.
    if (phase == "fresh" || phase == "keepalive") {
        const bool reuse = phase == "keepalive";
        httplib::Client shared(g_host, g_port);
        shared.set_keep_alive(true);
        const auto work = [&] {
            if (reuse) {
                (void)shared.Get("/users/1");
            } else {
                httplib::Client client(g_host, g_port);
                client.set_keep_alive(false);
                (void)client.Get("/users/1");
            }
        };
        const Stats stats = measure(work);
        std::printf("phase=%s ", phase.c_str());
        report("per request", stats);
        return 0;
    }

    const gateway::BackendEndpoint backend{g_host, static_cast<std::uint16_t>(g_port)};
    const auto timeout = std::chrono::milliseconds{5000};
    const httplib::Request request = make_request();

    // Fail fast rather than reporting timings for a backend that is not there.
    {
        httplib::Client probe(g_host, g_port);
        probe.set_connection_timeout(timeout);
        if (!probe.Get("/users/1")) {
            std::cerr << "proxy-probe: no backend at " << g_host << ':' << g_port << '\n';
            return 1;
        }
    }

    std::printf("proxy-probe: %s:%d, %d iterations after %d warm-up\n\n", g_host.c_str(), g_port,
                kIterations, kWarmup);

    std::printf("== ground truth: the real production path ==\n");
    const gateway::ReverseProxy proxy(timeout);
    report("ReverseProxy::forward (whole outbound step)", measure([&] {
               httplib::Response response;
               (void)proxy.forward(backend, request, response);
           }));

    std::printf("\n== replicated phases (same sequence, timed separately) ==\n");
    report("httplib::Client construct + configure (no I/O)", measure([&] {
               httplib::Client client(g_host, g_port);
               client.set_connection_timeout(timeout);
               client.set_read_timeout(timeout);
               client.set_write_timeout(timeout);
               client.set_keep_alive(false);
           }));

    report("client.send() [resolve+connect+send+recv+close]", measure([&] {
               httplib::Client client(g_host, g_port);
               client.set_connection_timeout(timeout);
               client.set_read_timeout(timeout);
               client.set_write_timeout(timeout);
               client.set_keep_alive(false);
               (void)client.send(request);
           }));

    {
        httplib::Client client(g_host, g_port);
        client.set_keep_alive(false);
        const httplib::Result result = client.send(request);
        report("response copy into a gateway Response", measure([&] {
                   httplib::Response out;
                   out.status = result->status;
                   for (const auto& [name, value] : result->headers) {
                       out.set_header(name, value);
                   }
                   out.set_content(result->body, result->get_header_value("Content-Type"));
               }));
    }

    std::printf("\n== independent syscall-level costs (bounds, not a split of send()) ==\n");
    const std::string port_text = std::to_string(g_port);
    report("getaddrinfo(host, port)", measure([&] {
               addrinfo hints{};
               hints.ai_family = AF_UNSPEC;
               hints.ai_socktype = SOCK_STREAM;
               addrinfo* result = nullptr;
               if (getaddrinfo(g_host.c_str(), port_text.c_str(), &hints, &result) == 0) {
                   freeaddrinfo(result);
               }
           }));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(g_host.c_str(), port_text.c_str(), &hints, &resolved) != 0) {
        std::cerr << "proxy-probe: cannot resolve backend\n";
        return 1;
    }

    report("socket() + connect() + close()", measure([&] {
               const int fd = ::socket(resolved->ai_family, resolved->ai_socktype,
                                       resolved->ai_protocol);
               if (fd >= 0) {
                   (void)::connect(fd, resolved->ai_addr, resolved->ai_addrlen);
                   ::close(fd);
               }
           }));

    {
        // cpp-httplib waits for readiness with select() before reading. Compare
        // that against a plain blocking recv() on the same socket.
        const int fd = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        const std::string wire = "GET /users/1 HTTP/1.1\r\nHost: " + g_host + ':' + port_text +
                                 "\r\nAccept: */*\r\n\r\n";
        if (fd >= 0 && ::connect(fd, resolved->ai_addr, resolved->ai_addrlen) == 0) {
            std::vector<char> buffer(4096);
            report("send() + select() + recv()  [httplib pattern]", measure([&] {
                       (void)::send(fd, wire.data(), wire.size(), 0);
                       fd_set readable;
                       FD_ZERO(&readable);
                       FD_SET(fd, &readable);
                       timeval tv{5, 0};
                       (void)::select(fd + 1, &readable, nullptr, nullptr, &tv);
                       (void)::recv(fd, buffer.data(), buffer.size(), 0);
                   }));
            ::close(fd);
        }
    }

    {
        // send+recv on an already established connection, no setup or teardown.
        const int fd = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        const std::string wire = "GET /users/1 HTTP/1.1\r\nHost: " + g_host + ':' + port_text +
                                 "\r\nAccept: */*\r\n\r\n";
        if (fd >= 0 && ::connect(fd, resolved->ai_addr, resolved->ai_addrlen) == 0) {
            std::vector<char> buffer(4096);
            report("send() + recv() on an established socket", measure([&] {
                       (void)::send(fd, wire.data(), wire.size(), 0);
                       (void)::recv(fd, buffer.data(), buffer.size(), 0);
                   }));
            ::close(fd);
        }
    }
    freeaddrinfo(resolved);

    std::printf("\n== headroom: what connection reuse would look like ==\n");
    std::printf("  (run with --phase fresh|keepalive and watch TIME_WAIT to confirm reuse)\n");
    report("fresh httplib::Client per request (as today)", measure([&] {
               httplib::Client client(g_host, g_port);
               client.set_keep_alive(false);
               (void)client.Get("/users/1");
           }));

    {
        httplib::Client reused(g_host, g_port);
        reused.set_keep_alive(true);
        report("one httplib::Client reused, keep-alive", measure([&] {
                   (void)reused.Get("/users/1");
               }));
    }

    std::printf("\n== A/B: cpp-httplib client vs a minimal raw client, same backend ==\n");
    {
        RawClient raw(g_host, port_text);
        if (raw.connect()) {
            report("raw client, reused connection", measure([&] { (void)raw.exchange(); }));
        }
    }
    {
        // The exact extra headers cpp-httplib adds, to test whether the request
        // bytes rather than the client explain the gap.
        RawClient raw(g_host, port_text, "Accept-Encoding: \r\nUser-Agent: cpp-httplib/0.18.7\r\n");
        if (raw.connect()) {
            report("raw client, httplib-shaped request headers", measure([&] {
                       (void)raw.exchange();
                   }));
        }
    }
    report("raw client, fresh connection per request", measure([&] {
               RawClient raw(g_host, port_text);
               if (raw.connect()) {
                   (void)raw.exchange();
               }
           }));

    // cpp-httplib defaults CPPHTTPLIB_TCP_NODELAY to false on both client and
    // server, so Nagle is active. Measured here only to attribute time; the
    // gateway is not changed.
    std::printf("\n== Nagle attribution (client-side TCP_NODELAY) ==\n");
    report("fresh client, TCP_NODELAY on", measure([&] {
               httplib::Client client(g_host, g_port);
               client.set_keep_alive(false);
               client.set_tcp_nodelay(true);
               (void)client.Get("/users/1");
           }));
    {
        httplib::Client reused(g_host, g_port);
        reused.set_keep_alive(true);
        reused.set_tcp_nodelay(true);
        report("reused client, keep-alive + TCP_NODELAY on", measure([&] {
                   (void)reused.Get("/users/1");
               }));
    }
    return 0;
}
