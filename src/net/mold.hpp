#pragma once

// MoldUDP64 framing (DESIGN §11 stretch; spec from nasdaqtrader.com).
//
// Downstream packet:
//   session[10] (ASCII, right-padded) | sequence u64 BE | count u16 BE
//   then `count` blocks of [length u16 BE][message bytes]
// `sequence` numbers the FIRST message in the packet; messages are numbered
// consecutively across the session. count==0 is a heartbeat (sequence =
// next expected); count==0xFFFF ends the session.
//
// Re-request packet (sent by a receiver to the rewinder port): the same
// 20-byte header shape — session | first wanted sequence | wanted count.
//
// Note the block framing matches the ITCH file-dump format exactly, so a
// packet's payload region can be handed straight to itch::Parser.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "core/endian.hpp"

namespace ob::net {

constexpr std::size_t kMoldHeaderSize = 20;
constexpr std::uint16_t kEndOfSession = 0xFFFF;
using Session = std::array<char, 10>;

struct MoldHeader {
    Session session{};
    std::uint64_t seq = 0;
    std::uint16_t count = 0;
};

inline void encode_header(const MoldHeader& h, std::byte* out) noexcept {
    std::memcpy(out, h.session.data(), 10);
    for (int i = 0; i < 8; ++i) {
        out[10 + i] = std::byte(h.seq >> (8 * (7 - i)));
    }
    out[18] = std::byte(h.count >> 8);
    out[19] = std::byte(h.count & 0xFF);
}

inline std::optional<MoldHeader> decode_header(std::span<const std::byte> pkt) noexcept {
    if (pkt.size() < kMoldHeaderSize) return std::nullopt;
    MoldHeader h;
    std::memcpy(h.session.data(), pkt.data(), 10);
    h.seq = load_be<std::uint64_t>(pkt.data() + 10);
    h.count = load_be<std::uint16_t>(pkt.data() + 18);
    return h;
}

// Iterate the message blocks of a packet payload; returns false on any
// malformed framing (a hostile packet must not crash the receiver, §4.1).
template <typename Fn>  // void(std::span<const std::byte> message)
bool for_each_block(std::span<const std::byte> payload, std::uint16_t count, Fn&& fn) {
    std::size_t pos = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + 2 > payload.size()) return false;
        const auto len = load_be<std::uint16_t>(payload.data() + pos);
        if (len == 0 || pos + 2 + len > payload.size()) return false;
        fn(payload.subspan(pos + 2, len));
        pos += 2 + std::size_t{len};
    }
    return pos == payload.size();
}

// Builds MTU-bounded downstream packets; sequence numbers advance
// automatically as messages are added.
class PacketBuilder {
public:
    PacketBuilder(Session session, std::size_t mtu, std::uint64_t first_seq = 1)
        : session_(session), mtu_(mtu), next_seq_(first_seq) {
        buf_.reserve(mtu_);
        begin();
    }

    // False if the message doesn't fit in the current packet (finish first).
    bool add(std::span<const std::byte> msg) {
        if (buf_.size() + 2 + msg.size() > mtu_) return false;
        buf_.push_back(std::byte(msg.size() >> 8));
        buf_.push_back(std::byte(msg.size() & 0xFF));
        buf_.insert(buf_.end(), msg.begin(), msg.end());
        ++count_;
        return true;
    }

    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }

    // Seals the packet: patches header, returns the bytes, starts the next.
    std::span<const std::byte> finish() {
        encode_header({session_, next_seq_, count_}, buf_.data());
        next_seq_ += count_;
        return buf_;
    }
    void begin() {
        buf_.assign(kMoldHeaderSize, std::byte{0});
        count_ = 0;
    }

    [[nodiscard]] std::vector<std::byte> heartbeat() const {
        std::vector<std::byte> hb(kMoldHeaderSize);
        encode_header({session_, next_seq_, 0}, hb.data());
        return hb;
    }
    [[nodiscard]] std::vector<std::byte> end_of_session() const {
        std::vector<std::byte> eos(kMoldHeaderSize);
        encode_header({session_, next_seq_, kEndOfSession}, eos.data());
        return eos;
    }
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

private:
    Session session_;
    std::size_t mtu_;
    std::uint64_t next_seq_;
    std::uint16_t count_ = 0;
    std::vector<std::byte> buf_;
};

// Bounded lookback of sent packets — the rewinder that serves re-requests.
// Real MoldUDP64 retransmission servers have bounded lookback too; requests
// older than the window are refused (counted, never crash).
class RetransmitBuffer {
public:
    explicit RetransmitBuffer(std::size_t max_packets) : max_(max_packets) {}

    void store(std::uint64_t first_seq, std::uint16_t count,
               std::span<const std::byte> packet) {
        ring_.push_back({first_seq, count, {packet.begin(), packet.end()}});
        if (ring_.size() > max_) ring_.pop_front();
    }

    // All stored packets overlapping [from, from+count).
    [[nodiscard]] std::vector<std::span<const std::byte>> lookup(
        std::uint64_t from, std::uint16_t count) const {
        std::vector<std::span<const std::byte>> out;
        const std::uint64_t want_end = from + count;
        for (const auto& p : ring_) {
            const std::uint64_t p_end = p.first_seq + p.count;
            if (p.first_seq < want_end && p_end > from) {
                out.emplace_back(p.bytes);
            }
        }
        return out;
    }

    [[nodiscard]] std::optional<std::uint64_t> oldest_seq() const {
        if (ring_.empty()) return std::nullopt;
        return ring_.front().first_seq;
    }

private:
    struct Stored {
        std::uint64_t first_seq;
        std::uint16_t count;
        std::vector<std::byte> bytes;
    };
    std::size_t max_;
    std::deque<Stored> ring_;
};

}  // namespace ob::net
