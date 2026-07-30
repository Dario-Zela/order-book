// Snapshot tool (DESIGN §11 stretch): start a replay mid-day.
//
//   snapshot save   <itch-file> <out.snap> --at=N     replay N msgs, dump state
//   snapshot resume <itch-file> <in.snap>             restore, replay to end
//
// resume prints the same style of report as `replay`, so a straight replay
// and a save+resume pair are directly comparable. Also unlocks fast golden
// iteration: snapshot just before the region under study, resume from there.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

#include "book/flat_book.hpp"
#include "engine/engine.hpp"
#include "engine/snapshot.hpp"
#include "itch/mmap_file.hpp"
#include "itch/parser.hpp"

namespace {

using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;

struct FlatFactory {
    BookResources* res;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, BandConfig{});
    }
};
using FlatEngine = ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory>;

void report(const FlatEngine& eng, std::uint64_t msgs, double secs) {
    std::uint64_t books = 0;
    std::uint64_t live = 0;
    for (std::uint32_t l = 0; l < FlatEngine::kMaxLocates; ++l) {
        const auto* b = eng.book(static_cast<StockLocate>(l));
        if (b == nullptr) continue;
        ++books;
        live += b->live_orders();
    }
    const auto& es = eng.stats();
    std::printf("messages        %llu (%.2fM msgs/s over %.2f s)\n",
                static_cast<unsigned long long>(msgs),
                static_cast<double>(msgs) / secs / 1e6, secs);
    std::printf("volume          lit %llu, hidden %llu, cross %llu\n",
                static_cast<unsigned long long>(es.volume_lit),
                static_cast<unsigned long long>(es.volume_hidden),
                static_cast<unsigned long long>(es.volume_cross));
    std::printf("books           %llu active, %llu live orders\n",
                static_cast<unsigned long long>(books),
                static_cast<unsigned long long>(live));
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = nullptr;
    const char* itch_path = nullptr;
    const char* snap_path = nullptr;
    std::uint64_t at = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--at=", 5) == 0) {
            at = std::strtoull(argv[i] + 5, nullptr, 10);
        } else if (mode == nullptr) {
            mode = argv[i];
        } else if (itch_path == nullptr) {
            itch_path = argv[i];
        } else {
            snap_path = argv[i];
        }
    }
    const bool save = mode != nullptr && std::strcmp(mode, "save") == 0;
    const bool resume = mode != nullptr && std::strcmp(mode, "resume") == 0;
    if (itch_path == nullptr || snap_path == nullptr || (!save && !resume) ||
        (save && at == 0)) {
        std::fprintf(stderr,
                     "usage: %s save <itch-file> <out.snap> --at=N\n"
                     "       %s resume <itch-file> <in.snap>\n",
                     argv[0], argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(itch_path);
        BookResources res(1u << 22);
        FlatEngine eng({}, FlatFactory{&res});
        const auto t0 = std::chrono::steady_clock::now();
        if (save) {
            ob::itch::Parser p(file.bytes());
            std::uint64_t n = 0;
            while (n < at && p.next(eng)) ++n;
            std::FILE* f = std::fopen(snap_path, "wb");
            if (f == nullptr || !ob::engine::save_snapshot(eng, {n, p.offset()}, f)) {
                std::fprintf(stderr, "snapshot write failed\n");
                return 1;
            }
            std::fclose(f);
            const auto secs = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            report(eng, n, secs);
            std::printf("snapshot        %s at message %llu (offset %llu)\n", snap_path,
                        static_cast<unsigned long long>(n),
                        static_cast<unsigned long long>(p.offset()));
            return 0;
        }
        // resume
        std::FILE* f = std::fopen(snap_path, "rb");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", snap_path);
            return 1;
        }
        const auto meta = ob::engine::load_snapshot(eng, f);
        std::fclose(f);
        if (!meta) {
            std::fprintf(stderr, "bad snapshot\n");
            return 1;
        }
        ob::itch::Parser p(file.bytes().subspan(meta->file_offset));
        const auto resumed = p.run(eng);
        const auto secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("resumed at      message %llu\n",
                    static_cast<unsigned long long>(meta->messages));
        report(eng, meta->messages + resumed, secs);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
