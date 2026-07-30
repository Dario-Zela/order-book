# Design Doc — C++ Limit Order Book & Matching Engine
*Project 1 of the portfolio · target: weeks 1–6 · C++20*

## 1. Goals & non-goals

**Goals**
- Parse real NASDAQ TotalView-ITCH 5.0 binary data at tens of millions of messages/sec.
- Maintain full-depth limit order books with price-time priority; support both *reconstruction* (replay exchange events) and *matching* (accept synthetic orders, produce fills).
- Two-thread pipeline (feed → engine) over a lock-free SPSC queue.
- Publish honest p50/p99/p99.9 latency and throughput numbers with methodology.

**Non-goals (state in README)**
- Networking in v1 (UDP multicast replay is the stretch phase, not core).
- Multi-symbol sharding across threads, persistence, risk checks, cancel-on-disconnect.
- Simulating auction crosses in match mode (we *replay* cross events faithfully; we do not implement opening/closing cross price-discovery logic).
- Self-trade prevention, order types beyond limit/IOC (no pegs, icebergs, stop orders).
- Fixed-point maths beyond what ITCH gives (prices arrive as integers — keep them that way).

## 2. Architecture overview

```
            Thread A (producer)                    Thread B (consumer)
 ┌────────┐   ┌──────────────┐   ┌───────────┐   ┌──────────────────┐   ┌────────────┐
 │ mmap'd │ → │ itch::Parser │ → │ SPSC ring │ → │ Engine (dispatch)│ → │ Listeners  │
 │ file   │   │ (zero-copy)  │   │ (64B slots)│  │  per-symbol Book │   │ stats, L1  │
 └────────┘   └──────────────┘   └───────────┘   └──────────────────┘   │ tape, bench│
                                                                        └────────────┘
```

Single-threaded mode (parser calls engine directly) is kept as a build/runtime option — it is the baseline that *justifies* the SPSC design with numbers, and simplifies debugging.

## 3. Module layout

```
src/
├── itch/          # protocol: message structs, parser, mmap reader
├── core/          # Price, OrderId, Side, Tick types; arena; intrusive list
├── book/          # OrderBook, PriceLevel, Order, BookListener interface
├── engine/        # Engine: symbol table, dispatch, matching logic
├── spsc/          # SpscRing<T,N>
├── bench/         # google-benchmark micro + end-to-end replay harness
├── tools/         # replay_main.cpp, book_dump.cpp (debug CLI)
└── tests/         # unit, golden-file, fuzz targets
```

Dependencies: none in the hot path. Google Benchmark + Catch2/GoogleTest as dev deps (FetchContent). Build: CMake presets (`debug`, `release`, `asan`, `tsan`).

## 4. ITCH parser design

- **Input:** mmap the raw ITCH file; messages are length-prefixed (2-byte big-endian) in the file dump format. Parser is a cursor over the mapping — zero copies, no allocation. `madvise(MADV_SEQUENTIAL)` the mapping; a full sample day is O(10 GB) uncompressed, hundreds of millions of messages (free from emi.nasdaq.com; pin one day's filename in the README for reproducibility).
- **Message subset (v1):** System Event (S), Stock Directory (R), Stock Trading Action (H), Add Order (A), Add Order MPID (F), Order Executed (E), Executed w/ Price (C), Order Cancel (X), Order Delete (D), Order Replace (U), Trade (P), Cross Trade (Q). Everything else: counted and skipped, with per-type counters cross-checked against the sample day's published totals — the weekend-1 acceptance test.
- **Common header:** every ITCH 5.0 message starts with type (1B), **stock locate** (2B), tracking number (2B), timestamp (6B, nanoseconds since midnight). The 48-bit timestamp needs its own byteswap helper. Keep event time around — it enables event-time-paced replay (§8) and "time of day vs latency" plots.
- **Symbol dispatch via stock locate:** the uint16 stock locate code (announced in Stock Directory) indexes a flat `std::array<Book*, 65536>` — O(1) symbol dispatch, no hashing, no string compares: the exchange hands you a perfect hash; use it.
- **Decoding:** `std::memcpy` into packed structs then byte-swap (`std::byteswap`, C++23-polyfill if needed) — memcpy is the defined-behaviour way to type-pun; note this in code comments as it's an interview talking point (strict aliasing).
- **API:** `parser.next(visitor)` with a compile-time visitor (CRTP or `if constexpr` on message type) — no virtual calls in the hot path.
- **Endianness/price format:** ITCH prices are big-endian uint32 in 1/10,000 USD (Cross Trade shares are 8 bytes, unlike everything else — easy to get wrong). Keep as `Price = uint32_t` ticks end to end; render to decimal only at the edges.

### 4.1 Protocol subtleties (document each in code + README)
- **Replace (U) inherits from the original:** U carries only {original ref, new ref, new total shares, new price} — **no side field**. Side must be looked up from the original order before it is deleted; symbol comes from the header's stock locate but assert it matches the original. Shares is the *new total*, not a delta. New ref, new time priority — even a replace to the same price goes to the back of the queue.
- **Executed w/ Price (C):** carries a `printable` flag. Non-printable ('N') executions still decrement the book but must not be counted in trade-volume stats. Execution price may differ from the resting display price (price improvement) — book removal is keyed by order ref, not price.
- **Trade (P):** execution against a *non-displayed* order — no book mutation at all, volume stats only. Easy to get wrong by "helpfully" decrementing something.
- **Cross Trade (Q):** opening/closing/halt/IPO cross volume print. No direct book mutation — displayed orders that participate in the cross get their own E/C messages. Just count volume by cross type.
- **Crossed/locked books are normal pre-open:** before the opening cross, `best_bid ≥ best_ask` happens legitimately (no continuous matching yet). Any "book not crossed" invariant must be gated on System Event 'Q' (start of market hours) / market state — asserting it unconditionally fails on real data within seconds.
- **Halts (H):** track per-symbol trading state (trading/halted/paused). Reconstruct mode applies events regardless (the feed is the truth); match mode rejects new synthetic orders on halted symbols.
- **Odd lots** appear as ordinary orders in ITCH and can set your derived L1 on levels the official round-lot BBO ignores — note this when comparing an L1 tape against any external quote source.
- **Unknown order refs** (Execute/Cancel/Delete/Replace for an id we never saw — possible when starting mid-stream, and guaranteed under fuzzing): count-and-skip, never crash, never allocate. Policy stated once, tested explicitly.

## 5. Book data structures — the core design discussion

This section is the interview; document every choice and its alternative.

### 5.1 Price levels: flat array, not `std::map`
- Per symbol, prices cluster tightly around the touch. Allocate a **dense vector of PriceLevel indexed by tick offset** from a per-symbol base price, giving O(1) level lookup vs `std::map`'s pointer-chasing O(log n) — and, more importantly, predictable memory access: neighbouring levels share cache lines, and the touch region stays resident in L1/L2.
- **Band sizing is a real constraint, do the arithmetic in the README:** ±2¹⁶ ticks × ~24 B/level ≈ 3 MB/symbol; across ~8–9k listed symbols that's >20 GB — untenable. Size bands adaptively instead: start small (e.g. ±4k ticks, ~200 KB/symbol), grow geometrically on first out-of-band hit; lazily allocate the band on a symbol's first Add. Most symbols never trade; total resident memory becomes proportional to *active* symbols. Report the measured footprint.
- Honest counter-argument: most levels are empty, so the cache win comes from the touched region, not the array per se — hence the experiment below matters more than the theory.
- Out-of-band prices (opening prints, fat fingers): overflow `std::map<Price, PriceLevel>` fallback; count hits — expect ≪0.1%.
- Rebasing: if the touch drifts near the band edge, re-centre the array (rare, measured, amortised — copy live levels only, fix up `Order::level` backpointers or use indices instead of pointers to avoid the fix-up entirely; decide and document).
- README experiment: same replay with `std::map` book vs flat book — the before/after table, including `perf stat` cache-miss counters, not just wall time.

### 5.2 Orders: intrusive FIFO per level, pooled
```cpp
struct Order {            // target: exactly 64 bytes, alignas(64)
    OrderId  id;          // 8  — ITCH order reference number
    Price    price;       // 4
    Qty      remaining;   // 4
    Side     side;        // 1
    // padding/flags      // ...
    Order*   next;        // 8   intrusive doubly-linked FIFO
    Order*   prev;        // 8
    PriceLevel* level;    // 8   backpointer for O(1) cancel
};
```
- **Arena + free-list allocator**: pre-allocate a slab of Orders at startup; `alloc()` pops free-list, `free()` pushes. Zero `new`/`delete` in the hot path. Measure: allocation cost disappears from the profile. Size the arena for **peak concurrent live orders** (low single-digit millions across all symbols — measure and publish the high-water mark), not total adds for the day; the free-list recycles. LIFO reuse is also cache-friendly (hottest recently-freed slots handed out first).
- **Order lookup** (Execute/Cancel/Delete/Replace arrive keyed by order ref): open-addressing hash map `OrderId → Order*` (robin-hood probing, power-of-two capacity, ~0.5 load factor), sized at init from the measured high-water mark with headroom — **no rehash in the hot path**, assert if load factor is breached. ITCH order refs are unique for the day, so no reuse ambiguity.
- **Deletion strategy is the interview question hiding in here:** deletes are as frequent as inserts (every Delete, every fully-executed order, every Replace). Naive tombstones degrade probe lengths over a full-day replay; use **backward-shift deletion** and show the probe-length histogram before/after. Justify vs `std::unordered_map` (per-node allocations, pointer-chasing on probe, iterator-stability guarantees you don't need) with a micro-benchmark; optionally add an `absl::flat_hash_map` column as dev-dependency comparison.
- README experiment: `Order` at exactly 64 B `alignas(64)` vs naturally-packed ~48 B (orders sometimes straddling a line) — measure, don't assert; the answer is workload-dependent, and saying so is the credible move.

### 5.3 PriceLevel
```cpp
struct PriceLevel {
    Qty    total_qty;     // maintained incrementally
    u32    order_count;
    Order* head;          // FIFO front = oldest = first to fill
    Order* tail;
};
```
Best-bid/best-ask tracked as cached tick indices, repaired by scanning toward the centre on level-empty (bounded, measured scan — document why this beats a heap here: scans are short because emptied levels are near the touch, and a heap's pointer-hopping plus lazy-deletion bookkeeping costs more than a short linear scan through contiguous memory).

**Planned experiment — bitmap best-price tracking:** keep a summary bitmap over the band (one bit per level, set = non-empty), find the next non-empty level with `std::countr_zero`/`countl_zero` over 64-bit words (two-level bitmap if the band is large). O(1)-ish worst case vs the linear scan's rare-but-unbounded tail — exactly a p99.9 story. An afternoon to build; compare scan-length distributions, and it demonstrates bit-manipulation fluency these interviews reward.

Edge cases to handle explicitly (and unit-test): partial fill leaves the order at the **head** (time priority retained); fill-to-zero unlinks and frees; cancel reducing to zero behaves as delete; last order removed empties the level and may trigger best-price repair; replace onto the same price still re-queues at the tail.

## 6. Engine modes

**Reconstruct mode (primary, runs on real data):** apply ITCH events verbatim — Add creates order, Execute/Cancel decrement, Delete unlinks, Replace = delete + add with new ref and new time priority, inheriting side from the original (§4.1). The exchange already matched; we mirror — reconstruct mode must contain **zero matching logic**, or crossed pre-open books (§4.1) will silently corrupt state. Correctness oracle: differential testing against the reference book plus stream-derived audits (§9).

**Match mode (synthetic flow):** `submit(NewOrder | Cancel | Replace)` API. Incoming marketable orders walk opposite-side levels from the touch, filling FIFO within each level (price-time priority), emitting `Fill` events; remainder rests. Supports limit + IOC in v1 (market = limit at far band edge, which naturally bounds the level walk to the band). Partial fills keep the resting order at the head of its level. Rejects orders on halted symbols. This mode is what the Go gateway (later project) and the backtest case-study drive.

Both modes emit through a `BookListener` interface (compile-time template parameter, not virtual): `on_add / on_fill / on_cancel / on_level_change`. Listeners in v1: L1 tape writer, stats counter, latency recorder, null (for pure-throughput runs).

## 7. SPSC ring buffer

```cpp
template <typename T, size_t N>  // N power of two; T trivially copyable, ≤ cache line
class SpscRing {
    alignas(64) std::atomic<size_t> head_;   // consumer-owned
    alignas(64) std::atomic<size_t> tail_;   // producer-owned
    alignas(64) std::array<T, N> buf_;
};
```
- `T` here is a normalized `BookEvent` struct (decoded, host-endian, ≤ 64 B — `static_assert` both size and `std::is_trivially_copyable_v`), not raw ITCH bytes: decode cost stays on the producer core.
- Indices are free-running `size_t` counters masked with `& (N-1)` on access — no modulo, no wrap branch; document why 64-bit wrap-around is a non-issue at any achievable rate.
- Producer: `tail_.load(relaxed)` (own index) + `head_.load(acquire)` for space check → store slot → `tail_.store(release)`.
- Consumer: mirror with roles swapped. Document *why* each ordering suffices (release publishes the slot write; acquire on the other side observes it), why `relaxed` is fine for your own index, and why seq_cst would be wasted money — this is a core interview answer. Bonus: on x86 (TSO) release/acquire compiles to plain MOVs (the win over seq_cst is an eliminated `xchg`/`mfence`); on ARM, `stlr`/`ldar` — show the Godbolt codegen in the README.
- **Cached-index optimisation**: producer caches last-seen head (re-reads only when apparently full), consumer mirrors — cuts cross-core cache-line ping-pong; include the before/after benchmark.
- Batch pop (`pop_n`) for the consumer to amortise atomic ops.
- Backpressure policy: spin with `_mm_pause()`/`__builtin_ia32_pause` (or `std::this_thread::yield` fallback); ring sized so replay rarely blocks in practice (measure occupancy high-water mark and publish it — it also tells you which thread is the bottleneck).
- Verification: TSan-instrumented stress test (randomised producer/consumer stalls, millions of ops, checksum of payload sequence) in CI. TSan silence is evidence, not proof — hence the written ordering argument above.

## 8. Threading & measurement methodology

- Thread A: parse + timestamp ingress; Thread B: book apply + timestamp egress. Pin threads to distinct physical cores where the OS allows (macOS: QoS hints + document the limitation honestly, and note Apple Silicon P- vs E-core scheduling as a confounder; Linux CI box: `pthread_setaffinity_np`).
- **Clocks:** `rdtsc` on x86 with measured frequency (check `constant_tsc`/`nonstop_tsc` in cpuinfo; know when `rdtscp`/`lfence` serialisation matters and say why plain `rdtsc` is acceptable for inter-thread interval stamps); on Apple Silicon use `mach_absolute_time`/`cntvct_el0`. Wrap in a `now()` shim; document per-platform, and measure the cost of `now()` itself — it bounds what you can claim.
- **Latency definition:** ingress (message available post-parse) → egress (book updated, listeners notified). Report p50/p90/p99/p99.9/max from an HDR-style fixed-bucket histogram (no allocation, log-spaced buckets).
- **Coordinated omission — address it by name.** Replaying as-fast-as-possible saturates the pipeline, so "latency" becomes queue depth in disguise; and any measurement scheme where a slow op delays the issuing of subsequent ops under-samples the bad tail. Split the claims: (a) **throughput runs** at full speed measure msgs/s only; (b) **latency runs** pace the replay by ITCH event timestamps (scaled if needed) and stamp ingress at the *intended* arrival time, so stalls are charged to every message they delay. Cite Gil Tene's "How NOT to Measure Latency". Publish both, labelled — worth more in interviews than either number alone.
- **Cold vs warm:** first pass over the mmap is page-fault/IO bound and not a parser benchmark. Report warm-cache numbers, state that, and optionally show the cold pass separately.
- **Microbenchmark hygiene:** `benchmark::DoNotOptimize`/`ClobberMemory` so the optimiser can't delete the work; feed realistic data distributions (replay-derived operation mixes, not uniform-random keys — hash-map and best-price numbers change materially with realism).
- **Machine hygiene:** performance governor, turbo noted (or disabled), laptop thermals acknowledged (M-series laptops throttle mid-run — run mains-powered, interleave A/B variants to cancel drift). Record `perf stat` counters (IPC, cache-misses, branch-misses) alongside wall time on the Linux box — attributing a win to a mechanism beats asserting it.
- **Discipline:** warmup discard (first 10%), ≥5 runs, report medians across runs with min/max spread, note hardware/OS/compiler flags (`-O3 -march=native`, LTO on/off) in the README table. Never publish numbers from a shared/virtualised box.

## 9. Correctness strategy

There is no external ground truth shipped with the sample data (no official per-message book snapshots), so correctness rests on three independent legs: a second implementation, spec-derived invariants, and audits derived from the stream itself. Say this plainly in the README — knowing where your oracle comes from is itself a signal.

1. **Golden-file tests:** replay N messages of the sample day, snapshot book state (per-symbol L2 up to 10 levels) at fixed message counts, compare against committed goldens. Goldens bootstrapped once with the simple `std::map` reference book (two independent implementations must agree — keep the reference book forever as the differential-testing oracle, and run the full-day differential comparison as a slow nightly/local job, not just short prefixes).
2. **Invariant checks (debug builds):** best_bid < best_ask **only after market open** (System Event 'Q' — pre-open books cross legitimately, §4.1); level `total_qty` == Σ member orders (sampled); order count consistency; no order in book absent from the id map and vice versa; every arena slot is exactly one of {live-in-book, free-listed}.
3. **Stream-derived audit — execution-at-front:** in reconstruct mode, when an Execute (E/C) hits order X during continuous trading, price-time priority implies X should be at the front of its price level. Run this as a counted audit, not a hard assert (crosses, halt-reopens and mid-stream start create legitimate exceptions); a high pass rate with explained exceptions is strong evidence the FIFO ordering is right — and a genuinely interesting README section.
4. **Parser fuzzing:** libFuzzer target over the message decoder, corpus seeded from real sample-day slices; ASan/UBSan in CI. Add a second structured-fuzz target that drives the *book* with syntactically-valid-but-hostile sequences (unknown refs, double deletes, cancel > remaining — count-and-skip policy per §4.1, never UB).
5. **Match-mode property tests:** random order streams → fills never exceed order qty, price-time priority never violated (verify FIFO by construction check), conservation (qty in = filled + resting + cancelled), and no resting order ever crosses the opposite touch post-match.
6. **SPSC stress test** under TSan per §7.
7. CI: GitHub Actions — {gcc, clang} × {Debug+sanitizers, Release}, tests + benchmark smoke (no perf assertions in CI; numbers come from dedicated local runs).

## 10. Benchmark targets (order-of-magnitude, to validate against)

| Metric | Target |
|---|---|
| Parse throughput (null listener, warm cache) | > 20M msgs/s |
| End-to-end apply throughput | 5–15M msgs/s |
| Full sample day replayed end to end | < 1 min wall clock |
| Book op latency p50 (service time) | < 100 ns |
| p99 (event-time-paced run) | < 1 µs |
| Id-map find hit (replay-realistic keys) | < 30 ns |
| SPSC round-trip overhead | < 100 ns |

Sanity-check targets from first principles in the README (e.g. 20M msgs/s × ~40 B/msg ≈ 800 MB/s streamed — comfortably under DRAM bandwidth, so the parser should be compute-bound; if it isn't, that's a finding). Every latency number states which run type produced it (§8). If results miss targets, the *investigation* becomes README content — a profiling chapter beats a lucky number.

## 11. Milestones

| Weekend | Deliverable |
|---|---|
| 1 | CMake skeleton, CI, mmap reader, parser for full v1 subset (incl. H/Q/P), per-type message counts vs sample-day totals |
| 2 | Reference `std::map` book + reconstruct mode (Replace inheritance, C printable flag, market-state gating) + golden tests green |
| 3 | Flat-array book + arena + intrusive lists + id map (backward-shift deletion); full-day differential test vs reference; first perf numbers |
| 4 | SPSC ring + TSan stress test + two-thread pipeline + latency histograms (throughput and event-time-paced runs); single- vs dual-thread comparison |
| 5 | Match mode + property tests; execution-at-front audit; bitmap best-price experiment; profiling pass (perf/Instruments) |
| 6 | README: architecture, design-decision sections with before/after tables, results, limitations. Tag v1.0 |

Weekends 3 and 5 are the overrun risks; the pre-agreed cuts are (in order): bitmap experiment → execution-at-front audit → the 64 B-vs-packed Order experiment. Everything else is core.

### Stretch (post-v1.0, honest estimates)
| Item | Est. effort | Notes |
|---|---|---|
| UDP multicast replay: MoldUDP64 framing, A/B feed arbitration, gap-fill request stub | 2 weekends | Feeds the Go gateway project; sequence-number handling is the interesting part |
| Multi-symbol sharding across engine threads | 1–2 weekends | Symbol→shard partitioning; only worth it with the measurement story to match |
| Huge pages for arena + id map (`madvise(MADV_HUGEPAGE)`), Linux only | ~½ weekend | Cheap; measure dTLB-miss delta with `perf stat` — small win, great methodology demo |
| Book snapshot + restore (start replay mid-day) | 1 weekend | Also unlocks faster golden-test iteration |
| Order struct hot/cold field splitting | ~½ weekend | Only if profiling shows the cold fields actually cost |

## 12. Risks

- **Scope creep into an exchange simulator** → non-goals list is contractual; stretch items live behind the v1.0 tag.
- **Apple Silicon vs x86 numbers** → publish both if possible (borrow/CI a Linux x86 box for one canonical run); always label the hardware.
- **ITCH spec fiddliness** (Replace inheritance, printable flags, crossed pre-open books, halts) → the §4.1 checklist exists precisely so these are handled deliberately, not discovered in a debugger; count-and-skip unknowns loudly; golden tests catch semantic drift.
- **No external oracle for book state** → mitigated by the three-legged strategy in §9; state the limitation in the README rather than implying validation that doesn't exist.
- **Full-day runs are slow during development** → develop against a committed first-hour slice; full-day differential + audit runs are nightly/local, not inner-loop.

## 13. References
- NASDAQ TotalView-ITCH 5.0 specification + MoldUDP64 / SoupBinTCP specs (nasdaqtrader.com); sample day files from emi.nasdaq.com
- CppCon: Carl Cook "When a Microsecond Is an Eternity"; Fedor Pikus atomics/lock-free talks
- Gil Tene, "How NOT to Measure Latency" (coordinated omission, HdrHistogram)
- 1024cores.net (Dmitry Vyukov) on SPSC/MPMC queue design
- Ulrich Drepper, "What Every Programmer Should Know About Memory" (cache/TLB background for the experiments)
