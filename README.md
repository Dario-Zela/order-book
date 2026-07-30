# order-book — C++ Limit Order Book & Matching Engine

Parses NASDAQ TotalView-ITCH 5.0 binary data, maintains full-depth limit order
books with price-time priority, and matches synthetic order flow. Two-thread
pipeline (feed → engine) over a lock-free SPSC ring buffer.

**Status: under construction.** See [docs/DESIGN.md](docs/DESIGN.md) for the
full design document and milestone plan.

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
