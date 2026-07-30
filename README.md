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
miles from actual trading. Retuning got the same throughput in 1.9GB, but
the deeper fix (re-centring bands on actual activity) is queued next — the
measurement made the design decision for me, which is exactly how I wanted
this project to work.

Latency methodology matters as much as latency numbers. Throughput runs and
latency runs are separate modes that answer different questions; the paced
mode stamps each message with its *intended* arrival time so that stalls are
charged to every message they delay (Gil Tene's coordinated-omission
argument). The numbers published so far were taken on a busy laptop and are
labelled as such — the quiet-machine pass is on the roadmap, and I'd rather
publish honest lower bounds than pretty lies.

## Building and running

CMake ≥ 3.24 and a C++20 compiler. Presets: `debug`, `release`, `asan`,
`tsan`.

```sh
cmake --preset release && cmake --build --preset release
ctest --preset release          # 95 tests
```

Tools (all under `build/release/src/tools/`):

| Tool | Purpose |
|---|---|
| `itch_count` | per-type message counts for a day file |
| `replay` | full reconstruct; `--threads=2` pipeline, `--audit`, `--bitmap` |
| `bench_replay` | throughput mode and paced-latency mode (`--speed`, histograms) |
| `differential` | full-file flat-vs-reference comparison |
| `golden` | dump/check L2 snapshots against the oracle |
| `itch_slice` | cut a first-hour dev slice by timestamp |

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
tests        95 tests: unit, property, differential, stress
```

## Honest limitations

No networking yet (MoldUDP64 replay is in progress), one engine thread per
book universe, no persistence or risk checks, and the only order types are
limit and IOC. Prices stay in ITCH's integer ticks end to end — no floating
point in the book, ever. See the design doc's non-goals list; scope
discipline was a feature of this project, not an accident.
