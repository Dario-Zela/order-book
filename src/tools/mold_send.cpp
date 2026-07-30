// MoldUDP64 sender (DESIGN §11 stretch): streams an ITCH file as a
// dual-feed UDP session with simulated independent packet loss per feed,
// and serves gap-fill re-requests from a bounded rewinder — the moving
// parts of a real market-data distribution, on loopback.
//
//   mold_send <itch-file> --a=127.0.0.1:6001 --b=127.0.0.1:6002 \
//             --rewind-port=6010 [--pps=50000] [--drop-a=2] [--drop-b=2] \
//             [--mtu=1200] [--linger=10]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <random>
#include <thread>

#include "core/endian.hpp"
#include "itch/mmap_file.hpp"
#include "net/mold.hpp"
#include "net/udp.hpp"

using namespace std::chrono;

int main(int argc, char** argv) {
    const char* path = nullptr;
    std::string addr_a = "127.0.0.1:6001";
    std::string addr_b = "127.0.0.1:6002";
    int rewind_port = 6010;
    double pps = 25'000;
    int drop_a = 0;
    int drop_b = 0;
    std::size_t mtu = 1200;
    int linger_s = 10;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--a=", 4) == 0) addr_a = argv[i] + 4;
        else if (std::strncmp(argv[i], "--b=", 4) == 0) addr_b = argv[i] + 4;
        else if (std::strncmp(argv[i], "--rewind-port=", 14) == 0) rewind_port = std::atoi(argv[i] + 14);
        else if (std::strncmp(argv[i], "--pps=", 6) == 0) pps = std::atof(argv[i] + 6);
        else if (std::strncmp(argv[i], "--drop-a=", 9) == 0) drop_a = std::atoi(argv[i] + 9);
        else if (std::strncmp(argv[i], "--drop-b=", 9) == 0) drop_b = std::atoi(argv[i] + 9);
        else if (std::strncmp(argv[i], "--mtu=", 6) == 0) mtu = std::strtoull(argv[i] + 6, nullptr, 10);
        else if (std::strncmp(argv[i], "--linger=", 9) == 0) linger_s = std::atoi(argv[i] + 9);
        else path = argv[i];
    }
    if (path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--a=host:port] [--b=host:port]\n"
                     "          [--rewind-port=N] [--pps=N] [--drop-a=%%] [--drop-b=%%]\n"
                     "          [--mtu=N] [--linger=secs]\n",
                     argv[0]);
        return 2;
    }
    try {
        const auto ep_a = *ob::net::Endpoint::parse(addr_a);
        const auto ep_b = *ob::net::Endpoint::parse(addr_b);
        const ob::itch::MmapFile file(path);

        ob::net::UdpSocket tx;
        tx.bind(0);
        ob::net::UdpSocket rewind;
        rewind.bind(static_cast<std::uint16_t>(rewind_port));

        constexpr ob::net::Session kSess = {'0', '1', '3', '0', '2', '0', '2', '0', ' ', ' '};
        ob::net::PacketBuilder builder(kSess, mtu, 1);
        ob::net::RetransmitBuffer rb(1 << 18);  // ~262k packets of lookback
        std::mt19937_64 rng_a(101);
        std::mt19937_64 rng_b(202);

        std::uint64_t sent_pkts = 0;
        std::uint64_t served = 0;
        std::uint64_t refused = 0;
        const auto t0 = steady_clock::now();
        const auto interval = duration<double>(1.0 / pps);

        auto serve_rerequests = [&](int timeout_ms) {
            std::byte req[64];
            ob::net::Endpoint from;
            while (const auto n = rewind.recv({req, sizeof(req)}, timeout_ms, &from)) {
                const auto h = ob::net::decode_header({req, n});
                if (!h) continue;
                const auto pkts = rb.lookup(h->seq, h->count);
                if (pkts.empty()) ++refused;  // beyond the rewinder window
                for (const auto& p : pkts) tx.send_to(from, p);
                served += pkts.size();
            }
        };

        auto emit = [&] {
            if (builder.count() == 0) return;
            const auto seq = builder.next_seq();
            const auto n = builder.count();
            const auto pkt = builder.finish();
            rb.store(seq, n, pkt);
            if (static_cast<int>(rng_a() % 100) >= drop_a) tx.send_to(ep_a, pkt);
            if (static_cast<int>(rng_b() % 100) >= drop_b) tx.send_to(ep_b, pkt);
            builder.begin();
            ++sent_pkts;
            // Pace to the target packet rate; serve re-requests in the slack.
            const auto due = t0 + duration_cast<steady_clock::duration>(
                                      interval * static_cast<double>(sent_pkts));
            while (steady_clock::now() < due) serve_rerequests(0);
        };

        const auto data = file.bytes();
        std::size_t pos = 0;
        while (pos + 2 <= data.size()) {
            const auto len = ob::load_be<std::uint16_t>(data.data() + pos);
            if (len == 0 || pos + 2 + len > data.size()) break;
            const auto msg = data.subspan(pos + 2, len);
            if (!builder.add(msg)) {
                emit();
                builder.add(msg);
                // Serve re-requests even when sending is the bottleneck —
                // otherwise the rewinder only answers during pacing slack,
                // and a behind receiver starves until linger.
                if (sent_pkts % 64 == 0) serve_rerequests(0);
            }
            pos += 2 + std::size_t{len};
        }
        emit();

        // Session tail: heartbeats + end-of-session while serving re-requests.
        const auto eos = builder.end_of_session();
        const auto hb = builder.heartbeat();
        const auto until = steady_clock::now() + seconds(linger_s);
        while (steady_clock::now() < until) {
            tx.send_to(ep_a, hb);
            tx.send_to(ep_b, eos);
            serve_rerequests(50);
        }

        const double secs = duration<double>(steady_clock::now() - t0).count();
        std::printf("sent %llu packets (%llu msgs) in %.1f s | retransmitted %llu, refused %llu\n",
                    static_cast<unsigned long long>(sent_pkts),
                    static_cast<unsigned long long>(builder.next_seq() - 1), secs,
                    static_cast<unsigned long long>(served),
                    static_cast<unsigned long long>(refused));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
