// Cuts a message-boundary-aligned prefix of an ITCH file: every whole frame
// whose timestamp is <= the cutoff. The committed first-hour slice is the
// dev inner loop (DESIGN §12); full-day runs stay nightly/local.
//
//   itch_slice in.itch out.itch --until=10:30
//
// The cutoff is ns since midnight (ITCH timestamps are exchange-local ET).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "core/endian.hpp"
#include "itch/mmap_file.hpp"

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* out_path = nullptr;
    std::uint64_t cutoff_ns = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--until=", 8) == 0) {
            int hh = 0;
            int mm = 0;
            if (std::sscanf(argv[i] + 8, "%d:%d", &hh, &mm) != 2) {
                std::fprintf(stderr, "bad --until, want HH:MM\n");
                return 2;
            }
            cutoff_ns = (static_cast<std::uint64_t>(hh) * 3600 +
                         static_cast<std::uint64_t>(mm) * 60) *
                        1'000'000'000ull;
        } else if (in_path == nullptr) {
            in_path = argv[i];
        } else {
            out_path = argv[i];
        }
    }
    if (in_path == nullptr || out_path == nullptr || cutoff_ns == 0) {
        std::fprintf(stderr, "usage: %s <in> <out> --until=HH:MM\n", argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile in(in_path);
        const auto data = in.bytes();
        std::size_t pos = 0;
        std::uint64_t msgs = 0;
        // Walk frames; stop at the first message past the cutoff.
        while (pos + 2 <= data.size()) {
            const auto len = ob::load_be<std::uint16_t>(data.data() + pos);
            if (len == 0 || pos + 2 + len > data.size()) break;
            // Common header: type(1) locate(2) tracking(2) timestamp(6).
            if (len >= 11) {
                const std::uint64_t ts = ob::load_be48(data.data() + pos + 2 + 5);
                if (ts > cutoff_ns) break;
            }
            pos += 2 + std::size_t{len};
            ++msgs;
        }
        std::FILE* out = std::fopen(out_path, "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", out_path);
            return 1;
        }
        const std::size_t written = std::fwrite(data.data(), 1, pos, out);
        std::fclose(out);
        if (written != pos) {
            std::fprintf(stderr, "short write\n");
            return 1;
        }
        std::printf("%llu messages, %.1f MB -> %s\n",
                    static_cast<unsigned long long>(msgs),
                    static_cast<double>(pos) / 1e6, out_path);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
