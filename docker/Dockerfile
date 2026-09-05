# Builds two images from one source tree: the gateway (target "gateway") and the
# Compose demo backend (target "backend"). They share the build stage, so the
# dependency fetch and compile happen once.

FROM debian:12-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Only what the gateway build reads. Tests, benchmarks and docs are excluded so
# that editing them does not invalidate this layer.
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
COPY docker/demo_backend.cpp docker/

# Tests are off, so GoogleTest is never fetched or built.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DAPI_GATEWAY_BUILD_TESTS=OFF \
        -DAPI_GATEWAY_BUILD_BENCH=OFF \
        -DAPI_GATEWAY_BUILD_DEMO_BACKEND=ON \
    && cmake --build build -j "$(nproc)"

# Runtime base: the C++ runtime the binaries link against, plus curl for the
# container health checks. No compiler, no CMake, no sources.
FROM debian:12-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends curl libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin app
USER app

FROM runtime AS gateway
COPY --from=build /src/build/api-gateway /usr/local/bin/api-gateway
EXPOSE 8080
# "Is the gateway process serving?" — deliberately not the same question as the
# gateway's own backend health checker. See the README.
HEALTHCHECK --interval=5s --timeout=2s --start-period=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/health || exit 1
ENTRYPOINT ["/usr/local/bin/api-gateway"]

FROM runtime AS backend
COPY --from=build /src/build/demo-backend /usr/local/bin/demo-backend
EXPOSE 9000
HEALTHCHECK --interval=5s --timeout=2s --start-period=3s --retries=3 \
    CMD curl -fsS http://127.0.0.1:9000/health || exit 1
ENTRYPOINT ["/usr/local/bin/demo-backend"]
