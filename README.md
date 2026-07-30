# order-book — C++ Limit Order Book & Matching Engine

Parses NASDAQ TotalView-ITCH 5.0 binary data, maintains full-depth limit order
books with price-time priority, and matches synthetic order flow. Two-thread
pipeline (feed → engine) over a lock-free SPSC ring buffer.

See [docs/DESIGN.md](docs/DESIGN.md) for the full design document and
milestone plan.

## Status

| Milestone | State |
|---|---|
| ITCH 5.0 parser (S,R,H,A,F,E,C,X,D,U,P,Q), mmap reader, `itch_count` | ✅ |
| Reference `std::map` book + reconstruct engine (§4.1 subtleties tested) | ✅ |
| Flat-array book: adaptive bands, shared arena, robin-hood id map with backward-shift deletion | ✅ |
| SPSC ring (cached indices, batch pop) + two-thread pipeline + `replay` tool | ✅ |
| Match mode: limit/IOC, price-time priority, property tests | ✅ |
| Real sample-day: counts, full-day differential, goldens, audit | ✅ 01302020 |
| Bench harness: paced replay (coordinated omission addressed), HDR histogram, clock shim | ✅ |
| Bitmap best-price experiment (correctness + first A/B) | ✅ |
| Fuzz targets (libFuzzer on Linux; standalone ASan driver locally) | ✅ smoke |
| Nightly CI: full-day differential + goldens + counts + audit | ✅ workflow |
| Band re-centring rebase (see finding below), publishable quiet-machine numbers, Linux/x86 canonical run, design-decision chapters, v1.0 | ⏳ |

All correctness work runs under ASan/UBSan and TSan; the SPSC ring's
memory-ordering argument is written out in `spsc_ring.hpp`, with the TSan
stress test as supporting evidence.

## Pinned sample day

`01302020.NASDAQ_ITCH50.gz` (5,597,158,940 bytes gz, 12,952,050,754 raw) from
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. The server's `.md5sum` link is
stale (404); integrity relies on the gzip CRC. Full-day reference counts
(also asserted nightly in CI): **423,285,709 messages, 0 malformed** —
A 184,735,355 · D 180,285,101 · U 36,777,372 · E 8,415,610 · X 4,990,972 ·
F 1,875,350 · P 1,779,727 · C 139,474 · Q 17,835 · H 8,921 · R 8,916 · S 6 ·
unknown types (I,L,Y,J,K,V) 4,251,070 counted-and-skipped.

## Real-day results so far

**Machine caveat (§8): all numbers below were taken on an M-series laptop
with a concurrent ~3-core workload running — treat them as lower bounds and
shape-checks, not publishable measurements. A quiet-machine, multi-run,
median±spread pass is still to come.**

- Parse-only (`itch_count`, cold single pass): **22.7M msgs/s** — §10 target
  (>20M warm) already exceeded cold.
- Full-day differential (flat vs `std::map` oracle, 423.3M msgs, 8,900
  books): **PASS**, including the observation that both books end the day
  exactly empty. First-hour differential additionally compares every level's
  FIFO: **PASS** (8,899 books, 723,605 levels, 1.7M live orders at 10:30).
- Golden snapshots (first hour, top-20 symbols, 10 levels): oracle-dumped,
  flat-book-verified, committed under `tests/goldens/`.
- Execution-at-front audit (§9.3), full day: **98.53%** of 8.44M audited
  executions hit the FIFO front. Split by type this becomes an explanation,
  not a blemish: **E (at-price) 99.87%** — price-time priority holds — while
  C (price-improved) passes only 17.3%, as expected: midpoint/improved
  prints do not follow displayed-queue priority.
- Reconstruct throughput, full day: 3.8M msgs/s single-thread, 4.8M msgs/s
  two-thread pipeline (ring occupancy pegged: consumer-bound). Arena
  high-water 1.93M live orders (123MB, zero growths); id map max probe 10,
  zero rehashes.
- Paced-latency harness runs end to end (60×, first hour, 69M samples):
  p50 ~2µs but ms-scale tails — dominated by host load plus 60× compression
  of the opening burst; labelled first-cut, not publishable.

**Finding — band sizing (§5.1 arithmetic, confirmed on real data):** the
design-doc default (±2048 ticks, 2^17 cap) produced **31GB of level arrays
and 35% out-of-band ops** — real books carry far-out resting quotes all day,
and bands anchor on the FIRST add per side, which pre-open is often a junk
quote ($0.01 bid on a $300 stock) stranding the band far from real trading.
Retuned defaults (512/8192) run at ~98% of best measured throughput on
1/16th the memory (1.9GB full-day), but out-of-band remains ~50-80% of ops:
the real fix is the §5.1 activity-centred rebase, now justified by data and
next in line. Correctness is unaffected either way (differential-verified in
both configs; overflow ops are just slower).

## Goals

- Parse real ITCH 5.0 data at tens of millions of messages/sec (zero-copy mmap
  cursor, no allocation in the hot path).
- Full-depth books with price-time priority, in two modes:
  - **Reconstruct** — replay exchange events verbatim (the exchange already
    matched; we mirror). Zero matching logic in this mode.
  - **Match** — accept synthetic limit/IOC orders, walk the opposite side,
    emit fills.
- Honest p50/p99/p99.9 latency and throughput numbers with stated methodology
  (coordinated omission addressed by name — see DESIGN §8).

## Non-goals (v1)

- Networking (UDP multicast replay is a post-v1.0 stretch item).
- Multi-symbol sharding across threads, persistence, risk checks,
  cancel-on-disconnect.
- Auction cross price-discovery (cross events are replayed faithfully, not
  simulated).
- Self-trade prevention; order types beyond limit/IOC.

## Data

Sample day files are free from <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>.
Place the uncompressed `*.NASDAQ_ITCH50` file under `data/` (git-ignored;
a full day is O(10 GB)). The pinned reference day for reproducible numbers
will be recorded here once the first full-day replay lands.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler (developed against Apple clang 21
and GCC/Clang on Linux CI).

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan`, `tsan`.

## Layout

```
src/
├── itch/          # protocol: message structs, parser, mmap reader
├── core/          # Price, OrderId, Side types; arena; intrusive list
├── book/          # OrderBook, PriceLevel, Order, BookListener interface
├── engine/        # Engine: symbol table, dispatch, matching logic
├── spsc/          # SpscRing<T,N>
├── bench/         # google-benchmark micro + end-to-end replay harness
├── tools/         # replay CLI, message-count CLI, book dump
└── tests/         # unit, golden-file, fuzz targets
```
