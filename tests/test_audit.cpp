// Execution-at-front audit (§9.3): in-order executions pass, out-of-order
// executions are counted (not asserted), pre-open and halted executions are
// categorised as skips.

#include <catch2/catch_test_macros.hpp>

#include "book/ref_book.hpp"
#include "engine/audit.hpp"
#include "engine/engine.hpp"
#include "itch/parser.hpp"
#include "wire.hpp"

using ob::book::RefBook;
using ob::engine::Engine;
using ob::engine::FrontAudit;
using obtest::Wire;

namespace {

FrontAudit<Engine<RefBook>>::Stats audit_replay(const Wire& w) {
    Engine<RefBook> eng;
    FrontAudit<Engine<RefBook>> audit(eng);
    ob::itch::Parser p(w.bytes());
    p.run(audit);
    return audit.stats();
}

}  // namespace

TEST_CASE("audit: FIFO-respecting executions pass") {
    Wire w;
    w.sys_event('Q');
    w.add(1, 'B', 100, 10000).add(2, 'B', 100, 10000);
    w.exec(1, 100);  // front
    w.exec(2, 50);   // becomes front after 1 is gone
    const auto s = audit_replay(w);
    CHECK(s.checked() == 2);
    CHECK(s.at_front() == 2);
    CHECK(s.pass_rate() == 1.0);
}

TEST_CASE("audit: out-of-order execution is counted, not fatal") {
    Wire w;
    w.sys_event('Q');
    w.add(1, 'B', 100, 10000).add(2, 'B', 100, 10000);
    w.exec(2, 50);  // 1 is still at the front: violation
    const auto s = audit_replay(w);
    CHECK(s.checked() == 1);
    CHECK(s.at_front() == 0);
}

TEST_CASE("audit: pre-open executions are skipped as expected exceptions") {
    Wire w;  // no 'Q': market never opens
    w.add(1, 'B', 100, 10000).add(2, 'B', 100, 10000);
    w.exec(2, 50);
    const auto s = audit_replay(w);
    CHECK(s.checked() == 0);
    CHECK(s.skipped_preopen == 1);
}

TEST_CASE("audit: halted-symbol executions are skipped") {
    Wire w;
    w.sys_event('Q');
    w.add(1, 'B', 100, 10000);
    w.halt('H');
    w.exec(1, 50);
    const auto s = audit_replay(w);
    CHECK(s.checked() == 0);
    CHECK(s.skipped_halted == 1);
}

TEST_CASE("audit: unknown refs (mid-stream start) are skipped") {
    Wire w;
    w.sys_event('Q');
    w.exec(42, 50);
    const auto s = audit_replay(w);
    CHECK(s.checked() == 0);
    CHECK(s.skipped_unknown == 1);
}
