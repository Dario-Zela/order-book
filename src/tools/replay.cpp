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
    std::uint64_t rebases = 0;
    std::uint64_t rebase_moved = 0;
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
        rebases += b->stats().rebases;
        rebase_moved += b->stats().rebase_levels_moved;
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
    std::printf("rebases         %llu (%llu levels migrated)\n",
                static_cast<unsigned long long>(rebases),
                static_cast<unsigned long long>(rebase_moved));
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

// Multi-symbol sharding (DESIGN §11 stretch): one parse/route thread, N
// engine threads each owning a locate partition (locate % N) behind its own
// SPSC ring. Per-shard BookResources stay thread-private — correct because
// each locate lands on exactly one shard and ITCH refs are day-unique, so
// the per-shard id maps see disjoint key sets. System events broadcast to
// every shard (market phase is global state).
int run_sharded(const ob::itch::MmapFile& file, int nshards, const BandConfig& bc) {
    using Ring = ob::spsc::SpscRing<BookEvent, 1u << 15>;
    std::vector<std::unique_ptr<Ring>> rings;
    std::vector<std::unique_ptr<BookResources>> resources;
    std::vector<std::unique_ptr<FlatEngine>> engines;
    for (int i = 0; i < nshards; ++i) {
        rings.push_back(std::make_unique<Ring>());
        resources.push_back(
            std::make_unique<BookResources>(kExpectedLiveOrders / static_cast<unsigned>(nshards)));
        engines.push_back(
            std::make_unique<FlatEngine>(ob::book::NullListener{},
                                         FlatFactory{resources.back().get(), bc}));
    }
    std::atomic<bool> done{false};

    ob::itch::Parser parser(file.bytes());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> consumers;
    std::vector<std::uint64_t> applied(static_cast<std::size_t>(nshards), 0);
    for (int i = 0; i < nshards; ++i) {
        consumers.emplace_back([&, i] {
            Ring& ring = *rings[static_cast<std::size_t>(i)];
            FlatEngine& eng = *engines[static_cast<std::size_t>(i)];
            BookEvent batch[256];
            std::uint64_t n_applied = 0;
            while (true) {
                const std::size_t n = ring.pop_n(batch, 256);
                for (std::size_t k = 0; k < n; ++k) ob::engine::apply_event(batch[k], eng);
                n_applied += n;
                if (n == 0) {
                    if (done.load(std::memory_order_acquire) && ring.pop_n(batch, 1) == 0)
                        break;
                    ob::spsc::cpu_relax();
                }
            }
            applied[static_cast<std::size_t>(i)] = n_applied;
        });
    }

    {
        auto emit = [&](const BookEvent& ev) {
            if (ev.kind == ob::engine::EventKind::system) {
                for (auto& r : rings) r->push(ev);  // phase is global
            } else {
                rings[ev.locate % static_cast<unsigned>(nshards)]->push(ev);
            }
        };
        ob::engine::EventEncoder<decltype(emit)> enc(emit);
        parser.run(enc);
        done.store(true, std::memory_order_release);
    }
    for (auto& t : consumers) t.join();
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::uint64_t total = 0;
    FlatEngine::Stats merged{};
    for (int i = 0; i < nshards; ++i) {
        total += applied[static_cast<std::size_t>(i)];
        const auto& s = engines[static_cast<std::size_t>(i)]->stats();
        merged.unknown_ref += s.unknown_ref;
        merged.clamped += s.clamped;
        merged.volume_lit += s.volume_lit;
        merged.volume_hidden += s.volume_hidden;
        merged.volume_cross += s.volume_cross;
    }
    std::printf("\n-- sharded replay ------------------------------------------\n");
    std::printf("shards          %d engine threads + 1 parse/route thread\n", nshards);
    std::printf("events          %llu applied (%.2fM/s over %.2f s)\n",
                static_cast<unsigned long long>(total),
                static_cast<double>(total) / secs / 1e6, secs);
    std::printf("volume          lit %llu, hidden %llu, cross %llu  <- must match unsharded\n",
                static_cast<unsigned long long>(merged.volume_lit),
                static_cast<unsigned long long>(merged.volume_hidden),
                static_cast<unsigned long long>(merged.volume_cross));
    std::printf("engine          unknown_ref %llu, clamped %llu\n",
                static_cast<unsigned long long>(merged.unknown_ref),
                static_cast<unsigned long long>(merged.clamped));
    for (int i = 0; i < nshards; ++i) {
        std::printf("shard %d         %llu events, ring occupancy hw %llu\n", i,
                    static_cast<unsigned long long>(applied[static_cast<std::size_t>(i)]),
                    static_cast<unsigned long long>(
                        rings[static_cast<std::size_t>(i)]->occupancy_high_water()));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    int threads = 2;
    int shards = 0;
    bool audit = false;
    BandConfig bc{};
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--threads=", 10) == 0) {
            threads = std::atoi(argv[i] + 10);
        } else if (std::strncmp(argv[i], "--shards=", 9) == 0) {
            shards = std::atoi(argv[i] + 9);
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
    if (path == nullptr || (threads != 1 && threads != 2) || (audit && threads != 1) ||
        shards < 0 || shards > 64) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--threads=1|2 | --shards=N] [--audit] [--bitmap]\n"
                     "       (--audit implies --threads=1)\n",
                     argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(path);
        if (shards > 0) return run_sharded(file, shards, bc);
        return threads == 1 ? run_single(file, audit, bc) : run_piped(file, bc);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
