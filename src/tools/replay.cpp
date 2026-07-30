// End-to-end replay harness: mmap an ITCH file, reconstruct all books, print
// throughput and structural stats. --threads=2 runs the §7 pipeline (parse +
// encode on the producer core, apply on the consumer core); --threads=1 is
// the single-thread baseline that justifies the SPSC design with numbers.
//
// Wall-clock msgs/s only — honest latency numbers need the paced-run harness
// (DESIGN §8; coordinated omission) which lives in bench/, not here.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "book/flat_book.hpp"
#include "engine/audit.hpp"
#include "engine/engine.hpp"
#include "engine/event.hpp"
#include "itch/mmap_file.hpp"
#include "itch/parser.hpp"
#include "spsc/spsc_ring.hpp"

namespace {

using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::engine::BookEvent;

struct FlatFactory {
    BookResources* res;
    BandConfig cfg;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, cfg);
    }
};

using FlatEngine = ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory>;

// Sized for a full day: peak live orders is low single-digit millions
// (DESIGN §5.2); growths are counted and reported if this is wrong.
constexpr std::size_t kExpectedLiveOrders = 1u << 22;  // 4M

void report(const FlatEngine& eng, const BookResources& res, const ob::itch::Parser& parser,
            std::uint64_t msgs, double secs) {
    std::uint64_t books = 0;
    std::uint64_t growths = 0;
    std::uint64_t overflow = 0;
    std::uint64_t repairs = 0;
    std::uint64_t repair_steps = 0;
    std::uint64_t level_mem = 0;
    std::uint64_t live = 0;
    for (std::uint32_t loc = 0; loc < FlatEngine::kMaxLocates; ++loc) {
        const FlatBook* b = eng.book(static_cast<StockLocate>(loc));
        if (b == nullptr) continue;
        ++books;
        growths += b->stats().band_growths;
        overflow += b->stats().overflow_hits;
        repairs += b->stats().best_repairs;
        repair_steps += b->stats().repair_steps;
        level_mem += b->memory_bytes();
        live += b->live_orders();
    }
    const auto& ps = parser.stats();
    const auto& es = eng.stats();
    std::printf("\n-- replay --------------------------------------------------\n");
    std::printf("messages        %llu (%.2fM msgs/s over %.2f s)\n",
                static_cast<unsigned long long>(msgs),
                static_cast<double>(msgs) / secs / 1e6, secs);
    std::printf("parser          unknown %llu, malformed %llu%s\n",
                static_cast<unsigned long long>(ps.unknown),
                static_cast<unsigned long long>(ps.malformed),
                ps.truncated ? ", TRUNCATED" : "");
    std::printf("engine          unknown_ref %llu, dup_ref %llu, clamped %llu\n",
                static_cast<unsigned long long>(es.unknown_ref),
                static_cast<unsigned long long>(es.duplicate_ref),
                static_cast<unsigned long long>(es.clamped));
    std::printf("volume          lit %llu, hidden %llu, cross %llu\n",
                static_cast<unsigned long long>(es.volume_lit),
                static_cast<unsigned long long>(es.volume_hidden),
                static_cast<unsigned long long>(es.volume_cross));
    std::printf("books           %llu active, %llu live orders at end\n",
                static_cast<unsigned long long>(books), static_cast<unsigned long long>(live));
    std::printf("arena           high-water %zu orders (%.1f MB), growths %llu\n",
                res.arena.high_water(),
                static_cast<double>(res.arena.high_water() * sizeof(ob::book::Order)) / 1e6,
                static_cast<unsigned long long>(res.arena.growths()));
    std::printf("id map          %zu slots (%.1f MB), growths %llu, max probe %u\n",
                res.ids.capacity(), static_cast<double>(res.ids.memory_bytes()) / 1e6,
                static_cast<unsigned long long>(res.ids.growths()),
                static_cast<unsigned>(res.ids.max_probe()));
    std::printf("bands           %.1f MB levels, %llu growths, %llu overflow hits\n",
                static_cast<double>(level_mem) / 1e6,
                static_cast<unsigned long long>(growths),
                static_cast<unsigned long long>(overflow));
    std::printf("best repairs    %llu scans, %.2f mean steps (0 = bitmap mode)\n",
                static_cast<unsigned long long>(repairs),
                repairs > 0 ? static_cast<double>(repair_steps) / static_cast<double>(repairs)
                            : 0.0);
}

int run_single(const ob::itch::MmapFile& file, bool audit, const BandConfig& bc) {
    BookResources res(kExpectedLiveOrders);
    FlatEngine eng({}, FlatFactory{&res, bc});
    ob::itch::Parser parser(file.bytes());
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t msgs = 0;
    ob::engine::FrontAudit<FlatEngine> auditor(eng);
    if (audit) {
        msgs = parser.run(auditor);
    } else {
        msgs = parser.run(eng);
    }
    const auto t1 = std::chrono::steady_clock::now();
    report(eng, res, parser, msgs, std::chrono::duration<double>(t1 - t0).count());
    std::printf("mode            single-thread baseline%s\n",
                audit ? " + execution-at-front audit" : "");
    if (audit) {
        const auto& a = auditor.stats();
        std::printf("audit           %llu checked, %llu at front (%.4f%% pass)\n",
                    static_cast<unsigned long long>(a.checked()),
                    static_cast<unsigned long long>(a.at_front()), a.pass_rate() * 100.0);
        std::printf("audit by type   E %.4f%% of %llu | C (price-improved) %.4f%% of %llu\n",
                    a.pass_rate_e() * 100.0,
                    static_cast<unsigned long long>(a.checked_e),
                    a.pass_rate_c() * 100.0,
                    static_cast<unsigned long long>(a.checked_c));
        std::printf("audit skipped   preopen %llu, halted %llu, unknown-ref %llu\n",
                    static_cast<unsigned long long>(a.skipped_preopen),
                    static_cast<unsigned long long>(a.skipped_halted),
                    static_cast<unsigned long long>(a.skipped_unknown));
    }
    return 0;
}

int run_piped(const ob::itch::MmapFile& file, const BandConfig& bc) {
    BookResources res(kExpectedLiveOrders);
    FlatEngine eng({}, FlatFactory{&res, bc});
    auto ring = std::make_unique<ob::spsc::SpscRing<BookEvent, 1u << 16>>();
    std::atomic<bool> done{false};

    ob::itch::Parser parser(file.bytes());
    const auto t0 = std::chrono::steady_clock::now();
    std::thread producer([&] {
        auto emit = [&](const BookEvent& ev) { ring->push(ev); };
        ob::engine::EventEncoder<decltype(emit)> enc(emit);
        parser.run(enc);
        done.store(true, std::memory_order_release);
    });

    std::uint64_t applied = 0;
    BookEvent batch[256];
    while (true) {
        const std::size_t n = ring->pop_n(batch, 256);
        for (std::size_t i = 0; i < n; ++i) ob::engine::apply_event(batch[i], eng);
        applied += n;
        if (n == 0) {
            if (done.load(std::memory_order_acquire) && ring->pop_n(batch, 1) == 0) break;
            ob::spsc::cpu_relax();
        }
    }
    producer.join();
    const auto t1 = std::chrono::steady_clock::now();
    report(eng, res, parser, applied, std::chrono::duration<double>(t1 - t0).count());
    std::printf("mode            two-thread pipeline, ring occupancy high-water %llu/%zu\n",
                static_cast<unsigned long long>(ring->occupancy_high_water()),
                ring->capacity());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    int threads = 2;
    bool audit = false;
    BandConfig bc{};
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--threads=", 10) == 0) {
            threads = std::atoi(argv[i] + 10);
        } else if (std::strcmp(argv[i], "--audit") == 0) {
            audit = true;
        } else if (std::strcmp(argv[i], "--bitmap") == 0) {
            bc.use_bitmap = true;
        } else if (std::strncmp(argv[i], "--band-half=", 12) == 0) {
            bc.initial_half_width = static_cast<std::uint32_t>(std::atoi(argv[i] + 12));
        } else if (std::strncmp(argv[i], "--band-max=", 11) == 0) {
            bc.max_width = static_cast<std::uint32_t>(std::atoi(argv[i] + 11));
        } else {
            path = argv[i];
        }
    }
    if (path == nullptr || (threads != 1 && threads != 2) || (audit && threads != 1)) {
        std::fprintf(stderr, "usage: %s <itch-file> [--threads=1|2] [--audit] [--bitmap]\n"
                             "       (--audit implies --threads=1)\n",
                     argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(path);
        return threads == 1 ? run_single(file, audit, bc) : run_piped(file, bc);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
