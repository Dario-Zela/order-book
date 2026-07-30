// Latency/throughput bench harness (DESIGN §8).
//
// Two run types with different claims — never mixed:
//
//   --mode=throughput   as-fast-as-possible; reports msgs/s ONLY. Running
//                       flat out saturates the pipeline, so any "latency"
//                       measured here would just be queue depth in disguise.
//
//   --mode=paced        event-time-paced replay (scaled by --speed). Each
//                       message's ingress is stamped at its INTENDED arrival
//                       time, not when the pipeline got around to it, so a
//                       stall is charged to every message it delays — the
//                       coordinated-omission correction (Gil Tene, "How NOT
//                       to Measure Latency"). Latency = egress (book updated)
//                       - intended ingress. Reported as p50/p90/p99/p99.9/max
//                       from a fixed-bucket log histogram, alongside the
//                       measured cost of the clock itself, which bounds any
//                       claim.
//
// Pacing starts at market open (System Event 'Q'): the pre-open book build
// is applied unpaced and unrecorded, then a --warmup-secs window (also
// unrecorded) lets caches settle. Both modes exist in --threads=1 and
// --threads=2 (SPSC pipeline) variants sharing the BookEvent code path.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

#include "book/flat_book.hpp"
#include "core/clock.hpp"
#include "core/histogram.hpp"
#include "engine/engine.hpp"
#include "engine/event.hpp"
#include "itch/mmap_file.hpp"
#include "itch/parser.hpp"
#include "spsc/spsc_ring.hpp"

namespace {

using ob::LogHistogram;
using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::engine::BookEvent;

struct FlatFactory {
    BookResources* res;
    BandConfig band_cfg;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, band_cfg);
    }
};
using FlatEngine = ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory>;

constexpr std::size_t kExpectedLiveOrders = 1u << 22;

struct Config {
    const char* path = nullptr;
    bool paced = false;
    int threads = 1;
    double speed = 50.0;       // event-time compression factor
    double warmup_secs = 5.0;  // wall-clock, unrecorded, after pacing starts
    std::uint64_t max_msgs = 0;  // 0 = all
    int runs = 5;                // throughput mode only
    BandConfig band{};
};

// Maps event time to intended wall arrival once pacing is armed.
class Pacer {
public:
    void arm(std::uint64_t event_ts, double speed) {
        armed_ = true;
        ts0_ = event_ts;
        t0_ = ob::now_ns();
        inv_speed_ = 1.0 / speed;
    }
    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] std::uint64_t intended(std::uint64_t event_ts) const {
        const auto delta = static_cast<double>(event_ts - ts0_) * inv_speed_;
        return t0_ + static_cast<std::uint64_t>(delta);
    }
    [[nodiscard]] std::uint64_t t0() const { return t0_; }

private:
    bool armed_ = false;
    std::uint64_t ts0_ = 0;
    std::uint64_t t0_ = 0;
    double inv_speed_ = 0.0;
};

void print_histogram(const LogHistogram& h, double secs, std::uint64_t clock_cost) {
    std::printf("paced samples   %llu over %.1f s\n",
                static_cast<unsigned long long>(h.total()), secs);
    std::printf("latency ns      p50 %llu | p90 %llu | p99 %llu | p99.9 %llu | max %llu\n",
                static_cast<unsigned long long>(h.quantile(0.50)),
                static_cast<unsigned long long>(h.quantile(0.90)),
                static_cast<unsigned long long>(h.quantile(0.99)),
                static_cast<unsigned long long>(h.quantile(0.999)),
                static_cast<unsigned long long>(h.max()));
    std::printf("clock overhead  ~%llu ns/read (bounds all claims above)\n",
                static_cast<unsigned long long>(clock_cost));
}

// ---------------------------------------------------------------- throughput

int run_throughput(const ob::itch::MmapFile& file, const Config& cfg) {
    std::printf("mode            throughput (as-fast-as-possible, msgs/s ONLY —\n"
                "                latency is meaningless in this run type, §8)\n");
    double best = 0;
    for (int r = 0; r < cfg.runs; ++r) {
        BookResources res(kExpectedLiveOrders);
        FlatEngine eng({}, FlatFactory{&res, cfg.band});
        ob::itch::Parser parser(file.bytes());
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t msgs = 0;
        if (cfg.threads == 1) {
            msgs = parser.run(eng);
        } else {
            auto ring = std::make_unique<ob::spsc::SpscRing<BookEvent, 1u << 16>>();
            std::atomic<bool> done{false};
            std::thread producer([&] {
                auto emit = [&](const BookEvent& ev) { ring->push(ev); };
                ob::engine::EventEncoder<decltype(emit)> enc(emit);
                parser.run(enc);
                done.store(true, std::memory_order_release);
            });
            BookEvent batch[256];
            while (true) {
                const std::size_t n = ring->pop_n(batch, 256);
                for (std::size_t i = 0; i < n; ++i) ob::engine::apply_event(batch[i], eng);
                msgs += n;
                if (n == 0) {
                    if (done.load(std::memory_order_acquire) && ring->pop_n(batch, 1) == 0)
                        break;
                    ob::spsc::cpu_relax();
                }
            }
            producer.join();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        const double rate = static_cast<double>(msgs) / secs / 1e6;
        if (rate > best) best = rate;
        std::printf("run %d           %.2fM msgs/s (%.2f s, %llu msgs)%s\n", r + 1, rate,
                    secs, static_cast<unsigned long long>(msgs),
                    r == 0 ? "  <- includes page-cache warm-up" : "");
    }
    std::printf("best            %.2fM msgs/s (report median across >=5 runs on a\n"
                "                quiet machine; run 1 is the cold pass)\n", best);
    return 0;
}

// --------------------------------------------------------------------- paced

// Paces, stamps intended ingress, applies via `apply`, records via `record`.
template <typename Apply>
class PacedFeed {
public:
    PacedFeed(const Config& cfg, Apply apply) : cfg_(cfg), apply_(apply) {}

    void operator()(BookEvent ev) {
        if (!pacer_.armed()) {
            if (ev.kind == ob::engine::EventKind::system && ev.flag == 'Q') {
                pacer_.arm(ev.ts, cfg_.speed);
                record_from_ =
                    pacer_.t0() +
                    static_cast<std::uint64_t>(cfg_.warmup_secs * 1e9);
            }
            ev.ingress_ns = 0;  // pre-open build: unpaced, unrecorded
            apply_(ev);
            return;
        }
        const std::uint64_t intended = pacer_.intended(ev.ts);
        while (ob::now_ns() < intended) ob::spsc::cpu_relax();
        ev.ingress_ns = intended >= record_from_ ? intended : 0;
        apply_(ev);
    }

private:
    Config cfg_;
    Apply apply_;
    Pacer pacer_;
    std::uint64_t record_from_ = ~std::uint64_t{0};
};

int run_paced(const ob::itch::MmapFile& file, const Config& cfg) {
    std::printf("mode            event-time-paced at %.0fx, %d thread(s); ingress\n"
                "                stamped at INTENDED arrival (coordinated omission\n"
                "                addressed); pacing from market open, first %.0f s\n"
                "                after open discarded as warmup\n",
                cfg.speed, cfg.threads, cfg.warmup_secs);
    BookResources res(kExpectedLiveOrders);
    FlatEngine eng({}, FlatFactory{&res, cfg.band});
    LogHistogram hist;
    const std::uint64_t clock_cost = ob::clock_overhead_ns();
    std::uint64_t paced_msgs = 0;

    auto apply_and_record = [&](const BookEvent& ev) {
        ob::engine::apply_event(ev, eng);
        if (ev.ingress_ns != 0) {
            hist.record(ob::now_ns() - ev.ingress_ns);
        }
    };

    const auto wall0 = std::chrono::steady_clock::now();
    if (cfg.threads == 1) {
        PacedFeed feed(cfg, apply_and_record);
        auto emit = [&](const BookEvent& ev) {
            feed(ev);
            ++paced_msgs;
        };
        ob::engine::EventEncoder<decltype(emit)> enc(emit);
        ob::itch::Parser parser(file.bytes());
        while (parser.next(enc)) {
            if (cfg.max_msgs != 0 && paced_msgs >= cfg.max_msgs) break;
        }
    } else {
        auto ring = std::make_unique<ob::spsc::SpscRing<BookEvent, 1u << 16>>();
        std::atomic<bool> done{false};
        std::thread producer([&] {
            // Producer paces and stamps; ring time is inside the measurement.
            PacedFeed feed(cfg, [&](const BookEvent& ev) { ring->push(ev); });
            std::uint64_t sent = 0;
            auto emit = [&](const BookEvent& ev) {
                feed(ev);
                ++sent;
            };
            ob::engine::EventEncoder<decltype(emit)> enc(emit);
            ob::itch::Parser parser(file.bytes());
            while (parser.next(enc)) {
                if (cfg.max_msgs != 0 && sent >= cfg.max_msgs) break;
            }
            done.store(true, std::memory_order_release);
        });
        BookEvent batch[256];
        while (true) {
            const std::size_t n = ring->pop_n(batch, 256);
            for (std::size_t i = 0; i < n; ++i) apply_and_record(batch[i]);
            paced_msgs += n;
            if (n == 0) {
                if (done.load(std::memory_order_acquire) && ring->pop_n(batch, 1) == 0)
                    break;
                ob::spsc::cpu_relax();
            }
        }
        producer.join();
    }
    const auto wall1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(wall1 - wall0).count();

    print_histogram(hist, secs, clock_cost);
    std::printf("messages        %llu total (%.2fM msgs/s achieved wall rate)\n",
                static_cast<unsigned long long>(paced_msgs),
                static_cast<double>(paced_msgs) / secs / 1e6);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--mode=", 7) == 0) {
            cfg.paced = std::strcmp(argv[i] + 7, "paced") == 0;
        } else if (std::strncmp(argv[i], "--threads=", 10) == 0) {
            cfg.threads = std::atoi(argv[i] + 10);
        } else if (std::strncmp(argv[i], "--speed=", 8) == 0) {
            cfg.speed = std::atof(argv[i] + 8);
        } else if (std::strncmp(argv[i], "--warmup-secs=", 14) == 0) {
            cfg.warmup_secs = std::atof(argv[i] + 14);
        } else if (std::strncmp(argv[i], "--max-msgs=", 11) == 0) {
            cfg.max_msgs = std::strtoull(argv[i] + 11, nullptr, 10);
        } else if (std::strncmp(argv[i], "--runs=", 7) == 0) {
            cfg.runs = std::atoi(argv[i] + 7);
        } else if (std::strcmp(argv[i], "--bitmap") == 0) {
            cfg.band.use_bitmap = true;
        } else {
            cfg.path = argv[i];
        }
    }
    if (cfg.path == nullptr || (cfg.threads != 1 && cfg.threads != 2) ||
        (cfg.paced && cfg.speed <= 0.0)) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--mode=throughput|paced] [--threads=1|2]\n"
                     "          [--speed=X] [--warmup-secs=S] [--max-msgs=N] [--runs=R]\n",
                     argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(cfg.path);
        return cfg.paced ? run_paced(file, cfg) : run_throughput(file, cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
