// Fallback driver where the libFuzzer runtime is unavailable (Apple
// command-line tools): feeds files given on argv, else N random buffers.
// NOT coverage-guided — a smoke harness, not a substitute for real libFuzzer
// runs on Linux (DESIGN §9.4). Same target code either way.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::FILE* f = std::fopen(argv[i], "rb");
            if (f == nullptr) continue;
            std::vector<std::uint8_t> buf;
            int ch;
            while ((ch = std::fgetc(f)) != EOF) buf.push_back(static_cast<std::uint8_t>(ch));
            std::fclose(f);
            LLVMFuzzerTestOneInput(buf.data(), buf.size());
            std::printf("ok %s (%zu bytes)\n", argv[i], buf.size());
        }
        return 0;
    }
    const std::uint64_t iters = 200'000;
    std::mt19937_64 rng(0xDEADBEEF);
    std::vector<std::uint8_t> buf;
    for (std::uint64_t i = 0; i < iters; ++i) {
        buf.resize(rng() % 512);
        for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
    }
    std::printf("ok: %llu random inputs, no crash (standalone driver — run real "
                "libFuzzer on Linux for coverage-guided depth)\n",
                static_cast<unsigned long long>(iters));
    return 0;
}
