#pragma once

// Shared test helper: builds framed ITCH wire data. Each msg() call starts a
// record, end_msg() back-patches the 2-byte big-endian length prefix.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace obtest {

class Wire {
public:
    Wire& msg() {
        len_at_ = buf_.size();
        buf_.push_back(std::byte{0});
        buf_.push_back(std::byte{0});
        return *this;
    }
    Wire& end_msg() {
        const auto len = buf_.size() - len_at_ - 2;
        buf_[len_at_] = std::byte(len >> 8);
        buf_[len_at_ + 1] = std::byte(len & 0xFF);
        return *this;
    }
    Wire& u8(std::uint8_t v) {
        buf_.push_back(std::byte{v});
        return *this;
    }
    Wire& ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
    Wire& u16(std::uint16_t v) { return be(v, 2); }
    Wire& u32(std::uint32_t v) { return be(v, 4); }
    Wire& u48(std::uint64_t v) { return be(v, 6); }
    Wire& u64(std::uint64_t v) { return be(v, 8); }
    Wire& str(std::string_view s) {
        for (char c : s) ch(c);
        return *this;
    }
    // ITCH header minus the type byte
    Wire& hdr(std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return u16(locate).u16(tracking).u48(ts);
    }
    Wire& raw_u16(std::uint16_t v) {  // for hand-writing bad length prefixes
        return be(v, 2);
    }
    Wire& append(const Wire& other) {
        buf_.insert(buf_.end(), other.buf_.begin(), other.buf_.end());
        return *this;
    }

    // --- whole-message conveniences (header defaults: tracking 0, ts 1000) --
    Wire& sys_event(char event, std::uint16_t locate = 0) {
        return msg().ch('S').hdr(locate, 0, 1000).ch(event).end_msg();
    }
    Wire& add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price,
              std::uint16_t locate = 1, std::string_view stock = "AAPL    ") {
        return msg()
            .ch('A')
            .hdr(locate, 0, 1000)
            .u64(ref)
            .ch(side)
            .u32(shares)
            .str(stock)
            .u32(price)
            .end_msg();
    }
    Wire& exec(std::uint64_t ref, std::uint32_t shares, std::uint64_t match = 1,
               std::uint16_t locate = 1) {
        return msg().ch('E').hdr(locate, 0, 2000).u64(ref).u32(shares).u64(match).end_msg();
    }
    Wire& exec_price(std::uint64_t ref, std::uint32_t shares, char printable,
                     std::uint32_t price, std::uint16_t locate = 1) {
        return msg()
            .ch('C')
            .hdr(locate, 0, 2000)
            .u64(ref)
            .u32(shares)
            .u64(1)
            .ch(printable)
            .u32(price)
            .end_msg();
    }
    Wire& cancel(std::uint64_t ref, std::uint32_t shares, std::uint16_t locate = 1) {
        return msg().ch('X').hdr(locate, 0, 3000).u64(ref).u32(shares).end_msg();
    }
    Wire& del(std::uint64_t ref, std::uint16_t locate = 1) {
        return msg().ch('D').hdr(locate, 0, 3000).u64(ref).end_msg();
    }
    Wire& replace(std::uint64_t orig, std::uint64_t nref, std::uint32_t shares,
                  std::uint32_t price, std::uint16_t locate = 1) {
        return msg()
            .ch('U')
            .hdr(locate, 0, 4000)
            .u64(orig)
            .u64(nref)
            .u32(shares)
            .u32(price)
            .end_msg();
    }
    Wire& trade(std::uint32_t shares, std::uint32_t price, std::uint16_t locate = 1,
                std::string_view stock = "AAPL    ") {
        return msg()
            .ch('P')
            .hdr(locate, 0, 5000)
            .u64(0)
            .ch('B')
            .u32(shares)
            .str(stock)
            .u32(price)
            .u64(99)
            .end_msg();
    }
    Wire& cross(std::uint64_t shares, std::uint32_t price, char cross_type,
                std::uint16_t locate = 1, std::string_view stock = "AAPL    ") {
        return msg()
            .ch('Q')
            .hdr(locate, 0, 6000)
            .u64(shares)
            .str(stock)
            .u32(price)
            .u64(99)
            .ch(cross_type)
            .end_msg();
    }
    Wire& halt(char state, std::uint16_t locate = 1, std::string_view stock = "AAPL    ") {
        return msg().ch('H').hdr(locate, 0, 500).str(stock).ch(state).ch(' ').str("    ").end_msg();
    }

    [[nodiscard]] std::span<const std::byte> bytes() const { return buf_; }

private:
    Wire& be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            buf_.push_back(std::byte((v >> (8 * i)) & 0xFF));
        }
        return *this;
    }
    std::vector<std::byte> buf_;
    std::size_t len_at_ = 0;
};

}  // namespace obtest
