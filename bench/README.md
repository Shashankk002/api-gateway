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

## Recorded baseline (Stage 11B)

Measured on this machine. Not a target, not a claim about the gateway in
general, and not a substitute for measuring on your own hardware.

- Apple Silicon, 8 cores, 8 GB RAM, macOS 25.6
- Release build (`-DCMAKE_BUILD_TYPE=Release`), Apple Clang 21
- wrk 4.2.0, `-t4` (clamped to `-t1` at `-c1`), 30 s measured after 5 s discarded warm-up
- gateway: rate limiting off, metrics on, access logging on, one backend
  instance, `--max-retries 1`, health checks every 5 s
- **three full matrix runs**; other software was running on the machine

Requests/sec, three runs:

| Concurrency | Direct backend | Gateway | Gateway / backend |
| --- | --- | --- | --- |
| 1 (t=1)   | 35 667 / 34 030 / 35 568 | 2 024 / 2 009 / 1 956 | ~5.7 % |
| 10 (t=4)  | 106 954 / 104 249 / 104 504 | 12 695 / 12 093 / 12 271 | ~12 % |
| 50 (t=4)  | 104 869 / 106 015 / 98 869 | 12 085 / 11 714 / 11 497 | ~11 % |
| 100 (t=4) | 109 761 / 108 518 / 107 531 | 10 970 / 10 439 / 11 648 | ~10 % |

Gateway latency, three runs:

| Concurrency | p50 | p90 | p99 |
| --- | --- | --- | --- |
| 1   | 496 / 481 / 513 us | 521 / 535 / 539 us | 4.46 / 7.50 / 4.65 ms |
| 10  | 571 / 614 / 611 us | 782 / 805 / 794 us | 139.7 / 32.2 / 25.5 ms |
| 50  | 779 / 777 / 791 us | 258 / 267 / 271 ms | 340 / 484 / 457 ms |
| 100 | 0.96 / 1.00 / 0.83 ms | 653 / 665 / 609 ms | 1.11 s / 936 ms / 779 ms |

Direct-backend latency stayed at p99 34-171 us in every row, so **the backend was
never the bottleneck** in any of these runs.

## Analysis

### Measured facts

1. **Gateway throughput plateaus at 10-12.7k req/s** from c=10 upward, roughly a
   tenth of the direct-backend baseline. Adding connections past ~10 does not
   raise it.
2. **p50 stays under ~1 ms at every concurrency while p99 grows with
   concurrency** (4 ms at c=1 to ~1 s at c=100). That gap is queueing, not slow
   service.
3. **Exactly 8 established gateway to backend connections** during load
   (`netstat` while running at c=100). cpp-httplib's server pool is
   `max(8, hardware_concurrency - 1)` = 8 here, and the gateway process has 10
   threads (8 pool + main + health checker). Backend concurrency is therefore
   capped at 8 synchronous workers.
4. **~16 500 sockets in TIME_WAIT** during load: the proxy opens and closes a
   connection per request.
5. **The gateway is not CPU-bound**: ~26 % of one core out of 800 % available.
   `sample` shows worker leaves in blocking syscalls (`__select`, `__sendto`,
   `__recvfrom`, `__connect`) with only ~15 samples in mutex waits, so there is
   no evidence of significant lock contention.
6. **The cost is in the proxy step, not the server or the pipeline.** At c=1:

   | Path | What it exercises | req/s | p50 |
   | --- | --- | --- | --- |
   | `/health` | middleware pipeline only | 32 250 | 30 us |
   | `/nope` | pipeline + routing (404) | 31 673 | 30 us |
   | `/users/1` | pipeline + routing + proxy | 2 022 | 497 us |

   The pipeline and routing cost ~30 us; **the proxy step adds ~467 us**, ~94 %
   of the serial cost. At c=100 the same split holds: `/health` sustains
   **89 714 req/s** against `/users/1` at **12 733 req/s**.
7. **Metrics and logging are not material.** At c=100, 15 s each:

   | Variant | req/s | p50 | p99 |
   | --- | --- | --- | --- |
   | metrics on, log to file | 12 803 | 787 us | 710 ms |
   | metrics off, log to file | 12 427 | 767 us | 717 ms |
   | metrics on, log to /dev/null | 11 900 | 768 us | 926 ms |
   | metrics off, log to /dev/null | 11 631 | 773 us | 975 ms |

   The spread is inside run-to-run noise (the baseline itself moved 10.4-12.8k).
8. **Not a timeout artifact.** `--backend-timeout-ms` of 100 / 1000 / 5000 gives
   2 005 / 2 009 / 2 016 req/s at c=1 (p50 499 / 498 / 498 us) - a 50x change in
   the setting moves nothing.
9. **The healthy path is clean.** After a 20 s c=100 run:
   `retry_attempts_total 0`, `retried_requests_total 0`,
   `backend_failures_total 0`, `circuit_opens_total 0`,
   `rate_limited_requests_total 0`. Retries, circuits and rate limiting are not
   contaminating the measurement.
10. A fresh TCP connection plus request to the backend, driven by wrk at c=1,
    costs about **61 us** end to end (16 351 req/s). The gateway's proxy step
    costs ~467 us for the same logical work.

### Supported conclusion

The bottleneck is **the outbound proxy step**, amplified by the **8 synchronous
worker threads**. Throughput follows directly: 8 workers divided by ~667 us of
service time is ~12k req/s, which is what is measured. Once all 8 workers are
busy, extra client connections queue, which is exactly the low-p50 / high-p99
signature observed.

The gateway's HTTP server, middleware pipeline, request IDs, routing, metrics
and logging are **not** the bottleneck: together they sustain ~90k req/s.

### Hypotheses, not yet proven

Fact 10 leaves roughly **400 us per request unattributed** inside the outbound
path. Plausible contributors, none of them yet demonstrated:

- a fresh `httplib::Client` is constructed per request;
- name resolution happens per request (`getaddrinfo` appears in the profile,
  though at low sample counts);
- `select_read` / `select_write` before each I/O;
- header multimap and string copies building the outbound request and copying
  the response back.

Do not act on any of these until a finer profile attributes the time.

A client-side `Connection: close` experiment was run and is **not** conclusive:
at c=100 it gave 7 097 req/s / p99 469 ms against 12 733 / p99 686 ms with
keep-alive - lower throughput and a somewhat lower tail, which does not cleanly
separate queueing from connection handling. (An earlier version of this
experiment was invalid because the shell mangled the `-H` argument; the numbers
here are from the corrected run.)

### Recommended next experiment

Attribute the ~400 us inside `ReverseProxy::forward` before changing anything:
either an Instruments/`xctrace` Time Profiler run focused on the proxy path, or
temporary scoped timing around client construction, connect, send, receive and
teardown. Separately, rebuilding with a larger `CPPHTTPLIB_THREAD_POOL_COUNT` is
a build-time experiment that would test the 8-worker cap without changing
gateway behaviour.

### Limitations

- One laptop, loopback only: client, gateway and backend share 8 cores, so
  absolute numbers understate what dedicated hosts would show.
- Three runs per row; the c=10 p99 in particular varied 25-140 ms.
- `wrk` is closed-loop, so its latency figures are subject to coordinated
  omission and understate queueing delay. `wrk2` at a fixed rate would be the
  honest tool for latency specifically.
- wrk reports p50/p75/p90/p99, not p95.
- `Socket errors: read 11` appeared in one 20 s c=100 run (out of 255 995
  requests); not investigated.

### Observations recorded, no action taken

No gateway source was changed in Stage 11A or 11B. Every item above is a
measurement or an explicitly labelled hypothesis.

## Worker-pool experiment (Stage 11C)

Stage 11B left one hypothesis outstanding: that cpp-httplib's 8 worker threads
cap gateway throughput. Stage 11C tested it directly.

### How the worker count is configured

cpp-httplib picks it at compile time:

```c++
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT \
  ((std::max)(8u, std::thread::hardware_concurrency() > 0 ? ... - 1 : 0))
#endif
```

On this 8-core machine that is 8. The `#ifndef` guard makes it overridable, and
the library is header-only, so the value must be identical in every translation
unit that includes it.

`API_GATEWAY_HTTPLIB_THREAD_POOL_COUNT` (CMake cache variable, **empty by
default**) defines the macro `PUBLIC` on `api_gateway_core`. Empty leaves
cpp-httplib's own default in place, so **production behaviour is unchanged**.
`bench_backend` links cpp-httplib directly rather than through the core library,
so it keeps the default pool in every build - only the gateway's pool varies.

```bash
for W in 8 16 32; do
  cmake -S . -B build-w$W -DCMAKE_BUILD_TYPE=Release -DAPI_GATEWAY_BUILD_TESTS=OFF \
        -DAPI_GATEWAY_HTTPLIB_THREAD_POOL_COUNT=$W
  cmake --build build-w$W -j
done
```

Verified before measuring: gateway thread counts were **10 / 18 / 34** (pool +
main + health checker) and `bench-backend` stayed at 9 in every build.

### Results

Same backend, endpoint, machine, Release build, wrk 4.2.0, 30 s measured after
5 s discarded warm-up, `-t4`. Three runs per configuration at c=100.

Requests/sec at c=100:

| Workers | Run 1 | Run 2 | Run 3 | Mean |
| --- | --- | --- | --- | --- |
| 8  | 13 408 | 12 183 | 12 202 | **12 598** |
| 16 | 13 225 | 13 755 | 12 030 | **13 003** |
| 32 | 13 324 | 12 785 | 12 570 | **12 893** |

Latency at c=100 (three runs):

| Workers | p50 | p90 | p99 |
| --- | --- | --- | --- |
| 8  | 765 / 796 / 803 us | 529 / 594 / 579 ms | 677 / 981 / 761 ms |
| 16 | 1.22 / 1.37 / 1.65 ms | 465 / 474 / 537 ms | 921 / 656 / 749 ms |
| 32 | 2.28 / 2.34 / 2.38 ms | 371 / 389 / 388 ms | 562 / 770 / 742 ms |

Other concurrencies (one run each):

| Workers | c=10 rps | c=50 rps | c=50 p90 |
| --- | --- | --- | --- |
| 8  | 12 076 | 12 166 | 256 ms |
| 16 | 11 372 | 12 485 | 193 ms |
| 32 | 10 562 | 11 928 | 98 ms |

### Interpretation: the 8-worker hypothesis is rejected

**Quadrupling the worker pool does not increase throughput.** 12 598 to 12 893
req/s across 8 to 32 workers is ~2 %, well inside the spread of the w8 runs
alone (12 183-13 408). This is case B: the worker pool is **not** the
fundamental limit.

What more workers do change is the *shape* of the latency distribution: p50 rises
roughly in proportion to the worker count (0.78 -> 1.4 -> 2.3 ms) while the tail
falls (c=50 p90: 256 -> 193 -> 98 ms). More requests are in service at once, each
proportionally slower, for the same total rate. That is the signature of a
saturated shared resource downstream, not of worker starvation.

### What the ceiling actually is

A control measurement against the benchmark backend alone, varying only the
connection pattern:

| Client pattern | req/s | p50 | backend CPU |
| --- | --- | --- | --- |
| keep-alive, c=100 | **125 687** | 54 us | 204 % |
| new connection per request, c=100 | **22 315** | 96 us | 69 % |
| new connection per request, c=200 | **22 172** | 96 us | 55 % |

Connection-per-request caps at ~22k req/s against this backend and does not move
with client concurrency. The gateway opens a new backend connection for every
request and reaches ~12.6k, about 56 % of that ceiling. **The dominant constraint
is the connection-per-request pattern, not the worker pool**, which is exactly
why adding workers does not help.

Resource observations under identical load (t4/c100): established
gateway-to-backend connections were **6 at 8 workers and 10 at 32 workers**,
with ~17-19k sockets in TIME_WAIT in both. In-flight connections track
throughput x latency (Little's law: 12.6k x 0.66 ms is about 8), not the pool
size. Mutex-wait samples stayed low in both profiles (10 at w8, 24 at w32
against 94 and 252 `__select` samples), so there is still no evidence of
significant lock contention.

This refines a Stage 11B observation: "exactly 8 established connections" was
read there as evidence of the pool cap. It is equally consistent with Little's
law, and 11C settles it - with 32 workers the count stays around 10.

### Remaining hypotheses

- The ~400 us of per-request proxy cost that Stage 11B could not attribute is
  still unattributed. The 22k control shows connection-per-request is expensive,
  but not how that cost splits between the gateway's client path, the kernel and
  the backend's accept path.
- Why the gateway reaches 12.6k when the same pattern sustains 22k from wrk is
  not established; the gateway is also serving inbound connections on the same
  machine, which the control is not.

### Limitations

- One laptop, loopback only; client, gateway and backend share 8 cores.
- Three runs per configuration at c=100, one run each at c=10 and c=50.
- `%CPU` from `ps` is a decaying average and is indicative only.
- **No worker count is recommended.** The data does not support calling any of
  them optimal, and the default is deliberately unchanged.

## Proxy-path attribution (Stage 11D)

Stage 11C pointed at the gateway's per-request outbound connection behaviour.
Stage 11D asks only: **where does that time go?**

### Method

`bench/proxy-probe` is a benchmark-only executable. It links the gateway library
and times the **real `ReverseProxy::forward`**, then re-creates the same
sequence step by step so phases can be timed separately, and measures the
underlying syscalls independently. 4 000 iterations after 400 warm-up, serial
(no concurrency, so queueing cannot distort the result), Release build.

**No production code was instrumented or changed.** cpp-httplib performs
resolve, connect, send and receive inside one `send()` call, so those cannot be
split from outside the library; they are bounded with independent syscall
measurements instead.

```bash
./build-release/bench/bench-backend --port 9101 &
./build-release/bench/proxy-probe --host 127.0.0.1 --port 9101
./build-release/bench/proxy-probe --port 9101 --phase fresh      # one pattern at a time,
./build-release/bench/proxy-probe --port 9101 --phase keepalive  # to verify reuse via TIME_WAIT
```

### Decomposition

| Phase | p50 | Share |
| --- | --- | --- |
| **`ReverseProxy::forward` (whole outbound step)** | **360.5 us** | 100 % |
| `httplib::Client` construct + configure | 0.1 us | <0.1 % |
| `client.send()` (resolve+connect+send+recv+close) | 356.7 us | ~99 % |
| response copy into the gateway's `Response` | 0.4 us | ~0.1 % |

Independently measured syscall costs, as bounds on what is inside `send()`:

| Operation | p50 |
| --- | --- |
| `getaddrinfo(host, port)` | **0.3 us** |
| `socket()` + `connect()` + `close()` | **33.2 us** |
| `send()` + `select()` + `recv()` on an established socket | 10.8 us |
| `send()` + `recv()` on an established socket | 10.4 us |

So of the ~360 us:

```
gateway-only overhead (11B, /health at c=1)   ~30 us
outbound proxy path                          ~360 us
  within the proxy step:
    client construction                        0.1 us   measured
    address resolution                         0.3 us   measured
    connection setup + teardown               ~33 us    measured (~9 %)
    the request/response exchange itself      ~11 us    measured (~3 %)
    response copying                           0.4 us   measured
    UNATTRIBUTED, inside cpp-httplib's client ~316 us   (~88 %)
```

### Evidence for each component

- **Address resolution is not the cost.** Stage 11B saw `getaddrinfo` in a
  sampled profile and flagged it as a hypothesis. Directly timed it is
  **0.3 us**, under 0.1 % of the request. **Hypothesis rejected.**
- **Connection setup is real but small**: ~33 us, ~9 % of the request.
- **The exchange itself is ~11 us.** A raw socket doing the same request against
  the same backend completes in 10.4 us; wrk at c=1 measures 28 us end to end.
  cpp-httplib's client takes ~360 us for the same logical work - roughly **30x**
  a raw socket exchange.
- **`select()` is not itself expensive**: adding it to a raw exchange costs
  0.4 us (10.4 -> 10.8 us).
- A `sample` profile of the probe (single-threaded, uncontended) puts
  **3 785 of 4 227 samples (~90 %) blocked in `__select`**, with 227 in
  `__connect`, 44 in `__sendto`, 43 in `__recvfrom` and only 8 in allocation.
  The client is **waiting**, not computing.
- **Nagle is not the cause.** cpp-httplib defaults `CPPHTTPLIB_TCP_NODELAY` to
  `false` on both client and server, which made it a strong candidate. Enabling
  `TCP_NODELAY` on the client changed nothing (fresh 373 vs 293 us; reused 329 vs
  322 us - within noise, and no improvement). **Hypothesis rejected.**

### Connection-reuse headroom

Each pattern run in isolation, with TIME_WAIT growth used to confirm that reuse
actually happened over 4 400 requests:

| Pattern | p50 | TIME_WAIT delta |
| --- | --- | --- |
| fresh `httplib::Client` per request (what the gateway does) | 373.2 us | **4 400** |
| one `httplib::Client` reused, keep-alive | 338.9 us | **1** |

Reuse is genuine, and it saves **~34 us of ~373 us, about 9 %** - which matches
the independently measured `socket()+connect()+close()` cost almost exactly.

This is an important correction to what Stage 11C's control might suggest. That
control (126k req/s keep-alive vs 22k req/s connection-per-request, driven by
wrk) showed connection churn dominating **for an efficient client**. For *this*
client it does not: the client's own per-request cost is so much larger that
connection setup is a small fraction of it. **Connection pooling alone would be
expected to recover roughly 9 % here, not a multiple.**

### Measured vs inferred

- **Measured**: every number in the tables above, plus the profile distribution
  and the TIME_WAIT confirmation of reuse.
- **Inferred**: that the ~316 us sits inside cpp-httplib's client
  implementation. This follows from subtraction plus the profile, but it has not
  been localised to a specific function or code path inside the library.

### Remaining uncertainty

**~316 us per request (~88 % of the outbound step) is unattributed.** It is
inside `ClientImpl::send()`, and the profile says the thread is blocked in
`select()` rather than burning CPU - yet a raw socket exchange with the same
backend completes in 10.4 us. Why the library waits ~30x longer for the same
response is **not explained by any hypothesis tested here** (resolution,
connection setup, the send/recv syscalls, select overhead, or Nagle).

Going further needs instrumentation *inside* `httplib.h`, or an A/B against a
different HTTP client, both of which are beyond this stage.

### Implication

No optimization is recommended from this stage, and none was made. What the data
does say is that **connection pooling alone is not the large win it appeared to
be** (~9 % here), and that the next investigation should target the ~316 us
inside the client rather than the connection lifecycle around it.

## Outbound client A/B and final result (Stage 11E-G)

Stage 11D left ~316 us per request unattributed inside cpp-httplib's outbound
client. This stage settled it with one decisive experiment, then decided what -
if anything - to change.

### The A/B

`bench/proxy-probe` gained a **minimal raw HTTP/1.1 client**: a TCP socket, one
request, and a read loop that consumes the complete response (headers plus
`Content-Length` bytes). It is benchmark-only, is never used by the gateway, and
is not a general HTTP implementation. Both clients hit the same backend, on the
same machine, in the same process, serially (c=1), 4 000 iterations after 400
warm-up, Release build.

| Client | p50 | mean | p99 |
| --- | --- | --- | --- |
| **raw client, reused connection** | **27.7 us** | 27.6 us | 34.8 us |
| raw client, httplib-shaped request headers | 29.2 us | 28.5 us | 37.4 us |
| **raw client, fresh connection per request** | **63.6 us** | 63.3 us | 79.1 us |
| cpp-httplib client, fresh per request (what the gateway does) | 293.9 us | 311.4 us | 390.1 us |
| cpp-httplib client, reused, keep-alive | 326.5 us | 324.6 us | 364.6 us |

Two internal consistency checks: the raw client's fresh-minus-reused difference
(63.6 - 27.7 = **35.9 us**) matches the independently measured
`socket()+connect()+close()` cost of **34.1 us**; and sending cpp-httplib's exact
request headers from the raw client changes nothing (29.2 vs 28.3 us).

### What this proves

Of the four candidate explanations:

1. **cpp-httplib is responsible for most of the unexplained latency — SUPPORTED.**
   A minimal client does identical work against the same backend in 27.7 us
   where cpp-httplib takes ~300-330 us. The difference is **~10x**.
2. **Connection setup — rejected.** 34-36 us, consistently measured two ways;
   ~10 % of the request.
3. **Latency independent of cpp-httplib — rejected.** The raw client is fast
   against the same backend.
4. **Benchmark methodology — rejected.** Same process, same harness, same
   backend, same request bytes; and the two independent estimates of connection
   cost agree.

### Where inside the library

A `sample` profile of the serial keep-alive loop attributes the blocked time:

| Call path | Samples | Share |
| --- | --- | --- |
| `process_request` -> **`read_response_line`** -> `SocketStream::read` -> `select_read` -> `__select` | 230 | **~86 %** |
| `process_request` -> `read_content` -> ... -> `__select` | 29 | ~11 % |
| `process_request` -> `write_request` -> `SocketStream::write` | 5 | ~2 % |

So the client sends the request and then blocks in `select()` waiting for the
**first byte of the response** - even though a raw socket receives the complete
response from the same backend in under 30 us. The waiting call is identified;
*why* it waits is not, and localising further would require instrumenting inside
the vendored `httplib.h`.

### Decision: no production optimization was made

This is a deliberate outcome, not an omission.

- **Replacing the outbound client** would address the measured bottleneck and
  the ceiling suggests a large gain, but it means owning an HTTP/1.1 client in a
  gateway: chunked transfer-encoding, keep-alive lifecycle and invalidation,
  timeout semantics, partial reads, error classification feeding the circuit
  breaker, and TLS later. That is a large architectural change with a real
  correctness surface, traded against a benchmark number. Out of proportion.
- **Connection pooling** is the only architecturally small option, and it is
  measured at **~34 us of ~373 us, about 9 %** - with reuse verified by TIME_WAIT
  growth (4 400 connections against 1). Doing it properly still needs a
  per-thread, per-endpoint client cache with invalidation on error and
  interaction with health and circuit state. That is disproportionate complexity
  for 9 %.
- The gateway is **not CPU-bound** (~26 % of one core) and sustains ~12k req/s,
  which is far beyond anything this project requires.

The honest engineering answer is that the dominant cost sits in a third-party
library, the cheap fix buys 9 %, and the expensive fix is not warranted by the
requirement. **The bottleneck is documented rather than papered over.**

### Final benchmark

Same methodology as Stages 11A-11C: Release build, wrk 4.2.0, `-t4` (`-t1` at
c=1), 30 s measured after 5 s discarded warm-up.

| Concurrency | Direct backend req/s | Gateway req/s | Gateway p50 | Gateway p90 | Gateway p99 |
| --- | --- | --- | --- | --- | --- |
| 1   | 34 849 | 2 031 | 489 us | 529 us | 102.12 ms |
| 10  | 103 611 | 12 449 | 598 us | 798 us | 26.01 ms |
| 50  | 109 510 | 11 850 | 786 us | 263.66 ms | 345.85 ms |
| 100 | 108 870 | 11 635 | 796 us | 553.38 ms | 779.76 ms |

Errors: none at c=1, c=10 and c=50. At c=100, wrk reported 12 socket read errors
against the backend and 6 against the gateway, out of roughly 3.3 M and 350 k
requests respectively - under 0.002 %, not investigated further.

**Before/after: unchanged, because nothing was changed.** These figures sit
within the run-to-run spread recorded in Stages 11B and 11C (gateway 10.4-13.8k
req/s at c=100), which is itself a useful result: the measurements are stable
and reproducible across many sessions.

### Engineering conclusion

The gateway sustains roughly **10 % of direct-backend throughput**, and that is
honestly explained rather than excused:

- ~30 us per request is the gateway itself - server, middleware, request id,
  routing, metrics, logging. That part scales to ~90k req/s.
- ~360 us per request is the outbound proxy step, of which ~300 us is inside
  cpp-httplib's client and ~34 us is connection setup.

In other words, **the gateway's own code is not the problem; the HTTP client it
proxies with is.** Fixing that means either accepting a 9 % gain from connection
reuse or taking on an HTTP client implementation. Neither is justified by this
project's requirements today, so the measurement is recorded and the code is
left alone.

If throughput ever becomes a real requirement, the evidence says to start with
the outbound client - and to re-run `bench/proxy-probe` first, because it
already contains the A/B that would prove or disprove any replacement.
