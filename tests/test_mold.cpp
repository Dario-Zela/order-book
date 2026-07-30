// MoldUDP64 framing + A/B arbitration unit tests — socket-free (§11 stretch:
// "sequence-number handling is the interesting part", so it gets the tests).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "net/arbitrator.hpp"
#include "net/mold.hpp"

using ob::net::Arbitrator;
using ob::net::decode_header;
using ob::net::kEndOfSession;
using ob::net::kMoldHeaderSize;
using ob::net::MoldHeader;
using ob::net::PacketBuilder;
using ob::net::RetransmitBuffer;
using ob::net::Session;

namespace {

constexpr Session kSess = {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' ', ' ', ' '};

std::vector<std::byte> msg_bytes(std::uint8_t tag, std::size_t len = 8) {
    std::vector<std::byte> m(len, std::byte{tag});
    return m;
}

// Builds a packet holding messages tagged [first_tag, first_tag+n).
std::vector<std::byte> packet(std::uint64_t seq, std::uint8_t first_tag, int n) {
    PacketBuilder b(kSess, 1400, seq);
    for (int i = 0; i < n; ++i) {
        REQUIRE(b.add(msg_bytes(static_cast<std::uint8_t>(first_tag + i))));
    }
    auto s = b.finish();
    return {s.begin(), s.end()};
}

struct Collector {
    std::vector<std::uint8_t> tags;
    void operator()(std::span<const std::byte> m) {
        tags.push_back(static_cast<std::uint8_t>(m[0]));
    }
};

}  // namespace

TEST_CASE("mold: header round-trip and heartbeat/EOS shapes") {
    PacketBuilder b(kSess, 1400, 41);
    REQUIRE(b.add(msg_bytes(7)));
    const auto pkt = b.finish();
    const auto h = decode_header(pkt);
    REQUIRE(h.has_value());
    CHECK(h->seq == 41);
    CHECK(h->count == 1);
    CHECK(std::string(h->session.data(), 10) == "TEST      ");

    const auto hb = b.heartbeat();
    const auto hh = decode_header(hb);
    CHECK(hh->count == 0);
    CHECK(hh->seq == 42);  // advanced past the finished packet

    const auto eos = b.end_of_session();
    CHECK(decode_header(eos)->count == kEndOfSession);
}

TEST_CASE("mold: builder respects the MTU") {
    PacketBuilder b(kSess, 64, 1);
    CHECK(b.add(msg_bytes(1, 20)));
    CHECK(b.add(msg_bytes(2, 20)));
    CHECK_FALSE(b.add(msg_bytes(3, 20)));  // would exceed 64 bytes
    CHECK(b.count() == 2);
}

TEST_CASE("arb: in-order stream applies everything, no requests") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 3));
    arb.on_packet(packet(4, 13, 2));
    CHECK(c.tags == std::vector<std::uint8_t>{10, 11, 12, 13, 14});
    CHECK(arb.next_seq() == 6);
    CHECK_FALSE(arb.take_rerequest().has_value());
    CHECK(arb.stats().gaps_opened == 0);
}

TEST_CASE("arb: A/B duplicates collapse via sequence arithmetic") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 3));  // feed A
    arb.on_packet(packet(1, 10, 3));  // feed B, same packet later
    arb.on_packet(packet(4, 13, 1));  // feed B wins the next one
    arb.on_packet(packet(4, 13, 1));  // A's copy arrives late
    CHECK(c.tags == std::vector<std::uint8_t>{10, 11, 12, 13});
    CHECK(arb.stats().duplicates == 2);
}

TEST_CASE("arb: complementary losses on A and B need no re-request") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    // A delivers packets 1 and 3; B delivers 2 and 4 — between them, whole.
    arb.on_packet(packet(1, 10, 2));   // A: seq 1-2
    arb.on_packet(packet(3, 12, 2));   // B: seq 3-4  (A dropped it)
    arb.on_packet(packet(5, 14, 2));   // A: seq 5-6
    arb.on_packet(packet(7, 16, 2));   // B: seq 7-8
    CHECK(c.tags.size() == 8);
    CHECK(arb.next_seq() == 9);
    CHECK(arb.stats().gaps_opened == 0);
}

TEST_CASE("arb: gap buffers the future, requests the hole, drains in order") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 2));  // seq 1-2 applied
    arb.on_packet(packet(6, 15, 2));  // future: hole is 3,4,5
    CHECK(c.tags.size() == 2);
    const auto rq = arb.take_rerequest();
    REQUIRE(rq.has_value());
    CHECK(rq->from == 3);
    CHECK(rq->count == 3);
    // Retransmission arrives (covers 3-5); buffered 6-7 drains after it.
    arb.on_packet(packet(3, 12, 3));
    CHECK(c.tags == std::vector<std::uint8_t>{10, 11, 12, 13, 14, 15, 16});
    CHECK(arb.next_seq() == 8);
    CHECK(arb.stats().gaps_opened == 1);
}

TEST_CASE("arb: late B-feed packet can close a gap before the retransmission") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 2));
    arb.on_packet(packet(4, 13, 1));  // A jumped: hole 3
    REQUIRE(arb.take_rerequest().has_value());
    arb.on_packet(packet(3, 12, 1));  // B's copy of 3 lands first
    CHECK(c.tags == std::vector<std::uint8_t>{10, 11, 12, 13});
    // The retransmission then arrives and must be a harmless duplicate.
    arb.on_packet(packet(3, 12, 1));
    CHECK(arb.stats().duplicates == 1);
    CHECK(arb.next_seq() == 5);
}

TEST_CASE("arb: overlapping retransmission applies only the unseen suffix") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 3));  // seq 1-3
    arb.on_packet(packet(2, 11, 4));  // covers 2-5: only 4,5 are new
    CHECK(c.tags == std::vector<std::uint8_t>{10, 11, 12, 13, 14});
    CHECK(arb.stats().partial_overlaps == 1);
}

TEST_CASE("arb: heartbeat reveals a gap; EOS completes only when caught up") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(1, 10, 2));
    // Heartbeat says the sender is at 6: we are missing 3-5.
    PacketBuilder hb_b(kSess, 1400, 6);
    arb.on_packet(hb_b.heartbeat());
    const auto rq = arb.take_rerequest();
    REQUIRE(rq.has_value());
    CHECK(rq->from == 3);
    CHECK(rq->count == 3);

    // EOS at 6 while still behind: not complete. Fill, then complete.
    arb.on_packet(hb_b.end_of_session());
    CHECK_FALSE(arb.stats().session_complete);
    arb.on_packet(packet(3, 12, 3));
    CHECK(arb.stats().session_complete);
}

TEST_CASE("arb: mid-session join starts at the first seq seen") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    arb.on_packet(packet(500, 10, 2));
    CHECK(arb.next_seq() == 502);
    CHECK(arb.stats().gaps_opened == 0);  // no request for 1..499
}

TEST_CASE("arb: malformed packets are counted, never applied") {
    Collector c;
    Arbitrator<Collector&> arb(c);
    std::vector<std::byte> junk(10, std::byte{0xFF});  // shorter than a header
    arb.on_packet(junk);
    CHECK(arb.stats().malformed == 1);
    // Claims 3 messages, carries garbage framing.
    std::vector<std::byte> bad(kMoldHeaderSize + 3, std::byte{0xFF});
    ob::net::encode_header({kSess, 1, 3}, bad.data());
    arb.on_packet(bad);
    CHECK(arb.stats().malformed == 2);
    CHECK(c.tags.empty());
}

TEST_CASE("rewinder: bounded lookback serves overlapping packets") {
    RetransmitBuffer rb(2);
    const auto p1 = packet(1, 10, 2);
    const auto p2 = packet(3, 12, 2);
    const auto p3 = packet(5, 14, 2);
    rb.store(1, 2, p1);
    rb.store(3, 2, p2);
    rb.store(5, 2, p3);  // evicts p1: bounded lookback, like real rewinders
    CHECK(rb.oldest_seq() == 3);
    CHECK(rb.lookup(1, 2).empty());          // too old: refused
    CHECK(rb.lookup(4, 3).size() == 2);      // spans p2 and p3
    CHECK(rb.lookup(3, 1).size() == 1);
}
