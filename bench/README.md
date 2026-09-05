# Benchmarking

Stage 11A infrastructure: a way to **measure** the gateway reproducibly. Nothing
here is part of the gateway library or binary, and no gateway code was changed
for performance.

## What is measured

Two paths, so gateway overhead is the difference between them rather than an
absolute number:

```
baseline   client ─────────────────────► bench-backend
gateway    client ──► api-gateway ─────► bench-backend
```

`bench-backend` is deliberately trivial — one preformatted response, no logging,
no request recording — so that the comparison reflects the gateway rather than
backend work. (The test suite's `TestBackend` is *not* reused: it stores every
request it receives, which would grow without bound and add lock contention
under load.)

## Requirements

- A load generator. **`wrk` is preferred** because it reports a latency
  distribution: `brew install wrk`. If it is missing the runner falls back to
  `ab`, which reports fewer percentiles.
- A **Release** build. Benchmarking the default Debug build measures the
  sanitizable, unoptimised code and is misleading.

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

`bench-backend` is built by default at top level; `-DAPI_GATEWAY_BUILD_BENCH=OFF`
turns it off.

## Running

The runner starts the backend and the gateway, waits for both to answer, runs a
discarded warm-up, then measures — and stops everything it started.

```bash
# validate the harness without benchmarking anything
BUILD_DIR=$PWD/build-release ./bench/run_bench.sh --smoke

# the concurrency matrix: 1, 10, 50 and 100 connections, both paths
BUILD_DIR=$PWD/build-release ./bench/run_bench.sh --matrix --duration 30s

# a single point
BUILD_DIR=$PWD/build-release ./bench/run_bench.sh \
    --target gateway --threads 4 --connections 100 --duration 30s
```

Options: `--duration`, `--warmup`, `--threads`, `--connections`, `--target
direct|gateway|both`, `--matrix`, `--backend-port`, `--gateway-port`,
`--loadgen`, `--out`. Raw load-generator output is written to `bench/results/`.

### Running the pieces by hand

```bash
# terminal 1 — backend
./build-release/bench/bench-backend --port 9101

# terminal 2 — gateway, pointed at it
./build-release/api-gateway --host 127.0.0.1 --port 8080 \
    --backend users=127.0.0.1:9101 \
    --rate-limit off --metrics on --max-retries 1 \
    --health-check-interval-ms 5000

# terminal 3 — load
wrk -t4 -c100 -d30s --latency http://127.0.0.1:9101/users/1   # baseline
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/users/1   # gateway
```

Concurrency sweep (note `wrk` requires connections ≥ threads, so use `-t1` at
`-c1`; the runner clamps this for you):

```bash
wrk -t1 -c1   -d30s --latency http://127.0.0.1:8080/users/1
wrk -t4 -c10  -d30s --latency http://127.0.0.1:8080/users/1
wrk -t4 -c50  -d30s --latency http://127.0.0.1:8080/users/1
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/users/1
```

## Methodology

Choices made to avoid misleading results:

- **Release build.** Stated in the runner's header output for every run.
- **Warm-up is discarded.** A separate load phase (default 5s) runs first and is
  thrown away, so process start-up, page faults and the first backend
  connections are not measured.
- **Runs are long enough.** 30s by default. Shorter runs are dominated by
  scheduling noise; `--duration` exists for quick iteration, not for numbers you
  intend to record.
- **The backend is not the bottleneck.** Always record the direct-backend
  baseline in the same session. If the gateway number approaches the baseline,
  the backend is the limit and the gateway measurement is meaningless.
- **The workload is fixed.** Same path, same method, same response size across
  compared runs. Changing any of them invalidates a comparison.
- **Configuration is explicit.** The runner passes every relevant flag rather
  than relying on defaults, so a run is reproducible from the command alone.
- **Logging is included, not hidden.** The gateway writes one access-log line per
  request; that is part of what it really costs. The runner redirects the
  gateway's stderr to `bench/results/gateway.log`. Running with stderr to
  `/dev/null` versus to a file is a useful comparison to record — but do not
  remove logging to make a number look better.
- **Repeat.** Run the matrix at least three times and record the spread, not a
  single best figure. Numbers from a laptop with other software running are
  indicative only.

Record for each run: requests/sec, average latency, p50, p90, p99 (wrk's
`--latency` reports 50/75/90/99 — **not** p95; `ab` reports p50/p95/p99),
connection count, thread count, build type, and any CPU/thermal observations.

## Recorded baseline

Measured on this machine, not a target and not a claim about the gateway in
general. Reproduce with the matrix command above before relying on any of it.

- Apple Silicon, 8 cores, 8 GB RAM, macOS 25.6
- Release build, Apple Clang 21
- wrk 4.2.0, `-t4` (clamped to `-t1` at `-c1`), 30s measured after 5s warm-up
- gateway: rate limiting off, metrics on, access logging on, one backend
  instance, `--max-retries 1`, health checks every 5s
- single run per row; other software was running on the machine

| Concurrency | Direct backend rps | Gateway rps | Gateway p50 | Gateway p90 | Gateway p99 |
| --- | --- | --- | --- | --- | --- |
| 1 (t=1)   | 35 254 | 2 104 | 459 us | 516 us | 4.08 ms |
| 10 (t=4)  | 107 728 | 11 797 | 613 us | 0.88 ms | 64.67 ms |
| 50 (t=4)  | 104 253 | 10 437 | 0.93 ms | 298.84 ms | 428.30 ms |
| 100 (t=4) | 107 581 | 11 598 | 821 us | 595.86 ms | 809.52 ms |

Direct-backend latency stayed at p99 36-147 us across every row, so the backend
was never the bottleneck in these runs.


### Observations, not conclusions

Things worth investigating in Stage 11B. **None of these has been acted on**, and
none should be treated as a diagnosis until profiled:

- The gateway opens a **new TCP connection to the backend for every request** —
  there is no outbound connection pooling (deliberately out of scope so far).
- Every request writes an **access-log line** to stderr.
- Every request takes a **routing decision, a rate-limit check when enabled,
  round-robin selection, a circuit-breaker acquire, and several metric updates**.

Which of these actually matters is a question for a profiler, not for guesswork.
