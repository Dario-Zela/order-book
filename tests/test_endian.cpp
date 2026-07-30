#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

#include "core/endian.hpp"
#include "core/types.hpp"

TEST_CASE("byteswap reverses byte order") {
    CHECK(ob::byteswap<std::uint16_t>(0x1234) == 0x3412);
    CHECK(ob::byteswap<std::uint32_t>(0x12345678u) == 0x78563412u);
    CHECK(ob::byteswap<std::uint64_t>(0x0102030405060708ull) == 0x0807060504030201ull);
    CHECK(ob::byteswap<std::uint8_t>(0xABu) == 0xABu);
}

TEST_CASE("load_be reads big-endian wire values") {
    const std::array<unsigned char, 8> buf{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    CHECK(ob::load_be<std::uint16_t>(buf.data()) == 0x0102u);
    CHECK(ob::load_be<std::uint32_t>(buf.data()) == 0x01020304u);
    CHECK(ob::load_be<std::uint64_t>(buf.data()) == 0x0102030405060708ull);
}

TEST_CASE("load_be48 reads 6-byte ITCH timestamps") {
    // 09:30:00.000000000 = 34200 * 1e9 ns = 0x1F1A_CED9_F000
    const std::array<unsigned char, 6> ts{0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00};
    CHECK(ob::load_be48(ts.data()) == 34'200'000'000'000ull);

    const std::array<unsigned char, 6> max{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(ob::load_be48(max.data()) == 0xFFFF'FFFF'FFFFull);
}

TEST_CASE("Side::opposite") {
    CHECK(ob::opposite(ob::Side::Bid) == ob::Side::Ask);
    CHECK(ob::opposite(ob::Side::Ask) == ob::Side::Bid);
}
