// MoldUDP64 receiver (DESIGN §11 stretch): binds two feeds + a re-request
// socket, arbitrates the streams, and reconstructs books with the same
// engine as file replay — so the outputs are directly comparable. Run
// `replay` on the same file and the volume/book numbers must match.
//
//   mold_recv --a-port=6001 --b-port=6002 --rewind=127.0.0.1:6010 [--idle=5]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

#include "book/flat_book.hpp"
#include "engine/engine.hpp"
#include "itch/parser.hpp"
#include "net/arbitrator.hpp"
#include "net/mold.hpp"
#include "net/udp.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    int port_a = 6001;
    int port_b = 6002;
    std::string rewind_addr = "127.0.0.1:6010";
    int idle_limit_s = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--a-port=", 9) == 0) port_a = std::atoi(argv[i] + 9);
        else if (std::strncmp(argv[i], "--b-port=", 9) == 0) port_b = std::atoi(argv[i] + 9);
        else if (std::strncmp(argv[i], "--rewind=", 9) == 0) rewind_addr = argv[i] + 9;
        else if (std::strncmp(argv[i], "--idle=", 7) == 0) idle_limit_s = std::atoi(argv[i] + 7);
    }
    try {
        const auto ep_rewind = *ob::net::Endpoint::parse(rewind_addr);
        ob::net::UdpSocket rx_a;
        rx_a.bind(static_cast<std::uint16_t>(port_a));
        rx_a.set_recv_buffer(32 << 20);
        ob::net::UdpSocket rx_b;
        rx_b.bind(static_cast<std::uint16_t>(port_b));
        rx_b.set_recv_buffer(32 << 20);
        ob::net::UdpSocket req_sock;
        req_sock.bind(0);

        BookResources res(1u << 22);
        FlatEngine eng({}, FlatFactory{&res});
        constexpr ob::net::Session kSess = {'0', '1', '3', '0', '2', '0', '2', '0', ' ', ' '};

        auto apply = [&](std::span<const std::byte> m) {
            ob::itch::Parser::dispatch_one(m, eng);
        };
        ob::net::Arbitrator<decltype(apply)> arb(apply, 1 << 14);

        std::byte buf[2048];
        int idle_ms = 0;
        while (!arb.stats().session_complete && idle_ms < idle_limit_s * 1000) {
            bool got = false;
            for (ob::net::UdpSocket* s : {&rx_a, &rx_b, &req_sock}) {
                while (const auto n = s->recv({buf, sizeof(buf)}, 0)) {
                    arb.on_packet({buf, n});
                    got = true;
                }
            }
            if (const auto rq = arb.take_rerequest()) {
                std::byte req[ob::net::kMoldHeaderSize];
                ob::net::encode_header({kSess, rq->from, rq->count}, req);
                req_sock.send_to(ep_rewind, req);
            }
            if (!got) {
                // Nothing pending anywhere: block ~1ms on feed A.
                if (const auto n = rx_a.recv({buf, sizeof(buf)}, 1)) {
                    arb.on_packet({buf, n});
                    idle_ms = 0;
                } else {
                    idle_ms += 1;
                }
            } else {
                idle_ms = 0;
            }
        }

        const auto& a = arb.stats();
        const auto& es = eng.stats();
        std::printf("-- mold_recv ----------------------------------------------\n");
        std::printf("session         %s\n", a.session_complete ? "COMPLETE" : "TIMED OUT (gap unfilled or sender gone)");
        std::printf("packets         %llu | dups %llu | partial overlaps %llu | malformed %llu\n",
                    static_cast<unsigned long long>(a.packets),
                    static_cast<unsigned long long>(a.duplicates),
                    static_cast<unsigned long long>(a.partial_overlaps),
                    static_cast<unsigned long long>(a.malformed));
        std::printf("gaps            %llu opened, %llu re-requests, %llu buffered drops\n",
                    static_cast<unsigned long long>(a.gaps_opened),
                    static_cast<unsigned long long>(a.rerequests),
                    static_cast<unsigned long long>(a.buffered_drops));
        std::printf("messages        %llu applied\n",
                    static_cast<unsigned long long>(a.messages_applied));
        std::printf("volume          lit %llu, hidden %llu, cross %llu  <- compare vs replay\n",
                    static_cast<unsigned long long>(es.volume_lit),
                    static_cast<unsigned long long>(es.volume_hidden),
                    static_cast<unsigned long long>(es.volume_cross));
        std::printf("engine          unknown_ref %llu, clamped %llu\n",
                    static_cast<unsigned long long>(es.unknown_ref),
                    static_cast<unsigned long long>(es.clamped));
        return a.session_complete ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
