# Benchmarking

Infrastructure for measuring the gateway, and the results of doing so. Nothing
here is part of the gateway library or binary, and **no gateway code was changed
for performance**.

## What is measured

Two paths, so gateway overhead is the difference between them rather than an
absolute number:

```
baseline   client ─────────────────────► bench-backend
gateway    client ──► api-gateway ─────► bench-backend
```

`bench-backend` is deliberately trivial — one preformatted response, no logging,
no request recording — so the comparison reflects the gateway rather than
backend work. The test suite's `TestBackend` is *not* reused: it stores every
request it receives, which would grow without bound and add lock contention
under load.

`proxy-probe` is an attribution tool. It times the real `ReverseProxy::forward`,
re-creates the same sequence step by step so phases can be timed individually,
and compares it against a minimal raw HTTP client.

## Requirements

- **`wrk`** (`brew install wrk`) — preferred, because it reports a latency
  distribution. The runner falls back to `ab`, which reports fewer percentiles.
- **A Release build.** Benchmarking the default Debug build measures
  unoptimised code and is misleading.

## Build and run

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

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
`--loadgen`, `--out`. Raw load-generator output lands in `bench/results/`.

By hand, if you prefer:

```bash
./build-release/bench/bench-backend --port 9101

./build-release/api-gateway --host 127.0.0.1 --port 8080 \
    --backend users=127.0.0.1:9101 \
    --rate-limit off --metrics on --max-retries 1 --health-check-interval-ms 5000

wrk -t4 -c100 -d30s --latency http://127.0.0.1:9101/users/1   # baseline
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/users/1   # gateway
```

`wrk` requires connections ≥ threads, so use `-t1` at `-c1`; the runner clamps
this for you.

## Methodology

Choices made to avoid misleading results:

- **Release build**, stated in the runner's header output for every run.
- **Warm-up is discarded.** A separate load phase (default 5s) runs first and is
  thrown away, so start-up, page faults and first connections are not measured.
- **Runs are long enough.** 30s by default; `--duration` exists for quick
  iteration, not for numbers you intend to record.
- **The backend is not the bottleneck.** Always record the direct-backend
  baseline in the same session. If the gateway number approaches it, the backend
  is the limit and the gateway measurement is meaningless.
- **The workload is fixed** — same path, method and response size across
  compared runs.
- **Configuration is explicit.** The runner passes every relevant flag rather
  than relying on defaults, so a run is reproducible from the command alone.
- **Logging is included, not hidden.** The gateway writes one access-log line
  per request; that is part of what it costs. Do not remove logging to improve a
  number.
- **Repeat.** Run the matrix several times and record the spread, not a single
  best figure.

Record per run: requests/sec, p50, p90, p99, connections, threads, build type.
`wrk --latency` reports 50/75/90/99 — **not p95**; `ab` reports p50/p95/p99.

## Environment

Every number below was measured on: Apple Silicon, 8 cores, 8 GB RAM, macOS
25.6, Release build with Apple Clang 21, wrk 4.2.0, loopback only. Client,
gateway and backend share the same 8 cores, so absolute figures understate what
dedicated hosts would show. These are not targets.

## Results

30s measured after 5s warm-up, `-t4` (`-t1` at c=1).

| Concurrency | Direct backend req/s | Gateway req/s | Gateway p50 | Gateway p90 | Gateway p99 |
| --- | --- | --- | --- | --- | --- |
| 1   | 34 849  | 2 031  | 489 us | 529 us    | 102.12 ms |
| 10  | 103 611 | 12 449 | 598 us | 798 us    | 26.01 ms  |
| 50  | 109 510 | 11 850 | 786 us | 263.66 ms | 345.85 ms |
| 100 | 108 870 | 11 635 | 796 us | 553.38 ms | 779.76 ms |

Errors: none at c=1, c=10 and c=50. At c=100, 12 socket read errors against the
backend and 6 against the gateway, out of roughly 3.3M and 350k requests
(<0.002%); not investigated.

Direct-backend p99 stayed at 34–171 us in every row, so the backend was never
the bottleneck. Across many sessions the gateway measured 10.4–13.8k req/s at
c=100, which is the run-to-run spread to expect.

## Analysis

The gateway sustains roughly 10% of direct-backend throughput. Throughput
plateaus from c=10 upward while p50 stays under 1 ms and p99 grows with
concurrency — the signature of queueing, not slow service.

### It is not the gateway's own code

Measured at c=1, by endpoint:

| Path | Exercises | req/s | p50 |
| --- | --- | --- | --- |
| `/health` | middleware pipeline only | 32 250 | 30 us |
| `/nope` | pipeline + routing (404) | 31 673 | 30 us |
| `/users/1` | pipeline + routing + proxy | 2 022 | 497 us |

The pipeline and routing cost ~30 us; **the proxy step adds ~467 us**. At c=100
the same split holds: `/health` sustains **89 714 req/s** against `/users/1` at
**12 733 req/s**.

Supporting observations: the gateway uses ~26% of one core out of 800%, so it is
not CPU-bound; profiles show worker threads in blocking syscalls with only ~15
samples in mutex waits, so there is no evidence of lock contention; and metrics
and access logging are immaterial (four on/off combinations at c=100 all landed
within run-to-run noise, 11.6–12.8k req/s).

### It is not the worker pool

cpp-httplib picks its pool at compile time, `max(8, hardware_concurrency - 1)` —
8 here. Rebuilding the gateway with 8, 16 and 32 workers (verified by thread
count: 10/18/34) and running three matrices each:

| Workers | Run 1 | Run 2 | Run 3 | Mean |
| --- | --- | --- | --- | --- |
| 8  | 13 408 | 12 183 | 12 202 | **12 598** |
| 16 | 13 225 | 13 755 | 12 030 | **13 003** |
| 32 | 13 324 | 12 785 | 12 570 | **12 893** |

A 4x increase moves throughput ~2%, inside the spread of the w8 runs alone. More
workers only reshape latency: p50 rises roughly in proportion (0.78 → 1.4 → 2.3
ms) while the tail falls (c=50 p90: 256 → 193 → 98 ms). That is a saturated
shared resource downstream, not worker starvation.

To reproduce, no project support is needed:
`-DCMAKE_CXX_FLAGS=-DCPPHTTPLIB_THREAD_POOL_COUNT=32`.

### It is the outbound HTTP client

`proxy-probe`, serial, 4 000 iterations after 400 warm-up:

| Phase | p50 |
| --- | --- |
| **`ReverseProxy::forward` (whole outbound step)** | **360.5 us** |
| `httplib::Client` construct + configure | 0.1 us |
| `client.send()` (resolve+connect+send+recv+close) | 356.7 us |
| response copy into the gateway's `Response` | 0.4 us |

Independently measured syscall costs, as bounds on what is inside `send()`:

| Operation | p50 |
| --- | --- |
| `getaddrinfo(host, port)` | 0.3 us |
| `socket()` + `connect()` + `close()` | 33.2 us |
| `send()` + `select()` + `recv()` on an established socket | 10.8 us |
| `send()` + `recv()` on an established socket | 10.4 us |

And the decisive A/B — same backend, same request, same process:

| Client | p50 |
| --- | --- |
| **raw client, reused connection** | **27.7 us** |
| **raw client, fresh connection per request** | **63.6 us** |
| cpp-httplib, fresh per request (what the gateway does) | 293.9 us |
| cpp-httplib, reused, keep-alive | 326.5 us |

A minimal client does identical work in 27.7 us where cpp-httplib takes
~300–330 us: a **~10x** difference. Two consistency checks: the raw client's
fresh-minus-reused delta (35.9 us) matches the independent
`socket()+connect()+close()` measurement (34.1 us), and sending cpp-httplib's
exact request headers from the raw client changes nothing (29.2 us).

A profile localises the wait: of 269 samples in the client path, **230 (~86%)
block in `read_response_line` → `select_read` → `__select`**, 29 in
`read_content`, 5 in `write_request`. The client sends the request and then
waits for the *first byte* of a response that a raw socket receives in under
30 us.

So the ~360 us breaks down as:

```
client construction        0.1 us   measured
address resolution         0.3 us   measured
connection setup + close  ~33 us    measured  (~9 %)
the exchange itself       ~11 us    measured  (~3 %)
response copying           0.4 us   measured
UNATTRIBUTED, in httplib ~316 us    (~88 %)
```

Rejected along the way, each with a measurement rather than an argument: DNS
(0.3 us), connection setup (~34 us), benchmark methodology (same process, same
bytes, two agreeing estimates), and Nagle (`CPPHTTPLIB_TCP_NODELAY` defaults to
false, but enabling it changed nothing).

### Connection-reuse headroom

Each pattern run in isolation, with TIME_WAIT growth confirming reuse over 4 400
requests:

| Pattern | p50 | TIME_WAIT delta |
| --- | --- | --- |
| fresh `httplib::Client` per request (what the gateway does) | 373.2 us | 4 400 |
| one `httplib::Client` reused, keep-alive | 338.9 us | 1 |

Reuse is genuine and saves **~34 us of ~373 us, about 9%** — matching the
measured connect cost. A separate control shows connection-per-request capping
at ~22k req/s against this backend versus ~126k with keep-alive, but that is the
cost profile of an *efficient* client; for this one the client's own per-request
cost dwarfs connection setup. **Connection pooling alone would recover roughly
9% here, not a multiple.**

## Conclusion: no optimization was made

This is a deliberate outcome.

- **Replacing the outbound client** would address the bottleneck, but means
  owning an HTTP/1.1 client in a gateway: chunked encoding, keep-alive lifecycle,
  timeout semantics, partial reads, error classification feeding the circuit
  breaker, TLS later. A large architectural change with a real correctness
  surface, traded for a benchmark number.
- **Connection pooling** is the only architecturally small option, and it is
  worth ~9%. Doing it properly still needs a per-thread, per-endpoint client
  cache with invalidation on error and interaction with health and circuit
  state — disproportionate for the return.
- The gateway is not CPU-bound and sustains ~12k req/s, far beyond what this
  project requires.

**The gateway's own code is not the problem — it scales to ~90k req/s — the HTTP
client it proxies with is.** That is recorded rather than papered over. If
throughput ever becomes a real requirement, start with the outbound client, and
re-run `proxy-probe` first: it already contains the A/B that would prove or
disprove any replacement.

## Limitations

- One laptop, loopback only, shared cores.
- Three runs per configuration at c=100; fewer at other concurrencies.
- `wrk` is closed-loop, so its latency figures are subject to coordinated
  omission and understate queueing delay. `wrk2` at a fixed rate would be the
  honest tool for latency specifically.
- `%CPU` from `ps` is a decaying average and is indicative only.
- The ~316 us inside cpp-httplib is localised to a call site but not explained;
  going further needs instrumentation inside the vendored `httplib.h`.
