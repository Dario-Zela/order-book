// Structured book fuzz target (DESIGN §9.4): interpret fuzz bytes as a
// syntactically-valid-but-hostile op stream against BOTH book
// implementations — unknown refs, double deletes, cancel > remaining,
// duplicate adds, wrong sides, extreme prices. The books must never crash,
// must stay structurally valid, and must agree with each other (mini
// differential inside the fuzzer).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "book/flat_book.hpp"
#include "book/ref_book.hpp"

namespace {

// Tiny deterministic byte cursor over the fuzz input.
struct Cursor {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t i = 0;
    std::uint64_t take(int bytes) {
        std::uint64_t v = 0;
        for (int b = 0; b < bytes && i < n; ++b) v = (v << 8) | p[i++];
        return v;
    }
    [[nodiscard]] bool done() const { return i >= n; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ob::book::BookResources res(256);
    ob::book::FlatBook flat(res, 1,
                            ob::book::BandConfig{.initial_half_width = 8, .max_width = 64});
    ob::book::RefBook ref;
    Cursor c{data, size};

    int ops = 0;
    while (!c.done() && ops < 4096) {
        ++ops;
        const auto op = c.take(1) % 5;
        // Small ref space forces collisions: duplicate adds, reuse-after-
        // delete, unknown refs all emerge from the input itself.
        const auto refid = c.take(1) % 61;
        switch (op) {
            case 0: {
                const auto side = (c.take(1) % 2 == 0) ? ob::Side::Bid : ob::Side::Ask;
                const auto px = static_cast<ob::Price>(c.take(2));  // 0..65535: hits band + overflow
                const auto qty = static_cast<ob::Qty>(c.take(1));   // includes 0
                if (qty == 0) break;  // books require qty>0 by contract (engine filters)
                const auto fe = flat.add(refid, side, px, qty);
                const auto re = ref.add(refid, side, px, qty);
                if (fe.result != re.result) __builtin_trap();
                break;
            }
            case 1: {
                const auto qty = static_cast<ob::Qty>(c.take(2));  // often > remaining
                if (qty == 0) break;
                const auto fe = flat.execute(refid, qty);
                const auto re = ref.execute(refid, qty);
                if (fe.result != re.result || fe.applied != re.applied) __builtin_trap();
                break;
            }
            case 2: {
                const auto qty = static_cast<ob::Qty>(c.take(2));
                if (qty == 0) break;
                const auto fe = flat.cancel(refid, qty);
                const auto re = ref.cancel(refid, qty);
                if (fe.result != re.result || fe.applied != re.applied) __builtin_trap();
                break;
            }
            case 3: {  // delete (double deletes arise naturally)
                const auto fe = flat.remove(refid);
                const auto re = ref.remove(refid);
                if (fe.result != re.result) __builtin_trap();
                break;
            }
            case 4: {  // agreement probes, cheap
                if (flat.live_orders() != ref.live_orders()) __builtin_trap();
                if (flat.best_bid() != ref.best_bid()) __builtin_trap();
                if (flat.best_ask() != ref.best_ask()) __builtin_trap();
                break;
            }
        }
    }
    if (!flat.validate() || !ref.validate()) __builtin_trap();
    if (flat.l2(ob::Side::Bid) != ref.l2(ob::Side::Bid)) __builtin_trap();
    if (flat.l2(ob::Side::Ask) != ref.l2(ob::Side::Ask)) __builtin_trap();
    return 0;
}
