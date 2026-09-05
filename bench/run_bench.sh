#!/usr/bin/env bash
#
# Reproducible benchmark driver: starts the benchmark backend and the gateway,
# warms both up, then runs a load generator against one or both.
#
# See bench/README.md for methodology. This script starts and stops everything
# it uses and leaves nothing running.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

BACKEND_PORT=9101
GATEWAY_PORT=8080
DURATION=30s
THREADS=4
CONNECTIONS=100
WARMUP=5s
TARGET=both
MATRIX=0
SMOKE=0
PATH_UNDER_TEST=/users/1
LOADGEN="${LOADGEN:-}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/bench/results}"

usage() {
    cat <<'USAGE'
usage: bench/run_bench.sh [options]

  --duration <t>      load phase per run (default 30s)
  --warmup <t>        discarded warm-up per run (default 5s)
  --threads <n>       load generator threads (default 4)
  --connections <n>   concurrent connections (default 100)
  --target <what>     direct | gateway | both (default both)
  --matrix            sweep 1/10/50/100 connections instead of a single run
  --smoke             start everything, verify it answers, tear down, exit
  --backend-port <n>  default 9101
  --gateway-port <n>  default 8080
  --loadgen <path>    load generator binary (default: wrk, else ab)
  --out <dir>         where raw output is written (default bench/results)
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --connections) CONNECTIONS="$2"; shift 2 ;;
        --target) TARGET="$2"; shift 2 ;;
        --matrix) MATRIX=1; shift ;;
        --smoke) SMOKE=1; shift ;;
        --backend-port) BACKEND_PORT="$2"; shift 2 ;;
        --gateway-port) GATEWAY_PORT="$2"; shift 2 ;;
        --loadgen) LOADGEN="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

BACKEND_BIN="$BUILD_DIR/bench/bench-backend"
GATEWAY_BIN="$BUILD_DIR/api-gateway"
for binary in "$BACKEND_BIN" "$GATEWAY_BIN"; do
    if [[ ! -x "$binary" ]]; then
        echo "missing $binary" >&2
        echo "build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
        exit 1
    fi
done

# wrk is preferred: it reports a latency distribution. ab is a fallback so the
# benchmark still runs on a machine without wrk, with fewer percentiles.
if [[ -z "$LOADGEN" ]]; then
    if command -v wrk >/dev/null 2>&1; then LOADGEN="$(command -v wrk)"
    elif command -v ab  >/dev/null 2>&1; then LOADGEN="$(command -v ab)"
    else
        echo "no load generator found; install wrk (brew install wrk) or pass --loadgen" >&2
        exit 1
    fi
fi
LOADGEN_KIND="$(basename "$LOADGEN")"

mkdir -p "$OUT_DIR"
BACKEND_PID=""
GATEWAY_PID=""

cleanup() {
    [[ -n "$GATEWAY_PID" ]] && kill "$GATEWAY_PID" 2>/dev/null || true
    [[ -n "$BACKEND_PID" ]] && kill "$BACKEND_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

wait_ready() {
    local url="$1" name="$2"
    for _ in $(seq 1 100); do
        if curl -fsS -o /dev/null --max-time 1 "$url" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "$name did not become ready at $url" >&2
    return 1
}

seconds_of() { echo "${1%s}"; }

# One measured run. Prints a single summary line.
run_load() {
    local label="$1" url="$2" connections="$3"
    local raw="$OUT_DIR/${label}-c${connections}.txt"

    # wrk refuses connections < threads, which a low-concurrency row would hit.
    local threads="$THREADS"
    (( threads > connections )) && threads="$connections"

    if [[ "$LOADGEN_KIND" == wrk* ]]; then
        "$LOADGEN" -t"$threads" -c"$connections" -d"$WARMUP" --latency "$url" >/dev/null 2>&1 || true
        if ! "$LOADGEN" -t"$threads" -c"$connections" -d"$DURATION" --latency "$url" >"$raw" 2>&1; then
            printf '  %-22s c=%-4s FAILED (see %s)\n' "$label" "$connections" "$raw"
            return 0
        fi
        local rps p50 p90 p99 avg
        rps=$(awk '/Requests\/sec/{print $2}' "$raw")
        avg=$(awk '/^ +Latency/{print $2}' "$raw" | head -1)
        p50=$(awk '/ 50%/{print $2}' "$raw")
        p90=$(awk '/ 90%/{print $2}' "$raw")
        p99=$(awk '/ 99%/{print $2}' "$raw")
        printf '  %-22s c=%-4s t=%-2s rps=%-11s avg=%-9s p50=%-9s p90=%-9s p99=%s\n' \
            "$label" "$connections" "$threads" "$rps" "$avg" "$p50" "$p90" "$p99"
    else
        # ab: requests rather than duration, and no p99 from a single line.
        local requests=$(( $(seconds_of "$DURATION") * 3000 ))
        "$LOADGEN" -k -c "$connections" -n $(( $(seconds_of "$WARMUP") * 3000 )) "$url" >/dev/null 2>&1 || true
        if ! "$LOADGEN" -k -c "$connections" -n "$requests" "$url" >"$raw" 2>&1; then
            printf '  %-22s c=%-4s FAILED (see %s)\n' "$label" "$connections" "$raw"
            return 0
        fi
        local rps p50 p95 p99
        rps=$(awk '/Requests per second/{print $4}' "$raw")
        p50=$(awk '/^  50%/{print $2}' "$raw")
        p95=$(awk '/^  95%/{print $2}' "$raw")
        p99=$(awk '/^  99%/{print $2}' "$raw")
        printf '  %-22s c=%-4s rps=%-12s p50=%-6sms p95=%-6sms p99=%sms\n' \
            "$label" "$connections" "$rps" "$p50" "$p95" "$p99"
    fi
}

echo "== environment =="
echo "  load generator : $LOADGEN ($LOADGEN_KIND)"
echo "  build dir      : $BUILD_DIR"
echo "  build type     : $(awk -F= '/^CMAKE_BUILD_TYPE:/{print $2}' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)"
echo "  cores          : $(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo '?')"
echo "  duration/warmup: $DURATION / $WARMUP   threads: $THREADS"
echo

"$BACKEND_BIN" --port "$BACKEND_PORT" >"$OUT_DIR/backend.log" 2>&1 &
BACKEND_PID=$!
wait_ready "http://127.0.0.1:$BACKEND_PORT/health" "benchmark backend"

# Explicit configuration so a run is reproducible rather than depending on
# whatever the built-in defaults happen to be. Access logging stays on: it is
# part of what the gateway really costs, and stderr goes to a file.
"$GATEWAY_BIN" \
    --host 127.0.0.1 --port "$GATEWAY_PORT" \
    --backend "users=127.0.0.1:$BACKEND_PORT" \
    --rate-limit off \
    --metrics on \
    --max-retries 1 \
    --health-check-interval-ms 5000 \
    >"$OUT_DIR/gateway.log" 2>&1 &
GATEWAY_PID=$!
wait_ready "http://127.0.0.1:$GATEWAY_PORT/health" "gateway"

if [[ "$SMOKE" == 1 ]]; then
    echo "== smoke =="
    direct=$(curl -fsS --max-time 5 "http://127.0.0.1:$BACKEND_PORT$PATH_UNDER_TEST")
    through=$(curl -fsS --max-time 5 "http://127.0.0.1:$GATEWAY_PORT$PATH_UNDER_TEST")
    echo "  direct  : $direct"
    echo "  gateway : $through"
    [[ "$direct" == "$through" ]] || { echo "  MISMATCH: gateway body differs from backend" >&2; exit 1; }
    curl -fsS -o /dev/null "http://127.0.0.1:$GATEWAY_PORT/metrics"
    echo "  /metrics: ok"
    echo "  smoke OK"
    exit 0
fi

CONN_LIST="$CONNECTIONS"
[[ "$MATRIX" == 1 ]] && CONN_LIST="1 10 50 100"

echo "== results (warm-up discarded) =="
for connections in $CONN_LIST; do
    if [[ "$TARGET" == direct || "$TARGET" == both ]]; then
        run_load "direct-backend" "http://127.0.0.1:$BACKEND_PORT$PATH_UNDER_TEST" "$connections"
    fi
    if [[ "$TARGET" == gateway || "$TARGET" == both ]]; then
        run_load "gateway" "http://127.0.0.1:$GATEWAY_PORT$PATH_UNDER_TEST" "$connections"
    fi
done

echo
echo "raw output in $OUT_DIR"
