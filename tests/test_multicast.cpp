// Multicast smoke (§11 stretch): join a group, loop packets through the
// local stack, decode MoldUDP64 frames off it. Multicast loopback is
// environment-dependent (some CI containers route it nowhere), so an
// environment that can't deliver skips rather than fails — the arbitration
// logic itself is transport-agnostic and fully covered by the unicast tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "net/mold.hpp"
#include "net/udp.hpp"

TEST_CASE("multicast: mold frames survive a group loopback") {
    using ob::net::Endpoint;
    using ob::net::UdpSocket;

    UdpSocket rx;
    UdpSocket tx;
    try {
        rx.bind(0);
        const auto group =
            *Endpoint::parse("239.255.42.42:" + std::to_string(rx.local_port()));
        rx.join_multicast(group);
        tx.bind(0);

        constexpr ob::net::Session kSess = {'M', 'C', 'A', 'S', 'T', ' ', ' ', ' ', ' ', ' '};
        ob::net::PacketBuilder b(kSess, 512, 7);
        const std::byte payload[4] = {std::byte{'t'}, std::byte{'e'}, std::byte{'s'},
                                      std::byte{'t'}};
        REQUIRE(b.add(payload));
        tx.send_to(group, b.finish());

        std::byte buf[512];
        std::size_t n = 0;
        for (int i = 0; i < 20 && n == 0; ++i) {
            n = rx.recv(buf, 50);
        }
        if (n == 0) {
            SKIP("environment does not loop multicast back (container/CI quirk)");
        }
        const auto h = ob::net::decode_header({buf, n});
        REQUIRE(h.has_value());
        CHECK(h->seq == 7);
        CHECK(h->count == 1);
        CHECK(std::string(h->session.data(), 5) == "MCAST");
    } catch (const std::exception& e) {
        SKIP("multicast unsupported here: " << e.what());
    }
}
