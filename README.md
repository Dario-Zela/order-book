# order-book

A C++20 limit order book and matching engine that parses real NASDAQ
TotalView-ITCH 5.0 data. I built it to answer a simple question honestly:
*can I reconstruct an entire trading day, message for message, and prove the
result is correct without an answer key?*

There's no official "here's what the book looked like" file to check against,
so correctness rests on three independent legs: a deliberately-simple
`std::map` reference implementation that the fast book must agree with on
every level and every FIFO queue; invariants and golden snapshots; and audits
derived from the stream itself. The whole strategy is described in
[docs/DESIGN.md](docs/DESIGN.md), which I wrote before the first line of code.

## What it does

- **Parses ITCH 5.0** from a memory-mapped file — zero copies, zero
  allocation, compile-time visitor dispatch. 22.7M msgs/s on the first cold
  pass over a 13GB day.
- **Reconstructs the full-depth book** for every listed symbol (~8,900 of
  them) with price-time priority, handling the spec's traps: side-less
  Replace messages, non-printable executions, crossed pre-open books, halts,
  cross trades that must *not* touch the book.
- **Matches synthetic order flow** (limit + IOC) through the same book, with
  fills emitted in price-time order.
- **Runs as a two-thread pipeline** — parse/decode on one core, book apply on
  the other, connected by a lock-free SPSC ring whose memory-ordering
  argument is written out in comments rather than waved at.

## The one-day acceptance test

Everything below ran against the pinned sample day `01302020.NASDAQ_ITCH50`
(free from [emi.nasdaq.com](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/),
12.95GB uncompressed), and runs again every night in CI:

- **423,285,709 messages parsed, zero malformed.** Unknown types are counted
  and skipped, never crashed on.
- **The fast book and the reference book agree on everything.** Full-day
  differential across all 8,900 symbols — every price level, every FIFO
  queue, every counter. Both books end the day exactly empty, which is a
  nice conservation proof in itself.
- **Price-time priority is real, and the stream proves it.** When an
  execution hits an order, that order should be at the front of its queue.
  At-price executions: **99.87% at front** (the remainder are halt-reopens
  and similar). Price-improved executions pass only 17% — which is the
  *right* answer, because midpoint prints don't follow displayed-queue
  priority. Finding that split was the point of the audit.
- **1.93M orders live at the intraday peak**, from a pooled arena with zero
  allocations after startup; the id map (robin-hood, backward-shift
  deletion) never rehashed and never probed past 10 slots.

## What real data taught me

The design doc predicted that a dense per-symbol array of price levels would
beat `std::map` because prices cluster near the touch. Real data agreed —
and then punished the naive sizing. My first configuration allocated **31GB
of price-level arrays**, because bands anchor on each side's *first* order,
and pre-open that's often a $0.01 bid on a $300 stock, stranding the band
miles from actual trading. Retuning got the same throughput in 1.9GB, and
the deeper fix — re-centring bands on actual activity — then shipped as the
dominance-gated rebase: +54% full-day throughput on top of the retune. (Its
first version re-anchored onto the junk quotes themselves and thrashed;
the gate that fixed it is documented in the code.) The residual overflow
traffic is a property of real quote ranges, which span more dollars than
any cache-friendly band can cover. The measurement made the design
decisions for me, which is exactly how I wanted this project to work.

Latency methodology matters as much as latency numbers. Throughput runs and
latency runs are separate modes that answer different questions; the paced
mode stamps each message with its *intended* arrival time so that stalls are
charged to every message they delay (Gil Tene's coordinated-omission
argument). The numbers published so far were taken on a busy laptop and are
labelled as such — the quiet-machine pass is on the roadmap, and I'd rather
publish honest lower bounds than pretty lies.

### So what are the latencies?

Paced-mode replay of the real first hour (93.3M messages, 69.2M paced
samples after the market-open arm and 5s warmup discard), event time
compressed 60×, ingress stamped at *intended* arrival — so these tails
include every stall the pipeline caused, plus the 60×-compressed opening
burst, plus whatever else the laptop was doing. Latency = intended ingress
→ book updated. Clock overhead ~8ns/read.

| ns | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| single-thread | 1,311 | 417,791 | 7,077,887 | 34,603,007 | 39,280,411 |
| 2-thread SPSC pipeline | 1,375 | 303,103 | 8,650,751 | 27,262,975 | 32,213,523 |

Read them for what they are: the p50 says the book-apply path is ~1.3µs
under paced load on a loaded laptop; the p90+ says a 60×-compressed open
on shared hardware queues for milliseconds — which the intended-arrival
stamping refuses to hide. The design targets (p50 < 100ns service time,
p99 < 1µs paced) are claims for the quiet-machine, bare-metal pass, not
for this environment; publishing the honest tail now beats curating a
prettier one.

## Building and running

CMake ≥ 3.24 and a C++20 compiler. Presets: `debug`, `release`, `asan`,
`tsan`.

```sh
cmake --preset release && cmake --build --preset release
ctest --preset release          # 116 tests
```

Tools (all under `build/release/src/tools/`):

| Tool | Purpose |
|---|---|
| `itch_count` | per-type message counts for a day file |
| `replay` | full reconstruct; `--threads=2` pipeline, `--shards=N`, `--audit`, `--bitmap` |
| `bench_replay` | throughput mode and paced-latency mode (`--speed`, histograms) |
| `differential` | full-file flat-vs-reference comparison |
| `golden` | dump/check L2 snapshots against the oracle |
| `itch_slice` | cut a first-hour dev slice by timestamp |
| `mold_send` / `mold_recv` | MoldUDP64 dual-feed UDP replay with loss simulation, rewinder, A/B arbitration and gap-fill |

CI runs {gcc, clang} × {ASan/UBSan, release} + TSan on every push, and a
nightly job replays the entire pinned day: exact counts, golden check,
full-day differential, audit threshold.

## Layout

```
src/itch     wire structs, mmap reader, visitor parser
src/core     types, endian, pool, robin-hood id map, clock, histogram
src/book     reference book (the oracle), flat book, level bitmap
src/engine   reconstruct engine, matcher, event pipeline, audit
src/spsc     the ring
src/tools    the CLIs above
fuzz         libFuzzer targets (parser + hostile book driver)
tests        116 tests: unit, property, differential, stress
```

## Networking and scaling (the stretch goals, delivered)

The MoldUDP64 layer streams a day as a dual-feed UDP session with simulated
independent loss, a bounded rewinder serving gap-fill re-requests, and a
receiver that arbitrates both feeds by sequence arithmetic. Acceptance run:
the full 93.3M-message first hour over loopback with 3% loss per feed —
66,714 gaps healed, 2.39M duplicates collapsed, and the reconstructed
volumes match a direct file replay exactly. The first attempt at full rate
collapsed spectacularly (kernel buffer overflow, a 260k re-request storm,
and a rewinder that had scrolled past the gap — refusing correctly); the
fixes that run taught are documented in the arbitrator.

Sharded replay (`--shards=4`: one parse/route thread, four engine threads
over per-shard SPSC rings) reconstructs the entire 423M-message day in
**21 seconds** — the design target of "full day under a minute" with room to
spare, and identical volumes to the single-thread run.

## The experiments, run

Every design decision the doc flagged as "measure, don't assert" got its
measurement. Hardware, per the doc's own rules: **Apple M4 Pro, 48GB,
macOS 26.5.2, Apple clang 21.0.0, `-O3 -march=native`, mains power,
concurrent background load present and varying** — interleaved A/B where
comparison matters (which cancels drift); absolute values are lower
bounds, relative ones are trustworthy.

| Experiment | Result | Verdict |
|---|---|---|
| `std::map` book vs flat book (same 93.3M-msg input) | 3.80 vs **7.69M msgs/s** | flat wins 2.02×; cache-miss attribution awaits the Linux perf run |
| Order at 64B `alignas(64)` vs naturally-packed 40B | medians 7.13 vs 7.09M msgs/s | **a wash** — workload-dependent, exactly as the doc suspected; 64B stays for layout predictability |
| Hot/cold Order field splitting | — | **declined on evidence**: if 40-vs-64B doesn't move, field splitting has no headroom |
| SPSC cached indices on vs off | 26.9 vs 22.5M items/s | **+19%** for the cached-index optimisation |
| Best-price: linear scan vs two-level bitmap (paced, 60×) | p50 −8%, p90 −14% for bitmap | mid-percentiles favour the bitmap; the designed p99.9 story is **unresolved** under host load — quiet-machine pass pending |
| Band rebase off vs on (full day) | 3.84 vs **5.90M msgs/s** | +54%; junk-anchor failure fixed, residual overflow is a data property |
| Id map vs `std::unordered_map` (churned-sequential keys) | find 11.8 vs 12.6ns; churn 54.6 vs 321ns | finds are a wash; **churn is 5.9×** — and churn is what a trading day is made of |
| Match mode submit cost | ~11ns rest+fill; ~511ns 5-level IOC sweep | microbench conditions (hot caches); labelled |

## Honest limitations

No persistence or risk checks, and the only order types are limit and IOC.
Prices stay in ITCH's integer ticks end to end — no floating point in the
book, ever. Huge-page backing exists (with an OB_NO_HUGEPAGES A/B switch
and a dispatchable CI job measuring the dTLB delta as mechanism evidence)
but its effect is unclaimed until the canonical Linux perf run. The
quiet-machine benchmark pass and bare-metal Linux numbers remain the two
open items — everything else in the design doc's roadmap, stretch goals
included, has shipped: MoldUDP64 dual-feed replay, sharding, snapshot +
restore, the band rebase, and the full experiments table above. Scope
discipline was a feature of this project, not an accident.
