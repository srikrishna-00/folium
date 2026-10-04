// SPDX-License-Identifier: Apache-2.0
//
// tests/sync_spike/fugue_spike_test.cpp — the four-property test plan of
// docs/sync-architecture.md section 6.2, run against core/sync/spike.
//
// SPIKE TEST. The code under test is research code that no product target
// links; see core/sync/spike/include/uapp/sync/spike/element.h.
//
// ===========================================================================
// THE FOUR PROPERTIES, AND WHICH CASE PROVES WHICH
// ===========================================================================
//   P1 convergence     -> property.random.*  (several valid causal orders of
//                         the same log must give byte-identical documents AND
//                         byte-identical tombstone-inclusive orders)
//   P2 idempotence     -> property.random.*  (the same log delivered twice)
//   P3 commutativity   -> property.commutativity.* (a concurrent pair applied
//                         in both orders from the same causally complete state)
//   P4 non-interleaving-> noninterleaving.*  (dedicated adversarial
//                         generators, plus the RGA control that proves the
//                         detector can fail)
//
// Two further oracles run on every random trial, because SP-6 F-1 is that
// P1-P3 all pass on a corrupted document:
//   * differential: the independent reference model must produce the same
//     order (ADR-0002's "reference-model differential testing");
//   * specification: the merged order must be a linear extension of the
//     constraints the generating replicas actually imposed (Attiya et al.
//     strong list specification condition (b)).
//
// Trial counts are printed, not just asserted, so the findings document can
// quote measured numbers. Override with UAPP_FUGUE_TRIALS.

#include <cstdio>
#include <string>
#include <vector>

#include "property_harness.h"
#include "test_framework.h"
#include "uapp/sync/spike/rga.h"

// File-scope, not inside the anonymous namespace below, so that every test
// case in this translation unit sees them.
using namespace uapp::spike_test;  // NOLINT(google-build-using-namespace) — test-local
using uapp::sync::spike::maximal_run_count;
using uapp::sync::spike::reference_order;
using uapp::sync::spike::ReferenceOrder;
using uapp::sync::spike::RgaOperation;
using uapp::sync::spike::RgaReplica;
using uapp::sync::spike::satisfies_strong_list_spec;

namespace {

std::string ids_to_string(const std::vector<ElementId>& ids) {
    std::string out;
    char        buf[32];
    for (const ElementId& id : ids) {
        std::snprintf(buf, sizeof(buf), "(%u,%u)", id.replica, id.counter);
        out += buf;
    }
    return out;
}

/// Everything a single random trial has to satisfy. Returns an empty string on
/// success, or the first failure's description.
std::string check_trial(const History& history, Variant variant, Prng& rng,
                        std::size_t causal_orders) {
    char buf[512];

    if (history.ops.empty()) { return std::string(); }

    // --- P1 + P3: several valid causal delivery orders must agree ----------
    ReplayResult baseline;
    for (std::size_t attempt = 0; attempt < causal_orders; ++attempt) {
        const std::vector<std::uint32_t> order = random_causal_order(history, rng);
        if (order.size() != history.ops.size()) {
            return "dependency DAG is not acyclic: topological sort was incomplete";
        }
        const ReplayResult r =
            replay(history, order, variant, static_cast<ReplicaId>(900u + attempt));
        if (!is_ok(r.status)) {
            std::snprintf(buf, sizeof(buf), "replay failed: %s", spike_status_code(r.status));
            return buf;
        }
        if (r.pending != 0) {
            std::snprintf(buf, sizeof(buf), "%zu operations still buffered after a full "
                                            "causal delivery", r.pending);
            return buf;
        }
        if (attempt == 0) {
            baseline = r;
            continue;
        }
        if (r.text != baseline.text) {
            std::snprintf(buf, sizeof(buf),
                          "P1 convergence: delivery order %zu gave \"%s\", order 0 gave \"%s\"",
                          attempt, r.text.c_str(), baseline.text.c_str());
            return buf;
        }
        if (r.order != baseline.order) {
            // Same visible text, different tombstone placement. This is the
            // failure a text-only convergence check cannot see.
            return "P1 convergence: identical text but a different "
                   "tombstone-inclusive element order";
        }
    }

    // --- P2: the same log delivered twice changes nothing ------------------
    {
        const std::vector<std::uint32_t> order = random_causal_order(history, rng);
        const ReplayResult twice = replay(history, order, variant, 950u, 2);
        if (!is_ok(twice.status)) {
            std::snprintf(buf, sizeof(buf), "double delivery failed: %s",
                          spike_status_code(twice.status));
            return buf;
        }
        if (twice.text != baseline.text || twice.order != baseline.order) {
            std::snprintf(buf, sizeof(buf),
                          "P2 idempotence: second delivery changed \"%s\" into \"%s\"",
                          baseline.text.c_str(), twice.text.c_str());
            return buf;
        }
    }

    // --- differential: the independent reference model must agree ----------
    {
        const ReferenceOrder ref = reference_order(history.ops, variant);
        if (!ref.ok) {
            std::snprintf(buf, sizeof(buf), "reference model refused to answer: %s",
                          ref.diagnostic.c_str());
            return buf;
        }
        if (ref.text != baseline.text) {
            std::snprintf(buf, sizeof(buf),
                          "DIFFERENTIAL: implementation gave \"%s\", reference model gave \"%s\"",
                          baseline.text.c_str(), ref.text.c_str());
            return buf;
        }
        if (ref.order != baseline.order) {
            std::string detail = "DIFFERENTIAL: same text, different element order. impl=";
            detail += ids_to_string(baseline.order);
            detail += " ref=";
            detail += ids_to_string(ref.order);
            return detail;
        }
    }

    // --- specification: is the agreed-on order actually the right one? -----
    {
        std::string why;
        if (!satisfies_strong_list_spec(history.witnesses, baseline.order, why)) {
            return "SPECIFICATION: " + why;
        }
    }

    return std::string();
}

}  // namespace

// ===========================================================================
// Paper fidelity and plumbing
// ===========================================================================

UAPP_TEST("fugue.basic: sequential typing reads back as typed") {
    Session s(Variant::Fugue, 1);
    UAPP_REQUIRE(is_ok(s.type_forward(0, 0, "hello world")));
    UAPP_CHECK_MSG(s.replica(0).text() == "hello world", s.replica(0).text());
    UAPP_CHECK(s.replica(0).visible_size() == 11);
}

UAPP_TEST("fugue.basic: right-to-left typing reads back in reading order") {
    Session s(Variant::Fugue, 1);
    UAPP_REQUIRE(is_ok(s.type_backward(0, 0, "bread")));
    UAPP_CHECK_MSG(s.replica(0).text() == "bread", s.replica(0).text());
}

UAPP_TEST("fugue.basic: delete tombstones but keeps the position usable") {
    Session s(Variant::Fugue, 1);
    UAPP_REQUIRE(is_ok(s.type_forward(0, 0, "abcd")));
    UAPP_REQUIRE(is_ok(s.local_erase(0, 1)));  // remove 'b'
    UAPP_CHECK_MSG(s.replica(0).text() == "acd", s.replica(0).text());
    UAPP_CHECK(s.replica(0).visible_size() == 3);
    // The tombstone is still a tree node: four elements, three visible.
    UAPP_CHECK(s.replica(0).element_count() == 4);
    // Inserting at the tombstone's old neighbourhood still works, and the
    // tombstone-inclusive right-origin rule (Algorithm 1 line 24) is what makes
    // the result deterministic.
    UAPP_REQUIRE(is_ok(s.local_insert(0, 1, 'B')));
    UAPP_CHECK_MSG(s.replica(0).text() == "aBcd", s.replica(0).text());
}

UAPP_TEST("fugue.basic: index out of range is rejected, not clamped") {
    Session s(Variant::Fugue, 1);
    UAPP_REQUIRE(is_ok(s.type_forward(0, 0, "ab")));
    UAPP_CHECK(s.local_insert(0, 3, 'x') == SpikeStatus::IndexOutOfRange);
    UAPP_CHECK(s.local_erase(0, 2) == SpikeStatus::IndexOutOfRange);
    // Appending at exactly the visible size IS legal.
    UAPP_CHECK(is_ok(s.local_insert(0, 2, 'c')));
    UAPP_CHECK_MSG(s.replica(0).text() == "abc", s.replica(0).text());
}

UAPP_TEST("fugue.boundary: structurally impossible operations are refused") {
    FugueReplica rep(1u, Variant::Fugue);

    Operation null_id;
    null_id.kind = OpKind::Insert;
    null_id.id   = ElementId{0u, 0u};
    UAPP_CHECK(rep.apply(null_id) == SpikeStatus::OpMalformed);

    Operation self_parent;
    self_parent.kind   = OpKind::Insert;
    self_parent.id     = ElementId{7u, 1u};
    self_parent.parent = ElementId{7u, 1u};
    UAPP_CHECK(rep.apply(self_parent) == SpikeStatus::OpMalformed);

    // Nothing can be ordered before the root: it is the first node of the
    // traversal, so a Left child of it is not a position the algorithm can
    // produce and accepting one would let two replicas disagree.
    Operation left_of_root;
    left_of_root.kind   = OpKind::Insert;
    left_of_root.id     = ElementId{7u, 2u};
    left_of_root.parent = uapp::sync::spike::kNullId;
    left_of_root.side   = uapp::sync::spike::Side::Left;
    UAPP_CHECK(rep.apply(left_of_root) == SpikeStatus::OpMalformed);

    UAPP_CHECK(rep.element_count() == 0);
}

UAPP_TEST("fugue.boundary: the causal-delivery buffer is bounded") {
    FugueReplica rep(1u, Variant::Fugue);

    // Every one of these names a parent that is never delivered, so every one
    // is held. The bound must stop the growth rather than let a peer pin
    // memory (docs/sync-architecture.md section 6.2).
    SpikeStatus last = SpikeStatus::Ok;
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < uapp::sync::spike::kMaxPendingOperations + 16u; ++i) {
        Operation orphan;
        orphan.kind   = OpKind::Insert;
        orphan.id     = ElementId{2u, static_cast<std::uint32_t>(i + 1u)};
        orphan.parent = ElementId{3u, static_cast<std::uint32_t>(i + 1u)};
        orphan.value  = 'z';
        last          = rep.apply(orphan);
        if (!is_ok(last)) { break; }
        ++accepted;
    }
    UAPP_CHECK(last == SpikeStatus::PendingBufferFull);
    UAPP_CHECK_MSG(accepted == uapp::sync::spike::kMaxPendingOperations,
                   std::string("accepted ") + std::to_string(accepted));
    UAPP_CHECK(rep.element_count() == 0);
    UAPP_CHECK(rep.text().empty());
}

UAPP_TEST("fugue.cost: sequentially typed text degenerates into a spine") {
    // NOT a nice-to-have measurement: Algorithm 1's traversal is written as
    // recursion, and this is why a direct transcription of it cannot ship.
    // Typing n characters left to right makes every character the right child
    // of the one before it, so tree depth == n.
    // Uses FugueReplica directly rather than Session: no history, no witnesses
    // and no delivery schedule are needed, and recording them for 4000 inserts
    // would make this measurement a memory test instead.
    const std::size_t n = 4000;

    FugueReplica forward(1u, Variant::Fugue);
    for (std::size_t i = 0; i < n; ++i) {
        Operation op;
        UAPP_REQUIRE(is_ok(forward.insert(i, 'x', op)));
    }
    UAPP_CHECK_MSG(forward.tree_depth() == n,
                   std::string("depth ") + std::to_string(forward.tree_depth()) + " for " +
                       std::to_string(n) + " sequential inserts");
    UAPP_CHECK(forward.text().size() == n);

    // Backward typing is just as degenerate, down the left children instead.
    FugueReplica backward(2u, Variant::Fugue);
    for (std::size_t i = 0; i < n; ++i) {
        Operation op;
        UAPP_REQUIRE(is_ok(backward.insert(0, 'y', op)));
    }
    UAPP_CHECK_MSG(backward.tree_depth() == n,
                   std::string("backward depth ") + std::to_string(backward.tree_depth()));
}

// ===========================================================================
// The RGA control. Calibrated against the Fugue paper's own appendix, so the
// detector and the scenarios are checked against the literature rather than
// against our expectations.
// ===========================================================================

UAPP_TEST("rga.control: paper appendix A.1.8 reproduces exactly, as axb") {
    // "replica A generates Insert(1,b) followed by prepending Insert(1,a),
    //  while concurrently replica B generates Insert(1,x) ... resulting in the
    //  document axb."
    RgaReplica a(1u);
    RgaReplica b(2u);

    RgaOperation op_b;
    RgaOperation op_a;
    RgaOperation op_x;
    UAPP_REQUIRE(is_ok(a.insert(0, 'b', op_b)));
    UAPP_REQUIRE(is_ok(a.insert(0, 'a', op_a)));
    UAPP_REQUIRE(is_ok(b.insert(0, 'x', op_x)));

    UAPP_REQUIRE(is_ok(a.apply(op_x)));
    UAPP_REQUIRE(is_ok(b.apply(op_b)));
    UAPP_REQUIRE(is_ok(b.apply(op_a)));

    UAPP_CHECK_MSG(a.text() == "axb", std::string("A sees \"") + a.text() + "\"");
    UAPP_CHECK_MSG(b.text() == "axb", std::string("B sees \"") + b.text() + "\"");
}

UAPP_TEST("rga.control: forward insertion does NOT interleave, as published") {
    // Table 1 of the paper marks RGA forward-interleaving-free (proven). If
    // this case failed, the RGA model would be wrong in a way that would make
    // the backward comparison meaningless.
    RgaReplica a(1u);
    RgaReplica b(2u);
    RgaOperation op1;
    RgaOperation op2;
    RgaOperation op3;
    UAPP_REQUIRE(is_ok(a.insert(0, 'a', op1)));
    UAPP_REQUIRE(is_ok(a.insert(1, 'b', op2)));
    UAPP_REQUIRE(is_ok(b.insert(0, 'x', op3)));
    UAPP_REQUIRE(is_ok(a.apply(op3)));
    UAPP_REQUIRE(is_ok(b.apply(op1)));
    UAPP_REQUIRE(is_ok(b.apply(op2)));
    UAPP_CHECK_MSG(a.text() == "xab", std::string("A sees \"") + a.text() + "\"");
    UAPP_CHECK(a.text() == b.text());
    UAPP_CHECK(a.text() != "axb");
}

UAPP_TEST("rga.control: the backward shopping-list shape interleaves, and the "
          "detector fires") {
    // The paper's Figure 2 shape, reduced to single characters: both users
    // PREPEND a three-item block after a shared first element.
    RgaReplica one(1u);
    RgaReplica two(2u);
    std::vector<RgaOperation> log;

    RgaOperation base;
    UAPP_REQUIRE(is_ok(one.insert(0, 'S', base)));
    log.push_back(base);
    UAPP_REQUIRE(is_ok(two.apply(base)));

    const std::string first  = "abc";
    const std::string second = "XYZ";
    for (std::size_t i = first.size(); i > 0; --i) {
        RgaOperation op;
        UAPP_REQUIRE(is_ok(one.insert(1, first[i - 1u], op)));
        log.push_back(op);
    }
    std::vector<RgaOperation> log_two;
    for (std::size_t i = second.size(); i > 0; --i) {
        RgaOperation op;
        UAPP_REQUIRE(is_ok(two.insert(1, second[i - 1u], op)));
        log_two.push_back(op);
    }
    UAPP_REQUIRE_MSG(one.text() == "Sabc", one.text());
    UAPP_REQUIRE_MSG(two.text() == "SXYZ", two.text());

    for (const RgaOperation& op : log_two) { UAPP_REQUIRE(is_ok(one.apply(op))); }
    for (std::size_t i = 1; i < log.size(); ++i) { UAPP_REQUIRE(is_ok(two.apply(log[i]))); }

    // Converged (so P1/P2/P3 would all pass) and corrupt.
    UAPP_CHECK_MSG(one.text() == two.text(), one.text() + " vs " + two.text());
    UAPP_CHECK_MSG(one.text() == "SXaYbZc", std::string("RGA merged to \"") + one.text() + "\"");

    // And the P4 detector must report the corruption. Without this assertion
    // the Fugue side of the comparison proves nothing.
    UAPP_CHECK_MSG(maximal_run_count(one.text(), first) == 3,
                   std::string("runs of \"abc\" in \"") + one.text() + "\": " +
                       std::to_string(maximal_run_count(one.text(), first)));
    UAPP_CHECK(maximal_run_count(one.text(), second) == 3);
}

// ===========================================================================
// P4 — non-interleaving. The property Fugue exists for.
// ===========================================================================

UAPP_TEST("noninterleaving.backward: the exact shape that breaks RGA does not "
          "break Fugue") {
    // Same operations as rga.control's A.1.8 case, under Fugue. RGA gives
    // "axb"; Fugue must not.
    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        Session s(variant, 2);
        UAPP_REQUIRE(is_ok(s.local_insert(0, 0, 'b')));
        UAPP_REQUIRE(is_ok(s.local_insert(0, 0, 'a')));  // prepend: backward
        UAPP_REQUIRE(is_ok(s.local_insert(1, 0, 'x')));  // concurrent
        UAPP_REQUIRE(is_ok(s.sync_all()));

        const std::string merged = s.replica(0).text();
        UAPP_CHECK_MSG(merged == s.replica(1).text(), merged + " vs " + s.replica(1).text());
        UAPP_CHECK_MSG(merged != "axb", std::string("Fugue interleaved: \"") + merged + "\"");
        UAPP_CHECK_MSG(merged == "abx" || merged == "xab",
                       std::string("unexpected merge \"") + merged + "\"");
        UAPP_CHECK(maximal_run_count(merged, "ab") == 1);
    }
}

UAPP_TEST("noninterleaving.backward: the shopping-list shape stays contiguous") {
    // The same scenario rga.control merged into "SXaYbZc".
    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        Session s(variant, 2);
        UAPP_REQUIRE(is_ok(s.local_insert(0, 0, 'S')));
        UAPP_REQUIRE(is_ok(s.sync_to(1)));
        UAPP_REQUIRE(is_ok(s.type_backward(0, 1, "abc")));
        UAPP_REQUIRE(is_ok(s.type_backward(1, 1, "XYZ")));
        UAPP_REQUIRE_MSG(s.replica(0).text() == "Sabc", s.replica(0).text());
        UAPP_REQUIRE_MSG(s.replica(1).text() == "SXYZ", s.replica(1).text());
        UAPP_REQUIRE(is_ok(s.sync_all()));

        const std::string merged = s.replica(0).text();
        UAPP_CHECK_MSG(merged == s.replica(1).text(), merged + " vs " + s.replica(1).text());
        UAPP_CHECK_MSG(maximal_run_count(merged, "abc") == 1,
                       std::string("merged \"") + merged + "\" splits \"abc\" into " +
                           std::to_string(maximal_run_count(merged, "abc")) + " runs");
        UAPP_CHECK_MSG(maximal_run_count(merged, "XYZ") == 1,
                       std::string("merged \"") + merged + "\"");
        UAPP_CHECK_MSG(merged == "SabcXYZ" || merged == "SXYZabc",
                       std::string("unexpected merge \"") + merged + "\"");
    }
}

UAPP_TEST("noninterleaving.forward: two users typing at the same position") {
    // The paper's Figure 1: "milk\n" then two users each add a word at the
    // same place. RGA survives this one; it is here because P4 must cover both
    // directions, and because it is the case users hit most often.
    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        Session s(variant, 2);
        UAPP_REQUIRE(is_ok(s.type_forward(0, 0, "milk.")));
        UAPP_REQUIRE(is_ok(s.sync_to(1)));
        UAPP_REQUIRE(is_ok(s.type_forward(0, 5, "eggs")));
        UAPP_REQUIRE(is_ok(s.type_forward(1, 5, "BREAD")));
        UAPP_REQUIRE(is_ok(s.sync_all()));

        const std::string merged = s.replica(0).text();
        UAPP_CHECK(merged == s.replica(1).text());
        UAPP_CHECK_MSG(maximal_run_count(merged, "eggs") == 1, merged);
        UAPP_CHECK_MSG(maximal_run_count(merged, "BREAD") == 1, merged);
        UAPP_CHECK_MSG(merged == "milk.eggsBREAD" || merged == "milk.BREADeggs", merged);
    }
}

UAPP_TEST("noninterleaving.backward: the three-replica shape (Yjs fails here)") {
    // Paper appendix A.1.2 / A.1.9: replica C inserts 'b'; replica A, having
    // received it, prepends 'a'; replica B concurrently inserts 'x'. This is
    // the multi-replica column of Table 1, where Yjs interleaves and Fugue is
    // proven not to.
    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        Session s(variant, 3);
        UAPP_REQUIRE(is_ok(s.local_insert(2, 0, 'b')));  // replica 3 == "C"
        UAPP_REQUIRE(is_ok(s.sync_to(0)));               // replica 1 == "A" receives it
        UAPP_REQUIRE(is_ok(s.local_insert(0, 0, 'a')));  // A prepends
        UAPP_REQUIRE(is_ok(s.local_insert(1, 0, 'x')));  // B, having seen nothing
        UAPP_REQUIRE(is_ok(s.sync_all()));

        const std::string merged = s.replica(0).text();
        UAPP_CHECK(merged == s.replica(1).text());
        UAPP_CHECK(merged == s.replica(2).text());
        UAPP_CHECK_MSG(merged != "axb", std::string("interleaved: \"") + merged + "\"");
        UAPP_CHECK_MSG(merged == "xab" || merged == "abx",
                       std::string("unexpected merge \"") + merged + "\"");
    }
}

UAPP_TEST("noninterleaving.random: randomised two-replica token scenarios") {
    // The dedicated P4 generator SP-6 R-2 asks for. Each trial: a random base
    // document, a random shared insertion position, and two replicas each
    // writing a random contiguous token there, each in a randomly chosen
    // DIRECTION (forward or backward). Both tokens must survive as single runs.
    //
    // Two replicas only, and strictly one token each: that is the case the
    // paper PROVES non-interleaving for (Table 1, all three columns). Asserting
    // contiguity for arbitrary multi-replica interacting edits would be
    // asserting something no algorithm can satisfy (paper section 5.3), and a
    // test that demands the impossible is worse than no test.
    const std::size_t trials = trial_count(4000);
    std::size_t       checked      = 0;
    std::size_t       backward_any = 0;

    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        for (std::size_t t = 0; t < trials; ++t) {
            Prng rng(0x5EEDu + t * 7919u + (variant == Variant::FugueMax ? 1000003u : 0u));

            const std::size_t base_len  = rng.below(7);
            const std::size_t token1    = 1u + rng.below(5);
            const std::size_t token2    = 1u + rng.below(5);
            const bool        backward1 = rng.below(2) == 1;
            const bool        backward2 = rng.below(2) == 1;
            if (backward1 || backward2) { ++backward_any; }

            Session s(variant, 2);
            std::string base;
            for (std::size_t i = 0; i < base_len; ++i) {
                base.push_back(static_cast<char>('0' + static_cast<int>(rng.below(10))));
            }
            if (!base.empty()) {
                UAPP_REQUIRE(is_ok(s.type_forward(0, 0, base)));
                UAPP_REQUIRE(is_ok(s.sync_to(1)));
            }
            const std::size_t at = rng.below(base_len + 1u);

            std::string tok1;
            for (std::size_t i = 0; i < token1; ++i) {
                tok1.push_back(static_cast<char>('a' + static_cast<int>(rng.below(8))));
            }
            std::string tok2;
            for (std::size_t i = 0; i < token2; ++i) {
                tok2.push_back(static_cast<char>('A' + static_cast<int>(rng.below(8))));
            }

            UAPP_REQUIRE(is_ok(backward1 ? s.type_backward(0, at, tok1)
                                         : s.type_forward(0, at, tok1)));
            UAPP_REQUIRE(is_ok(backward2 ? s.type_backward(1, at, tok2)
                                         : s.type_forward(1, at, tok2)));
            UAPP_REQUIRE(is_ok(s.sync_all()));

            const std::string merged = s.replica(0).text();
            UAPP_REQUIRE_MSG(merged == s.replica(1).text(), merged + " vs " + s.replica(1).text());

            const std::size_t runs1 = maximal_run_count(merged, "abcdefgh");
            const std::size_t runs2 = maximal_run_count(merged, "ABCDEFGH");
            UAPP_REQUIRE_MSG(runs1 == 1 && runs2 == 1,
                             std::string("P4 FAILED: merged \"") + merged + "\" splits \"" +
                                 tok1 + "\" into " + std::to_string(runs1) + " run(s) and \"" +
                                 tok2 + "\" into " + std::to_string(runs2) +
                                 " run(s); backward1=" + std::to_string(backward1 ? 1 : 0) +
                                 " backward2=" + std::to_string(backward2 ? 1 : 0));
            ++checked;
        }
    }
    std::printf("       P4: %zu two-replica token scenarios (both variants), "
                "%zu involving a backward-built token\n",
                checked, backward_any);
}

UAPP_TEST("noninterleaving.measured: unordered within-token typing is NOT "
          "guaranteed, and here is how often it interleaves") {
    // An honest measurement rather than an assertion. The paper's guarantee
    // covers tokens built in one direction. Real editing is messier: a user
    // fixes a typo in the middle of a word they just typed. This case builds
    // each token by inserting its characters in a RANDOM order of positions and
    // reports the interleaving rate instead of demanding zero, because
    // demanding zero here would be demanding something the paper proves is
    // sometimes impossible.
    const std::size_t trials      = trial_count(4000);
    std::size_t       interleaved = 0;
    std::size_t       done        = 0;

    for (std::size_t t = 0; t < trials; ++t) {
        Prng rng(0xA11CEu + t * 104729u);

        Session s(Variant::Fugue, 2);
        UAPP_REQUIRE(is_ok(s.type_forward(0, 0, "00")));
        UAPP_REQUIRE(is_ok(s.sync_to(1)));

        const std::size_t len1 = 2u + rng.below(4);
        const std::size_t len2 = 2u + rng.below(4);
        for (std::size_t i = 0; i < len1; ++i) {
            const char c = static_cast<char>('a' + static_cast<int>(rng.below(8)));
            UAPP_REQUIRE(is_ok(s.local_insert(0, 1u + rng.below(i + 1u), c)));
        }
        for (std::size_t i = 0; i < len2; ++i) {
            const char c = static_cast<char>('A' + static_cast<int>(rng.below(8)));
            UAPP_REQUIRE(is_ok(s.local_insert(1, 1u + rng.below(i + 1u), c)));
        }
        UAPP_REQUIRE(is_ok(s.sync_all()));

        const std::string merged = s.replica(0).text();
        UAPP_REQUIRE(merged == s.replica(1).text());

        // Still has to satisfy the specification, whatever the interleaving.
        std::string why;
        UAPP_REQUIRE_MSG(
            satisfies_strong_list_spec(s.history().witnesses, s.replica(0).traversal_ids(), why),
            why);

        if (maximal_run_count(merged, "abcdefgh") != 1 ||
            maximal_run_count(merged, "ABCDEFGH") != 1) {
            ++interleaved;
        }
        ++done;
    }
    std::printf("       P4 (measured, not asserted): %zu/%zu unordered-typing trials "
                "interleaved (%.2f%%)\n",
                interleaved, done,
                done == 0 ? 0.0 : 100.0 * static_cast<double>(interleaved) /
                                      static_cast<double>(done));
}

// ===========================================================================
// P3 — commutativity, isolated.
// ===========================================================================

UAPP_TEST("property.commutativity: a concurrent pair applied in both orders") {
    const std::size_t trials   = trial_count(3000);
    std::size_t       examined = 0;

    for (const Variant variant : {Variant::Fugue, Variant::FugueMax}) {
        for (std::size_t t = 0; t < trials; ++t) {
            GenConfig config;
            config.replicas = 3;
            config.steps    = 22;
            config.variant  = variant;
            const History h = generate_history(config, 0xC0FFEEu + t * 2654435761u);
            if (h.ops.size() < 2) { continue; }

            // A pair is usable when both are MAXIMAL (nothing depends on
            // either), so the rest of the log is causally closed without them,
            // and CONCURRENT (neither is in the other's dependency set).
            std::vector<bool> depended_on(h.ops.size(), false);
            for (const std::vector<std::uint32_t>& dep : h.deps) {
                for (const std::uint32_t d : dep) { depended_on[d] = true; }
            }
            std::size_t first  = h.ops.size();
            std::size_t second = h.ops.size();
            for (std::size_t i = 0; i < h.ops.size() && second == h.ops.size(); ++i) {
                if (depended_on[i]) { continue; }
                for (std::size_t j = i + 1; j < h.ops.size(); ++j) {
                    if (depended_on[j]) { continue; }
                    bool i_in_j = false;
                    for (const std::uint32_t d : h.deps[j]) {
                        if (d == i) {
                            i_in_j = true;
                            break;
                        }
                    }
                    if (i_in_j) { continue; }
                    first  = i;
                    second = j;
                    break;
                }
            }
            if (second == h.ops.size()) { continue; }

            FugueReplica ab(800u, variant);
            FugueReplica ba(801u, variant);
            for (std::size_t k = 0; k < h.ops.size(); ++k) {
                if (k == first || k == second) { continue; }
                UAPP_REQUIRE(is_ok(ab.apply(h.ops[k])));
                UAPP_REQUIRE(is_ok(ba.apply(h.ops[k])));
            }
            UAPP_REQUIRE(ab.pending_count() == 0);

            UAPP_REQUIRE(is_ok(ab.apply(h.ops[first])));
            UAPP_REQUIRE(is_ok(ab.apply(h.ops[second])));
            UAPP_REQUIRE(is_ok(ba.apply(h.ops[second])));
            UAPP_REQUIRE(is_ok(ba.apply(h.ops[first])));

            UAPP_REQUIRE_MSG(ab.text() == ba.text(),
                             std::string("P3 FAILED on seed ") + std::to_string(h.seed) + ": \"" +
                                 ab.text() + "\" vs \"" + ba.text() + "\"");
            UAPP_REQUIRE_MSG(ab.traversal_ids() == ba.traversal_ids(),
                             std::string("P3 FAILED (tombstone order) on seed ") +
                                 std::to_string(h.seed));
            ++examined;
        }
    }
    std::printf("       P3: %zu concurrent pairs applied in both orders\n", examined);
}

// ===========================================================================
// P1 / P2 + differential + specification, over randomised histories.
// ===========================================================================

namespace {

void run_random_property_suite(::uapp::test::CaseResult& uapp_test_result, const char* label,
                              Variant variant, bool prepend_bias, std::size_t delete_percent,
                              std::size_t trials, std::uint64_t seed_base) {
    std::size_t total_ops   = 0;
    std::size_t total_dels  = 0;
    std::size_t trials_run  = 0;

    for (std::size_t t = 0; t < trials; ++t) {
        GenConfig config;
        config.replicas       = 2u + (t % 4u);  // 2..5 replicas
        config.steps          = 14u + (t % 23u);
        config.variant        = variant;
        config.prepend_bias   = prepend_bias;
        config.delete_percent = delete_percent;

        const std::uint64_t seed = seed_base + t * 6364136223846793005ull;
        const History       h    = generate_history(config, seed);
        total_ops += h.ops.size();
        for (const Operation& op : h.ops) {
            if (op.kind == OpKind::Delete) { ++total_dels; }
        }

        Prng              rng(seed ^ 0xDEADBEEFull);
        const std::string failure = check_trial(h, variant, rng, 3);
        UAPP_REQUIRE_MSG(failure.empty(), std::string("seed ") + std::to_string(seed) + " (" +
                                              label + "): " + failure);
        ++trials_run;
    }
    std::printf("       %s: %zu trials, %zu operations (%zu deletes), "
                "3 causal delivery orders + 1 double delivery each\n",
                label, trials_run, total_ops, total_dels);
}

}  // namespace

UAPP_TEST("property.random.fugue: convergence, idempotence, differential, spec") {
    run_random_property_suite(uapp_test_result, "Fugue/uniform", Variant::Fugue, false, 18,
                              trial_count(5000), 0x1234ull);
}

UAPP_TEST("property.random.fugue.prepend: the same, with backward-biased edits") {
    // SP-6 R-2's point: the uniform generator above essentially never builds
    // the right-to-left shape, so a second generator exists purely to make
    // backward construction common.
    run_random_property_suite(uapp_test_result, "Fugue/prepend-biased", Variant::Fugue, true, 18,
                              trial_count(5000), 0x9ABCull);
}

UAPP_TEST("property.random.fugue.tombstones: the same, with a 45% delete rate") {
    // Added because mutation testing demanded it, not for symmetry. Breaking
    // Algorithm 1 line 24 (the right origin must be the TOMBSTONE-INCLUSIVE
    // successor, not the visible one) went undetected by 500 trials of the
    // 18%-delete generator above and is caught by this one in tens. See
    // docs/spikes/fugue-correctness-spike.md, "What the mutation tests found".
    run_random_property_suite(uapp_test_result, "Fugue/tombstone-heavy", Variant::Fugue, true, 45,
                              trial_count(5000), 0x7E57ull);
}

UAPP_TEST("property.random.fuguemax: convergence, idempotence, differential, spec") {
    run_random_property_suite(uapp_test_result, "FugueMax/uniform", Variant::FugueMax, false, 18,
                              trial_count(5000), 0x55AAull);
}

UAPP_TEST("property.random.fuguemax.prepend: the same, backward-biased") {
    run_random_property_suite(uapp_test_result, "FugueMax/prepend-biased", Variant::FugueMax, true,
                              18, trial_count(5000), 0xF00Dull);
}

UAPP_TEST("property.random.fuguemax.tombstones: the same, with a 45% delete rate") {
    run_random_property_suite(uapp_test_result, "FugueMax/tombstone-heavy", Variant::FugueMax,
                              true, 45, trial_count(5000), 0xBEADull);
}

UAPP_TEST("property.noncausal: a non-causal delivery order still converges") {
    // docs/sync-architecture.md section 6.2: "the implementation BUFFERS; it
    // never assumes". The log is delivered in a plain shuffle that ignores
    // causality, and the result must match a causal delivery of the same log
    // exactly — not merely "not crash".
    const std::size_t trials = trial_count(2000);
    std::size_t       matched = 0;

    for (std::size_t t = 0; t < trials; ++t) {
        GenConfig config;
        config.replicas = 2u + (t % 4u);
        config.steps    = 16u + (t % 17u);
        const std::uint64_t seed = 0xBEEF0000ull + t * 2862933555777941757ull;
        const History       h    = generate_history(config, seed);
        if (h.ops.empty()) { continue; }

        Prng rng(seed ^ 0x5151ull);
        const ReplayResult causal =
            replay(h, random_causal_order(h, rng), Variant::Fugue, 700u);
        UAPP_REQUIRE(is_ok(causal.status));

        const ReplayResult shuffled =
            replay(h, random_shuffle_order(h.ops.size(), rng), Variant::Fugue, 701u);
        UAPP_REQUIRE_MSG(is_ok(shuffled.status),
                         std::string("seed ") + std::to_string(seed) + ": " +
                             spike_status_code(shuffled.status));
        UAPP_REQUIRE_MSG(shuffled.pending == 0,
                         std::string("seed ") + std::to_string(seed) + ": " +
                             std::to_string(shuffled.pending) + " operations left buffered");
        UAPP_REQUIRE_MSG(shuffled.text == causal.text,
                         std::string("seed ") + std::to_string(seed) + ": non-causal \"" +
                             shuffled.text + "\" vs causal \"" + causal.text + "\"");
        UAPP_REQUIRE_MSG(shuffled.order == causal.order,
                         std::string("seed ") + std::to_string(seed) +
                             ": non-causal delivery placed tombstones differently");
        ++matched;
    }
    std::printf("       buffer: %zu histories delivered in a non-causal shuffle and "
                "still byte-identical to causal delivery\n",
                matched);
}

namespace {

/// The paper's Figure 7, built so that the ID tie-break goes the "wrong" way
/// for plain Fugue, which is the condition under which section 5.3 says Fugue
/// "might not satisfy maximal non-interleaving".
///
///   r1, r2, r3 concurrently insert A, B, C at index 0 (ids order A < B < C).
///   r4 learns {A, B} only, and inserts Y between them   -> "AYB"
///   r5 learns {A, C} only, and inserts X between them   -> "AXC"
///
/// Y and X then share a left origin (A) but have DIFFERENT right origins (B
/// and C). Five replicas rather than the paper's three because replica 2 is
/// the author of B and so can never be in the state "AC"; the extra replicas
/// change nothing about the shape and let the ids be chosen deliberately.
///
/// Maximal non-interleaving admits exactly one order here: AXYBC (paper
/// section 5.2, end). Plain Fugue orders Y before X because (4,0) < (5,0).
/// Built up to the point where r4 and r5 have diverged, but BEFORE the merge,
/// so the caller can assert the two intermediate states the paper names.
Session build_figure7_forked(Variant variant, SpikeStatus& status) {
    Session s(variant, 5);
    const SpikeStatus steps[] = {
        s.local_insert(0, 0, 'A'),  // op 0
        s.local_insert(1, 0, 'B'),  // op 1
        s.local_insert(2, 0, 'C'),  // op 2
        s.deliver(3, 0),            // r4 <- A
        s.deliver(3, 1),            // r4 <- B
        s.local_insert(3, 1, 'Y'),  // op 3, right origin B
        s.deliver(4, 0),            // r5 <- A
        s.deliver(4, 2),            // r5 <- C
        s.local_insert(4, 1, 'X'),  // op 4, right origin C
    };
    status = SpikeStatus::Ok;
    for (const SpikeStatus st : steps) {
        if (!is_ok(st)) {
            status = st;
            break;
        }
    }
    return s;
}

}  // namespace

UAPP_TEST("property.differential: the oracle tells the two variants apart") {
    // A mutation check on the ORACLE. If reference_order() agreed with
    // anything handed to it, the differential check inside every random trial
    // would be worthless. So: give it one element set and both sibling rules,
    // and require that it returns the two DIFFERENT orders the paper says they
    // produce.
    SpikeStatus built = SpikeStatus::Ok;
    Session     fugue_session = build_figure7_forked(Variant::Fugue, built);
    UAPP_REQUIRE_MSG(is_ok(built), spike_status_code(built));
    Session max_session = build_figure7_forked(Variant::FugueMax, built);
    UAPP_REQUIRE_MSG(is_ok(built), spike_status_code(built));

    // The two divergent states the paper's Figure 7 names, before the merge.
    UAPP_REQUIRE_MSG(fugue_session.replica(3).text() == "AYB",
                     fugue_session.replica(3).text());
    UAPP_REQUIRE_MSG(fugue_session.replica(4).text() == "AXC",
                     fugue_session.replica(4).text());

    UAPP_REQUIRE(is_ok(fugue_session.sync_all()));
    UAPP_REQUIRE(is_ok(max_session.sync_all()));

    const ReferenceOrder as_fugue = reference_order(fugue_session.ops(), Variant::Fugue);
    const ReferenceOrder as_max   = reference_order(max_session.ops(), Variant::FugueMax);
    UAPP_REQUIRE_MSG(as_fugue.ok, as_fugue.diagnostic);
    UAPP_REQUIRE_MSG(as_max.ok, as_max.diagnostic);

    std::printf("       Figure 7: implementation Fugue=\"%s\" FugueMax=\"%s\"; "
                "reference Fugue=\"%s\" FugueMax=\"%s\"\n",
                fugue_session.replica(0).text().c_str(), max_session.replica(0).text().c_str(),
                as_fugue.text.c_str(), as_max.text.c_str());

    // The oracle is NOT variant-blind.
    UAPP_CHECK_MSG(as_fugue.text != as_max.text,
                   std::string("oracle gave the same answer for both variants: ") +
                       as_fugue.text);

    // FugueMax must produce the one order maximal non-interleaving allows.
    UAPP_CHECK_MSG(max_session.replica(0).text() == "AXYBC", max_session.replica(0).text());
    UAPP_CHECK_MSG(as_max.text == "AXYBC", as_max.text);

    // Plain Fugue is NOT maximally non-interleaving, and this is the concrete
    // witness. The project's own ADR-0002 claims Fugue provides "maximal
    // non-interleaving"; it does not, FugueMax does. See the findings doc.
    UAPP_CHECK_MSG(fugue_session.replica(0).text() == "AYXBC",
                   fugue_session.replica(0).text());
    UAPP_CHECK_MSG(as_fugue.text == "AYXBC", as_fugue.text);

    // Both orders are nonetheless legal under the strong list specification —
    // which is exactly why a specification oracle alone cannot decide between
    // them, and why P4 needs its own assertions.
    std::string why;
    UAPP_CHECK_MSG(satisfies_strong_list_spec(fugue_session.history().witnesses,
                                              as_fugue.order, why),
                   why);
    UAPP_CHECK_MSG(satisfies_strong_list_spec(max_session.history().witnesses, as_max.order, why),
                   why);

    // And each implementation agrees with its own reference order.
    UAPP_CHECK(fugue_session.replica(0).traversal_ids() == as_fugue.order);
    UAPP_CHECK(max_session.replica(0).traversal_ids() == as_max.order);

    // All five replicas converge in both variants.
    for (std::size_t r = 1; r < 5; ++r) {
        UAPP_CHECK(fugue_session.replica(r).text() == fugue_session.replica(0).text());
        UAPP_CHECK(max_session.replica(r).text() == max_session.replica(0).text());
    }
}
