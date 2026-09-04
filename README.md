# api-gateway

A C++20 HTTP API gateway, built up in stages.

**Current stage: 2 — routing.**

The gateway starts an HTTP listener, answers `GET /health` itself, and resolves
every other request against a route table that maps a method and path prefix to
a logical service name. Matched requests get a response naming the selected
service; nothing is forwarded anywhere. Proxying, load balancing, rate limiting
and the rest of the eventual feature set are not implemented yet.

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

To build without tests, configure with `-DAPI_GATEWAY_BUILD_TESTS=OFF`.

## Configuration

Settings are resolved lowest-to-highest precedence: built-in defaults,
environment variables, then command-line flags.

| Setting | Default   | Environment    | Flag             |
| ------- | --------- | -------------- | ---------------- |
| Host    | `0.0.0.0` | `GATEWAY_HOST` | `--host <addr>`  |
| Port    | `8080`    | `GATEWAY_PORT` | `--port <1-65535>` |

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

Stage 2 does not forward anything, so a matched request reports which service
the router selected and why.

```
$ curl http://localhost:8080/users/123
200 OK
Content-Type: application/json

{"status":"routed","service":"users","matched_prefix":"/users","method":"GET","path":"/users/123"}
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

## Layout

```
CMakeLists.txt          Top-level build definition
cmake/Dependencies.cmake  Pinned third-party dependencies (FetchContent)
config/gateway.env      Sourceable defaults
include/gateway/        Public headers
src/                    Implementation; main.cpp is the entry point only
tests/                  GoogleTest suite
```

`src/config.cpp`, `src/router.cpp` and `src/server.cpp` build into the
`api_gateway_core` library, which both the executable and the tests link
against — the tests therefore run the same server code that ships.

`Router` holds all route matching and knows nothing about cpp-httplib, so it is
unit tested without opening a socket. `GatewayServer` asks it for a decision and
turns that decision into an HTTP response.

## Dependencies

Both are fetched at configure time by CMake `FetchContent` and pinned to exact
tags, so builds are reproducible without a system-wide install.

- [cpp-httplib](https://github.com/yhirose/cpp-httplib) `v0.18.7` — header-only
  HTTP server (and the client used by the tests).
- [GoogleTest](https://github.com/google/googletest) `v1.15.2` — test framework;
  fetched only when `API_GATEWAY_BUILD_TESTS` is on.
