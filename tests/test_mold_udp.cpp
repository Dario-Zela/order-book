// End-to-end MoldUDP64 over real loopback sockets: a sender streams a
// synthetic ITCH session over two lossy "feeds" (deterministic independent
// drops) plus a rewinder port serving re-requests; the receiver arbitrates
// into an Engine<RefBook>. Acceptance: the reconstructed book equals a
// direct replay of the same stream, despite the losses.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "itch/parser.hpp"
#include "net/arbitrator.hpp"
#include "net/mold.hpp"
#include "net/udp.hpp"
#include "stream_gen.hpp"
#include "wire.hpp"

using ob::Side;
using ob::StockLocate;
using ob::book::RefBook;
using ob::engine::Engine;
using ob::net::Arbitrator;
using ob::net::Endpoint;
using ob::net::PacketBuilder;
using ob::net::RetransmitBuffer;
using ob::net::Session;
using ob::net::UdpSocket;

namespace {

Endpoint loopback(std::uint16_t port) {
    return *Endpoint::parse("127.0.0.1:" + std::to_string(port));
}

}  // namespace

TEST_CASE("mold-udp: lossy dual-feed session reconstructs exactly") {
    constexpr Session kSess = {'D', 'A', 'Y', '0', '1', ' ', ' ', ' ', ' ', ' '};
    std::vector<StockLocate> locates;
    const auto wire = obtest::make_stream(4242, 30'000, locates);

    // Direct replay: the ground truth.
    Engine<RefBook> truth;
    {
        ob::itch::Parser p(wire.bytes());
        p.run(truth);
    }

    // Receiver sockets (ephemeral ports).
    UdpSocket rx_a;
    rx_a.bind(0);
    rx_a.set_recv_buffer(4 << 20);
    UdpSocket rx_b;
    rx_b.bind(0);
    rx_b.set_recv_buffer(4 << 20);
    UdpSocket rewind;
    rewind.bind(0);
    const auto ep_a = loopback(rx_a.local_port());
    const auto ep_b = loopback(rx_b.local_port());
    const auto ep_rewind = loopback(rewind.local_port());

    std::atomic<bool> sender_done{false};

    std::thread sender([&] {
        UdpSocket tx;
        tx.bind(0);
        RetransmitBuffer rb(1 << 16);
        PacketBuilder builder(kSess, 1200, 1);
        // Deterministic, independent per-feed loss. Loss rates high enough
        // that BOTH feeds drop some packets and re-requests must happen.
        std::mt19937_64 drop_a(1);
        std::mt19937_64 drop_b(2);

        auto emit_packet = [&] {
            if (builder.count() == 0) return;
            const auto first_seq = builder.next_seq();
            const auto n = builder.count();
            const auto pkt = builder.finish();
            rb.store(first_seq, n, pkt);
            if (drop_a() % 100 >= 12) tx.send_to(ep_a, pkt);  // 12% loss on A
            if (drop_b() % 100 >= 12) tx.send_to(ep_b, pkt);  // 12% loss on B
            builder.begin();
        };

        // Walk the framed stream; group messages into packets.
        const auto data = wire.bytes();
        std::size_t pos = 0;
        int since_pause = 0;
        while (pos + 2 <= data.size()) {
            const auto len = ob::load_be<std::uint16_t>(data.data() + pos);
            if (len == 0 || pos + 2 + len > data.size()) break;
            const auto msg = data.subspan(pos + 2, len);
            if (!builder.add(msg)) {
                emit_packet();
                REQUIRE(builder.add(msg));
            }
            pos += 2 + std::size_t{len};
            // Serve any pending re-requests and pace a little so loopback
            // buffers never overflow silently.
            if (++since_pause == 64) {
                since_pause = 0;
                emit_packet();
                std::byte req[64];
                Endpoint from;
                while (const auto n = rewind.recv(req, 0, &from)) {
                    const auto h = ob::net::decode_header({req, n});
                    if (!h) continue;
                    for (const auto& p : rb.lookup(h->seq, h->count)) {
                        tx.send_to(from, p);
                    }
                }
                std::this_thread::yield();
            }
        }
        emit_packet();
        // Keep serving re-requests + heartbeats until the receiver is done.
        const auto eos = builder.end_of_session();
        for (int i = 0; i < 4000 && !sender_done.load(std::memory_order_acquire); ++i) {
            tx.send_to(ep_a, builder.heartbeat());
            tx.send_to(ep_b, eos);
            std::byte req[64];
            Endpoint from;
            while (const auto n = rewind.recv(req, 1, &from)) {
                const auto h = ob::net::decode_header({req, n});
                if (!h) continue;
                for (const auto& p : rb.lookup(h->seq, h->count)) {
                    tx.send_to(from, p);
                }
            }
        }
    });

    // Receiver: arbitrate both feeds into an engine.
    Engine<RefBook> rebuilt;
    auto apply = [&](std::span<const std::byte> m) {
        ob::itch::Parser::dispatch_one(m, rebuilt);
    };
    Arbitrator<decltype(apply)> arb(apply, 4096);

    UdpSocket req_sock;
    req_sock.bind(0);
    std::byte buf[2048];
    int idle = 0;
    while (!arb.stats().session_complete && idle < 2000) {
        bool got = false;
        // Retransmissions come back unicast to the REQUESTING socket (as on
        // real rewinders), so it is read alongside the two feeds.
        for (UdpSocket* s : {&rx_a, &rx_b, &req_sock}) {
            if (const auto n = s->recv(buf, 1)) {
                arb.on_packet({buf, n});
                got = true;
            }
        }
        if (const auto rq = arb.take_rerequest()) {
            std::byte req[ob::net::kMoldHeaderSize];
            ob::net::encode_header({kSess, rq->from, rq->count}, req);
            req_sock.send_to(ep_rewind, req);
        }
        idle = got ? 0 : idle + 1;
    }
    sender_done.store(true, std::memory_order_release);
    sender.join();

    INFO("gaps=" << arb.stats().gaps_opened << " rerequests=" << arb.stats().rerequests
                 << " dups=" << arb.stats().duplicates
                 << " applied=" << arb.stats().messages_applied);
    REQUIRE(arb.stats().session_complete);
    // The lossy path must have actually been exercised...
    CHECK(arb.stats().duplicates > 0);  // A/B overlap collapsed
    // ...and the rebuilt books must equal ground truth everywhere.
    CHECK(truth.stats().volume_lit == rebuilt.stats().volume_lit);
    CHECK(truth.stats().unknown_ref == rebuilt.stats().unknown_ref);
    for (StockLocate loc : locates) {
        REQUIRE(rebuilt.book(loc) != nullptr);
        CHECK(rebuilt.book(loc)->validate());
        CHECK(truth.book(loc)->live_orders() == rebuilt.book(loc)->live_orders());
        for (Side s : {Side::Bid, Side::Ask}) {
            REQUIRE(truth.book(loc)->l2(s) == rebuilt.book(loc)->l2(s));
        }
    }
}
