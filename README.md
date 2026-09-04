# api-gateway

A C++20 HTTP API gateway, built up in stages.

**Current stage: 7 — rate limiting.**

The gateway starts an HTTP listener, answers `GET /health` itself, and resolves
every other request against a route table that maps a method and path prefix to
a logical service name. A service may have several backend instances, which are
probed in the background; one of the currently healthy ones is picked
round-robin and the request is forwarded to it, with the backend's response
returned to the client. A transient failure on a safe method is retried once
onto another instance, and an instance that keeps failing has its circuit opened
so it stops receiving traffic until it recovers. Clients can be rate limited
per IP, locally or through Redis shared across gateway instances.

## Requirements

- A C++20 compiler (tested with Apple Clang 21)
- CMake 3.20 or newer
- Git and network access on the first configure (dependencies are fetched then)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The first configure downloads the dependencies listed below into `build/_deps`.

## Run

```sh
./build/api-gateway
```

By default the gateway listens on `0.0.0.0:8080`.

## Test

```sh
ctest --test-dir build --output-on-failure
```

The suite starts real `GatewayServer` instances on OS-assigned ports and drives
them over HTTP, so it exercises observable behaviour rather than internals.

The Redis integration tests need a live Redis and skip cleanly without one, so
the default run needs no external infrastructure. To run them:

```sh
docker run --rm -p 6379:6379 redis:7-alpine
```

`GATEWAY_REDIS_TEST_HOST` and `GATEWAY_REDIS_TEST_PORT` override the endpoint.

To build without tests, configure with `-DAPI_GATEWAY_BUILD_TESTS=OFF`.

## Rate limiting

Off by default: enabling throttling is an explicit decision, so the gateway
behaves exactly as before until asked. Turn it on with `--rate-limit on`.

A limited request is answered by the gateway alone with `429`. It never selects
a backend, never spends retry budget and never touches a circuit breaker —
throttling runs immediately after routing and before any backend machinery. The
gateway's own `/health` is answered before routing and is never limited.

```
429 Too Many Requests
Content-Type: application/json
RateLimit-Limit: 100
RateLimit-Remaining: 0
Retry-After: 1

{"status":"error","error":"rate_limited","reason":"rate_limit_exceeded","service":"users"}
```

`RateLimit-Limit` and `RateLimit-Remaining` are also set on allowed responses.
No reset timestamp is advertised: neither algorithm can state one honestly.

### Client key

Requests are keyed on the peer address the socket reports. `X-Forwarded-For`
and similar headers are deliberately ignored — without trusted-proxy
configuration they are client-supplied, and honouring them would let anyone
move themselves into someone else's bucket. Behind a real proxy this means every
request keys on the proxy's address, so terminate the proxy hop before the
gateway or add trusted-proxy handling first.

### Algorithms

Both read the configuration as "`--rate-limit-requests` per
`--rate-limit-window-ms`".

- **Token bucket** (default) — a burst of that many requests, then a steady
  refill of the same amount per window. Buckets start full and refill from
  elapsed time when a request arrives, so there is no timer thread and no
  request ever sleeps.
- **Sliding window** — at most that many requests in any trailing window.
  Timestamps of accepted requests are kept per key and expire as the window
  moves, so the allowance does not reset in a burst at a fixed boundary.

Local state is split across fixed shards so unrelated clients do not contend on
one mutex, and idle keys are swept out on later requests so memory stays bounded.

### Distributed mode

`--rate-limit-mode redis` moves state into Redis, so several gateway processes
share one budget. Every decision is a single `EVALSHA` of a token-bucket Lua
script: the read, refill, allow/deny and write happen in one Redis-side atomic
step. A local check followed by a Redis counter would race, so there is none.
The script takes its clock from `redis.call('TIME')`, so gateway hosts do not
need synchronised wall clocks.

Keys are `<prefix>:tb:<requests>-<window ms>:<client>`. The algorithm and the
policy parameters are part of the key, so changing a limit starts fresh buckets
and unrelated policies never share a counter. Keys carry a TTL, so Redis does
not accumulate state for clients that stop calling.

**Redis failure policy.** `--redis-failure-policy open` (the default) allows
requests when Redis cannot be reached: rate limiting is protection, not
authorization, and losing it should degrade protection rather than availability.
`closed` rejects instead, favouring protection at the cost of turning a Redis
outage into an outage for clients. There is no silent fallback to local
counting, which would quietly break the shared limit; the limiter counts these
fallbacks instead.

## Configuration

Settings are resolved lowest-to-highest precedence: built-in defaults,
environment variables, then command-line flags.

| Setting         | Default              | Environment                   | Flag                          |
| --------------- | -------------------- | ----------------------------- | ----------------------------- |
| Host            | `0.0.0.0`            | `GATEWAY_HOST`                | `--host <addr>`               |
| Port            | `8080`               | `GATEWAY_PORT`                | `--port <1-65535>`            |
| Backends        | built-in table       | `GATEWAY_BACKENDS`            | `--backend <svc>=<host>:<port>` |
| Backend timeout | `5000` ms            | `GATEWAY_BACKEND_TIMEOUT_MS`  | `--backend-timeout-ms <ms>`   |
| Health interval | `5000` ms            | `GATEWAY_HEALTH_CHECK_INTERVAL_MS` | `--health-check-interval-ms <ms>` |
| Max retries     | `1`                  | `GATEWAY_MAX_RETRIES`         | `--max-retries <0-10>`        |
| Circuit threshold | `5`                | `GATEWAY_CIRCUIT_FAILURE_THRESHOLD` | `--circuit-failure-threshold <1-1000>` |
| Circuit cooldown | `5000` ms           | `GATEWAY_CIRCUIT_COOLDOWN_MS` | `--circuit-cooldown-ms <0-3600000>` |
| Rate limiting   | `off`                | `GATEWAY_RATE_LIMIT`          | `--rate-limit on\|off`         |
| Algorithm       | `token-bucket`       | `GATEWAY_RATE_LIMIT_ALGORITHM` | `--rate-limit-algorithm token-bucket\|sliding-window` |
| Requests        | `100`                | `GATEWAY_RATE_LIMIT_REQUESTS` | `--rate-limit-requests <1-1000000>` |
| Window          | `60000` ms           | `GATEWAY_RATE_LIMIT_WINDOW_MS` | `--rate-limit-window-ms <1-3600000>` |
| Mode            | `local`              | `GATEWAY_RATE_LIMIT_MODE`     | `--rate-limit-mode local\|redis` |
| Redis host      | `127.0.0.1`          | `GATEWAY_REDIS_HOST`          | `--redis-host <addr>`         |
| Redis port      | `6379`               | `GATEWAY_REDIS_PORT`          | `--redis-port <1-65535>`      |
| Redis prefix    | `gateway:ratelimit`  | `GATEWAY_REDIS_KEY_PREFIX`    | `--redis-key-prefix <text>`   |
| Redis failure   | `open`               | `GATEWAY_REDIS_FAILURE_POLICY` | `--redis-failure-policy open\|closed` |

`--backend` may be repeated; `GATEWAY_BACKENDS` takes a comma-separated list of
the same `service=host:port` form. A leading `http://` is accepted and ignored.
Repeating a service name adds an instance to it, in the order given:

```sh
api-gateway --backend users=127.0.0.1:9001 --backend users=127.0.0.1:9002
```

```sh
GATEWAY_BACKENDS=users=127.0.0.1:9001,users=127.0.0.1:9002 api-gateway
```

The health-check interval accepts `0` to turn health checking off entirely,
leaving every configured instance eligible; otherwise it is 1–3600000 ms.
`--max-retries 0` disables retries, and `--circuit-cooldown-ms 0` lets an open
circuit probe on the very next request.

`config/gateway.env` holds the defaults in a form that a shell can source.

An invalid or unknown argument makes the process exit with status `2` and print
usage; a failure to bind the port exits with status `1`.

## Endpoints

### `GET /health`

Owned by the gateway itself. It is never offered to the router, so no route
table can shadow it.

```
200 OK
Content-Type: application/json

{"status":"healthy","service":"api-gateway"}
```

### A path covered by a route

The request is forwarded to the service's backend, and the backend's status,
body and headers come back unchanged.

```
$ curl http://localhost:8080/users/123
200 OK
Content-Type: application/json

<whatever the users backend returned for GET /users/123>
```

### A path covered by a route, with a method it does not accept

```
405 Method Not Allowed
Content-Type: application/json
Allow: GET

{"status":"error","error":"method_not_allowed","allowed":["GET"]}
```

### Any other path

```
404 Not Found
Content-Type: application/json

{"status":"error","error":"not_found"}
```

### The gateway could not reach the backend

```
502 Bad Gateway
Content-Type: application/json

{"status":"error","error":"bad_gateway","reason":"backend_unreachable","service":"users"}
```

`reason` is `no_backend_configured` when the matched service has no entry in the
backend table, and `backend_unreachable` when the connection failed.

### The backend did not answer in time

```
504 Gateway Timeout
Content-Type: application/json

{"status":"error","error":"gateway_timeout","reason":"backend_timeout","service":"users"}
```

## Routing

A route is a method, a path prefix, and a logical service name. The built-in
table (`default_service_router()` in `src/router.cpp`) is:

| Method | Prefix      | Service    |
| ------ | ----------- | ---------- |
| `GET`  | `/users`    | `users`    |
| `GET`  | `/orders`   | `orders`   |
| `GET`  | `/products` | `products` |

There is no route configuration format yet; the table lives in code, and
`GatewayServer` accepts an alternative table through its constructor.

### Matching rules

A route with prefix `P` **covers** a path when the path equals `P` or continues
with a `/` immediately after it. So `/users` covers `/users` and `/users/123`,
but not `/usersomething`. The prefix `/` covers every path. Trailing slashes are
trimmed on registration, so `/users/` and `/users` are the same route, and
methods are upper-cased on registration.

Matching then runs in two ordered steps:

1. **Resolve the path**, ignoring the method: take the longest prefix that
   covers it. Nothing covering it means `404`. Two different prefixes of equal
   length cannot both cover the same path, so the winner is unique.
2. **Resolve the method** among the routes sharing that winning prefix. An exact
   match is routed; otherwise the response is `405` with an `Allow` header.

Because the path is resolved first, a longer prefix shadows a shorter one even
when only the shorter one has the requested method: with `GET /api` and
`POST /api/admin` registered, `GET /api/admin/users` is a `405`, not a match on
`api`. This keeps the outcome a function of the path alone.

## Proxying

Routing decides *which* service a request belongs to; the backend table decides
*where* its instances are. The built-in table is:

| Service    | Instances                                            |
| ---------- | ---------------------------------------------------- |
| `users`    | `127.0.0.1:9001`, `127.0.0.1:9002`, `127.0.0.1:9003` |
| `orders`   | `127.0.0.1:9010`, `127.0.0.1:9011`                   |
| `products` | `127.0.0.1:9020`                                     |

Override it with `--backend` or `GATEWAY_BACKENDS` (see Configuration). The
first backend supplied from any source replaces the built-in table; later ones
append, so repeating a service name gives it more instances.

A matched request is forwarded with its method, its original request target
(path and query unchanged — there is no path rewriting), its body and its
headers. Hop-by-hop headers (`Connection`, `Keep-Alive`, `Proxy-Authenticate`,
`Proxy-Authorization`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`) are
dropped in both directions, and `Host` and `Content-Length` are regenerated for
the backend. The backend's status, body and remaining headers are returned to
the client as-is, so a backend `201` or `404` reaches the client as `201` or
`404` rather than becoming a gateway error.

Only services present in the backend table are ever contacted. The destination
is never taken from the request, so the gateway cannot be used as an open proxy.

### Load balancing

Requests to a service are spread across its instances in round-robin order:

```
request 1 -> 127.0.0.1:9001
request 2 -> 127.0.0.1:9002
request 3 -> 127.0.0.1:9003
request 4 -> 127.0.0.1:9001
```

Each service advances its own position, so traffic to `orders` never shifts the
rotation of `users`. Selection takes an atomic ticket and is correct when
requests arrive concurrently.

Only instances currently considered healthy take part. With `A` healthy, `B`
unhealthy and `C` healthy, the rotation becomes `A → C → A → C`, splitting
evenly rather than doubling up on whoever follows `B`.

A service that is routed but has no instances — or none currently healthy —
answers `502` with reason `no_backend_configured`.

## Health checks

A background thread probes every configured instance with `GET /health` sent
straight to that instance. This is the *backend's* health endpoint and has
nothing to do with the gateway's own `/health`, which is answered by the gateway
and never depends on backend health.

An instance is healthy when a response arrives with a 2xx status. Connection
failures, timeouts and any non-2xx status are unhealthy. Response bodies are not
inspected. Probes reuse the backend timeout, so a dead instance cannot stall a
sweep, and all instances in a sweep are probed in parallel so one slow instance
does not hold up the others.

**Startup.** Instances begin healthy. Before its first probe an instance is not
*known* to be bad, so the gateway serves normally from the first request instead
of rejecting everything during a startup window. The first sweep runs
immediately, so a dead instance is normally detected within one probe.

**Recovery.** Unhealthy instances keep being probed. When one starts answering
2xx again it re-enters the rotation on its own — no restart, no manual step.

Transitions are logged to stderr:

```
gateway: backend users 127.0.0.1:9002 is unhealthy
gateway: backend users 127.0.0.1:9002 is healthy
```

**Limitations at this stage.** Health state is only as fresh as the last sweep,
so an instance that dies between probes is still selected; a retry can recover
that request when the method is safe and another instance is eligible. A proxy
failure does not change health state — it feeds the circuit breaker instead.
There is no backoff, no weighting and no session affinity.

## Reliability

Every outbound request uses the backend timeout for connect, read and write. A
timeout that no retry rescues is still a `504`, and an unreachable backend is
still a `502`.

### Retry policy

A failed attempt is retried only when **both** of these hold:

- **The method is safe to repeat** — `GET`, `HEAD` or `OPTIONS`. `POST`, `PUT`,
  `PATCH` and `DELETE` are never replayed. A connection failure probably means
  the backend never saw the request, but the gateway cannot tell that apart from
  a failure after the backend already acted, so it does not guess.
- **The failure is transient** — the backend was unreachable, it timed out, or
  it answered `502`, `503` or `504`. Any other status, including `4xx` and a
  plain `500`, is the backend's answer and is passed straight through.

Retries are bounded by `--max-retries` (default `1`, so two attempts at most)
and each attempt goes to a *different* instance: an instance already tried in
this request is never tried again, so a single-instance service is never
retried. Circuit state is respected — a retry never lands on a circuit that is
refusing traffic. The request target, headers and body are re-sent unchanged on
each attempt, and a failed attempt contributes nothing to the final response.

When every attempt fails, the client gets the transport error (`502` or `504`)
or, if the last backend actually answered with `502`/`503`/`504`, that response
itself, which carries more information than a synthesised error would.

### Circuit breakers

Each backend instance has its own breaker, keyed to its position in the
service's configured list so state stays tied to the right endpoint.

| State | Behaviour |
| --- | --- |
| `CLOSED` | Normal traffic. Consecutive transient failures are counted; reaching the threshold opens the circuit. Any success resets the count. |
| `OPEN` | No traffic reaches the instance. After the cooldown it admits one probe. |
| `HALF_OPEN` | Exactly one probe is in flight. Success closes the circuit and clears the count; failure reopens it and restarts the cooldown. |

The same predicate that decides "retryable" decides "counts as a circuit
failure", so the two can never disagree: transport failures and `502`/`503`/`504`
count, and successes and ordinary `4xx`/`5xx` answers do not.

The load balancer skips an instance whose circuit is open and still cooling
down. A circuit past its cooldown stays selectable, which is how the probe gets
through. When no instance can be reached because every circuit is refusing
traffic, the gateway answers `503` with reason `circuit_open`.

Circuit state and health state are independent. Health reflects what background
probing observed; a circuit reflects how real requests have been failing.
Neither resets the other.

Outbound requests use a finite connect/read/write timeout, **5000 ms** by
default (`--backend-timeout-ms`). A refused connection is a `502`; exceeding the
timeout is a `504`.

## Layout

```
CMakeLists.txt          Top-level build definition
cmake/Dependencies.cmake  Pinned third-party dependencies (FetchContent)
config/gateway.env      Sourceable defaults
include/gateway/        Public headers
src/                    Implementation; main.cpp is the entry point only
tests/                  GoogleTest suite
```

`src/circuit_breaker.cpp`, `src/config.cpp`, `src/health.cpp`,
`src/load_balancer.cpp`, `src/proxy.cpp`, `src/rate_limiter.cpp`,
`src/redis_rate_limiter.cpp`, `src/router.cpp` and `src/server.cpp` build into
the `api_gateway_core` library, which both the
executable and the tests link against — the tests therefore run the same server
code that ships.

Responsibilities are split so each answers one question:

- `Router` — which logical service does this request belong to? Knows nothing
  about cpp-httplib, so it is unit tested without opening a socket.
- `ServerConfig` — where are that service's instances, and how long may they take?
- `BackendHealth` / `HealthChecker` — is this instance currently healthy? The
  only component that probes backends, on its own thread.
- `RateLimiter` — may this client make this request? `TokenBucketLimiter` and
  `SlidingWindowLimiter` hold local state; `RedisRateLimiter` holds none and
  defers to Redis. None of them knows anything about HTTP.
- `CircuitBreaker` — should this instance be given traffic right now, given how
  its recent requests went? Holds no network code.
- `LoadBalancer` — which eligible instance serves this request? Does no I/O and
  never probes; it only reads health and circuit state.
- `ReverseProxy` — how do I forward this request to a given instance and return
  the response? Never chooses among instances and never retries.
- `GatewayServer` — coordinates the HTTP lifecycle across the rest: it sequences
  select → circuit check → forward → record outcome → maybe retry, and owns the
  health checker's lifetime (started on construction, stopped and joined by
  `stop()` and by the destructor).

The proxy tests start a real backend server in-process and assert on what it
received, so they exercise the whole client → gateway → backend → client path.

## Dependencies

Both are fetched at configure time by CMake `FetchContent` and pinned to exact
tags, so builds are reproducible without a system-wide install.

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) `v0.18.7` — header-only
  HTTP server (and the client used by the tests).
- [hiredis](https://github.com/redis/hiredis) `v1.4.1` — minimal C client for
  Redis. Chosen over a heavier C++ wrapper because the gateway only needs
  `EVALSHA`/`EVAL`.
- [GoogleTest](https://github.com/google/googletest) `v1.15.2` — test framework;
  fetched only when `API_GATEWAY_BUILD_TESTS` is on.
