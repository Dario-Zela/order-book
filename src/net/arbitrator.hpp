#pragma once

// A/B feed arbitration + gap handling (DESIGN §11: "sequence-number
// handling is the interesting part"). Socket-free and fully unit-testable;
// the receiver tool wires packets in and re-requests out.
//
// Policy:
// - The next expected sequence initialises from the FIRST packet seen
//   (mid-session join is normal for multicast).
// - Whichever feed delivers the next expected message first wins; the other
//   feed's copy arrives later and is dropped as a duplicate. Per-message
//   dedup falls out of sequence arithmetic — no hashing, no history.
// - A packet from the future opens a gap: it is buffered (bounded) and a
//   re-request for [next_seq, first buffered seq) is surfaced via
//   take_rerequest(). The request re-arms if traffic keeps arriving while
//   the gap stays open, so a lost retransmission is re-asked-for.
// - Overlapping packets apply only their unseen suffix (retransmissions
//   after a partial fill are normal).
// - Heartbeats advance nothing but can REVEAL a gap (seq ahead of us with
//   no data in flight). End-of-session completes only once we are caught
//   up to it.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "net/mold.hpp"

namespace ob::net {

struct Rerequest {
    std::uint64_t from = 0;
    std::uint16_t count = 0;
};

template <typename ApplyMsg>  // void(std::span<const std::byte> message)
class Arbitrator {
public:
    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t messages_applied = 0;
        std::uint64_t duplicates = 0;      // packets fully behind next_seq
        std::uint64_t partial_overlaps = 0;
        std::uint64_t gaps_opened = 0;
        std::uint64_t rerequests = 0;
        std::uint64_t buffered_drops = 0;  // future packets beyond buffer bound
        std::uint64_t malformed = 0;
        bool session_complete = false;
    };

    // ApplyMsg may be a value or a reference type; the cast collapses to a
    // plain bind for references and a move for values.
    explicit Arbitrator(ApplyMsg apply, std::size_t max_buffered = 1024)
        : apply_(static_cast<ApplyMsg&&>(apply)), max_buffered_(max_buffered) {}

    void on_packet(std::span<const std::byte> pkt) {
        const auto h = decode_header(pkt);
        if (!h) {
            ++stats_.malformed;
            return;
        }
        ++stats_.packets;
        if (!started_) {
            started_ = true;
            next_seq_ = h->seq;  // mid-session join: start where the feed is
        }
        if (h->count == kEndOfSession) {
            eos_seq_ = h->seq;
            maybe_complete();
            return;
        }
        if (h->count == 0) {  // heartbeat: no data, but can reveal a gap
            if (h->seq > next_seq_) open_gap(h->seq);
            maybe_complete();
            return;
        }
        const auto payload = pkt.subspan(kMoldHeaderSize);
        if (h->seq > next_seq_) {
            // Future packet: buffer it, ask for what's missing.
            if (buffered_.size() < max_buffered_) {
                buffered_.emplace(h->seq, Buffered{h->count,
                                                   {payload.begin(), payload.end()}});
            } else {
                ++stats_.buffered_drops;  // will be covered by a re-request later
            }
            open_gap(h->seq);
            return;
        }
        apply_packet(h->seq, h->count, payload);
        drain_buffered();
        maybe_complete();
    }

    // The pending re-request, if any (consumed on read; re-arms while the
    // gap persists and further traffic arrives).
    std::optional<Rerequest> take_rerequest() {
        if (!pending_request_) return std::nullopt;
        pending_request_ = false;
        ++stats_.rerequests;
        return armed_;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

private:
    struct Buffered {
        std::uint16_t count;
        std::vector<std::byte> payload;
    };

    void apply_packet(std::uint64_t seq, std::uint16_t count,
                      std::span<const std::byte> payload) {
        const std::uint64_t end = seq + count;
        if (end <= next_seq_) {
            ++stats_.duplicates;
            return;
        }
        const std::uint64_t skip = next_seq_ - seq;  // already-applied prefix
        if (skip > 0) ++stats_.partial_overlaps;
        std::uint64_t idx = 0;
        const bool ok = for_each_block(payload, count, [&](std::span<const std::byte> m) {
            if (idx++ >= skip) {
                apply_(m);
                ++stats_.messages_applied;
            }
        });
        if (!ok) {
            ++stats_.malformed;  // partial application is fine: seq tracks it
            next_seq_ = seq + idx;  // exactly the blocks we managed to walk
            return;
        }
        next_seq_ = end;
        gap_open_ = false;
    }

    void drain_buffered() {
        while (true) {
            // Discard anything now stale, apply anything now contiguous.
            auto it = buffered_.begin();
            if (it == buffered_.end()) return;
            if (it->first + it->second.count <= next_seq_) {
                buffered_.erase(it);
                continue;
            }
            if (it->first > next_seq_) return;  // still a hole in front
            apply_packet(it->first, it->second.count, it->second.payload);
            buffered_.erase(it);
        }
    }

    void open_gap(std::uint64_t seen_ahead) {
        const std::uint64_t hole = seen_ahead - next_seq_;
        const bool fresh = !gap_open_;
        if (fresh) {
            ++stats_.gaps_opened;
            packets_since_rq_ = 0;
        }
        gap_open_ = true;
        armed_.from = next_seq_;
        armed_.count = hole > 0xFFFE ? std::uint16_t{0xFFFE}
                                     : static_cast<std::uint16_t>(hole);
        // Rate limit: request immediately on a fresh gap, then re-ask only
        // every kRerequestInterval packets while it persists — a stuck
        // receiver must not turn into a re-request storm (measured: 260k
        // requests in one bad run before this guard existed).
        if (fresh || ++packets_since_rq_ >= kRerequestInterval) {
            packets_since_rq_ = 0;
            pending_request_ = true;
        }
    }

    void maybe_complete() {
        if (eos_seq_ && next_seq_ >= *eos_seq_) stats_.session_complete = true;
    }

    static constexpr std::uint32_t kRerequestInterval = 256;

    ApplyMsg apply_;
    std::size_t max_buffered_;
    std::map<std::uint64_t, Buffered> buffered_;
    std::uint64_t next_seq_ = 0;
    bool started_ = false;
    bool gap_open_ = false;
    bool pending_request_ = false;
    std::uint32_t packets_since_rq_ = 0;
    Rerequest armed_{};
    std::optional<std::uint64_t> eos_seq_;
    Stats stats_;
};

}  // namespace ob::net
