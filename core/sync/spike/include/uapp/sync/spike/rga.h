// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/rga.h — classic RGA. A DELIBERATELY DEFECTIVE CONTROL.
//
// SPIKE CODE — NOT PRODUCTION, AND NOT A CANDIDATE. See element.h.
//
// ===========================================================================
// WHY A SECOND, WRONG ALGORITHM IS IN THE TREE
// ===========================================================================
// SP-6 F-1 is the whole reason this project switched from RGA to Fugue: RGA
// exhibits BACKWARD interleaving, and convergence/idempotence/commutativity
// tests all pass on the interleaved result. The obvious way to write the
// non-interleaving test is to assert "Fugue does not interleave" — but that
// assertion passes just as happily if the detector is broken, if the scenario
// does not actually trigger the defect, or if the merge never happened.
//
// So this file implements classic RGA, the algorithm that is KNOWN to fail,
// and the test suite asserts that the detector FIRES on it. That turns the
// non-interleaving test from a claim into a measurement: the scenario is
// proven to be an adversarial one, and the detector is proven able to fail.
// Without this control, the P4 test is unfalsifiable, which is the precise
// criticism SP-6 made of the ADR's original three-property plan.
//
// Reference: H.-G. Roh et al., "Replicated abstract data types: Building
// blocks for collaborative applications" (JPDC 2011). As modelled here:
//
//   * every element records a LEFT ORIGIN only — the element it was inserted
//     after — and nothing about what was to its right;
//   * the list order is the depth-first PRE-ORDER traversal of that
//     left-origin tree, with same-parent children visited in DESCENDING
//     timestamp order;
//   * timestamps are Lamport (counter, replica) pairs compared
//     lexicographically.
//
// This model is calibrated against the Fugue paper's Appendix A.1.8, which
// gives the minimal RGA backward-interleaving example and its expected output
// ("axb"); the test suite asserts that exact string, so the control is checked
// against the literature rather than against our own expectations.

#ifndef UAPP_CORE_SYNC_SPIKE_RGA_H
#define UAPP_CORE_SYNC_SPIKE_RGA_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "uapp/sync/spike/element.h"
#include "uapp/sync/spike/spike_status.h"

namespace uapp::sync::spike {

/// An RGA operation. Its shape IS the finding: there is no right-origin field,
/// which is exactly the information Fugue adds and RGA lacks.
struct RgaOperation final {
    OpKind    kind = OpKind::Insert;
    /// (counter, replica) compared counter-major — a Lamport timestamp, not
    /// the (replica, counter) id order Fugue uses for its sibling tie-break.
    ElementId id{};
    /// The element this one was inserted after; null means "at the head".
    ElementId origin{};
    char      value = '\0';
};

/// Lamport order: counter first, then replica id. Matches the s4vector
/// ordering the Fugue paper's Appendix A.1.8 assumes.
[[nodiscard]] inline constexpr bool rga_timestamp_less(ElementId a, ElementId b) noexcept {
    return a.counter != b.counter ? a.counter < b.counter : a.replica < b.replica;
}

class RgaReplica final {
public:
    explicit RgaReplica(ReplicaId replica_id);

    [[nodiscard]] SpikeStatus insert(std::size_t index, char value, RgaOperation& out);
    [[nodiscard]] SpikeStatus erase(std::size_t index, RgaOperation& out);
    [[nodiscard]] SpikeStatus apply(const RgaOperation& op);

    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::size_t visible_size() const;
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Node final {
        ElementId                  id{};
        ElementId                  origin{};
        std::vector<std::uint32_t> children;  ///< descending timestamp order
        char                       value   = '\0';
        bool                       deleted = false;
    };

    void                      traversal(std::vector<std::uint32_t>& out) const;
    [[nodiscard]] bool        resolve(ElementId id, std::uint32_t& index) const;
    [[nodiscard]] SpikeStatus integrate(const RgaOperation& op);
    void                      drain_pending();

    ReplicaId     replica_;
    std::uint32_t clock_ = 0;  ///< Lamport clock: max seen, incremented on send

    std::vector<Node>                                nodes_;  ///< nodes_[0] is the head sentinel
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
    std::vector<RgaOperation>                        pending_;
};

}  // namespace uapp::sync::spike

#endif  // UAPP_CORE_SYNC_SPIKE_RGA_H
