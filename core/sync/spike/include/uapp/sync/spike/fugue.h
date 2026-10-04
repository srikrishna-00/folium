// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/fugue.h — Fugue / FugueMax sequence CRDT, one replica.
//
// SPIKE CODE — NOT PRODUCTION. See element.h for the scope statement.
//
// ===========================================================================
// WHAT THIS IMPLEMENTS, AND WHERE IT COMES FROM
// ===========================================================================
// M. Weidner and M. Kleppmann, "The Art of the Fugue: Minimizing Interleaving
// in Collaborative Text Editing", arXiv:2305.00583 (2023).
//
//   * `Variant::Fugue`    = Algorithm 1 (paper page 5), implemented line for
//                           line. The line numbers are quoted in fugue.cpp at
//                           each step so a reviewer can diff code against
//                           paper rather than against intent.
//   * `Variant::FugueMax` = Algorithm 1 plus Definition 6 (section 5.3): right
//                           -side siblings are visited in the REVERSE order of
//                           their right origins, ties broken by id.
//
// The algorithm in one paragraph. The state is a tree. Every element is a node
// with a parent and a side, Left or Right; a parent may have MANY left or many
// right children (the tree is n-ary, not binary, and is never balanced). The
// list order is the depth-first IN-ORDER traversal: left children, then the
// node itself, then right children. To insert between the element at index i-1
// (`leftOrigin`) and the next element in the tombstone-inclusive traversal
// (`rightOrigin`): if leftOrigin has no right children, the new node is a
// right child of leftOrigin; otherwise it is a left child of rightOrigin.
// Deletes are tombstones — a deleted node keeps its place in the tree because
// concurrent inserts may still name it as a parent.
//
// That second clause — fall back to the RIGHT origin — is the entire
// difference from classic RGA, and the entire reason this project switched
// algorithms. RGA records only a left origin, so a word typed right-to-left
// produces N elements that all share one origin and therefore sort against a
// concurrent insertion character by character. See rga.h, which implements the
// defect on purpose so the tests can show the contrast rather than assert it.
//
// ===========================================================================
// THREE PLACES THIS DEPARTS FROM THE PAPER'S PSEUDOCODE, ON PURPOSE
// ===========================================================================
//  1. TRAVERSAL IS ITERATIVE, NOT RECURSIVE. Algorithm 1 lines 14-20 are
//     written as recursion. A direct transcription stack-overflows on real
//     documents: typing n characters left to right builds a right-spine of
//     depth n, so `traverse()` would recurse once per character. A 100 000-
//     character note is not an adversarial input, it is a long note. The
//     traversal here carries its own explicit heap stack. This is a finding,
//     not a style preference — see the findings doc.
//  2. BOUNDED CAUSAL-DELIVERY BUFFER. The paper assumes a causal broadcast
//     layer. docs/sync-architecture.md section 6.2 requires that the CRDT
//     buffer rather than assume, and that the buffer be bounded. `apply()`
//     therefore holds operations whose dependencies have not arrived and
//     returns SpikeStatus::PendingBufferFull at kMaxPendingOperations instead
//     of growing without limit.
//  3. EXPLICIT LIMITS. kMaxElements and the malformed-operation checks exist
//     because every operation here is, in production, attacker-influenced data
//     arriving from another device (spec section 38).
//
// NOT IMPLEMENTED, deliberately, and NOT faked: tombstone garbage collection
// (needs the causal-stability + 90-day-horizon design in sync-architecture.md
// section 6.2), run-length/waypoint compression, persistence, encryption.

#ifndef UAPP_CORE_SYNC_SPIKE_FUGUE_H
#define UAPP_CORE_SYNC_SPIKE_FUGUE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "uapp/sync/spike/element.h"
#include "uapp/sync/spike/spike_status.h"

namespace uapp::sync::spike {

/// One replica of the sequence. Values are `char` because the property under
/// test is about ORDER, and single characters are what the interleaving
/// literature's examples are written in. Nothing in the algorithm depends on
/// the value type.
///
/// Copyable on purpose: the test harness forks a replica to create a
/// "same history so far, now diverge" scenario, which is how concurrency is
/// constructed without a network.
class FugueReplica final {
public:
    /// `replica_id` must be non-zero (0 is reserved for the null/root id) and
    /// must be unique across the replicas that will ever merge. A duplicate
    /// replica id is an id collision, i.e. silent corruption; the spike
    /// asserts uniqueness in the harness rather than defending against it
    /// here, because production will mint device ids centrally.
    FugueReplica(ReplicaId replica_id, Variant variant);

    // --- local edits -------------------------------------------------------
    // Both produce the operation to broadcast AND apply it locally, matching
    // the paper's assumption that "the causal broadcast protocol immediately
    // delivers the message to the sender".

    /// Insert `value` at visible index `index`, which may equal the current
    /// visible size (append). Algorithm 1 lines 21-29.
    [[nodiscard]] SpikeStatus insert(std::size_t index, char value, Operation& out);

    /// Delete the element at visible index `index`. Algorithm 1 lines 39-41.
    [[nodiscard]] SpikeStatus erase(std::size_t index, Operation& out);

    // --- remote edits ------------------------------------------------------

    /// Apply a broadcast operation. Algorithm 1 lines 30-38 and 42-44.
    ///
    /// IDEMPOTENT: an operation that has already taken effect is a no-op and
    /// returns Ok, because duplicate delivery is normal on a retrying mobile
    /// client (docs/sync-architecture.md section 6.2, property P2).
    ///
    /// TOLERANT OF NON-CAUSAL ORDER: an operation whose dependencies are
    /// missing is buffered and retried after every subsequent apply, so a
    /// caller may deliver a whole log in any permutation and still converge.
    /// That is deliberately STRONGER than the paper needs; it is here because
    /// the test harness uses it to prove the property set holds under random
    /// delivery, and because production will face reordering at the transport.
    [[nodiscard]] SpikeStatus apply(const Operation& op);

    // --- observation -------------------------------------------------------

    /// The visible document: Algorithm 1 lines 12-13, `values()`.
    [[nodiscard]] std::string text() const;

    /// Ids of every element in tombstone-inclusive traversal order, root
    /// excluded.
    ///
    /// Convergence is asserted on THIS, not only on `text()`. Two replicas can
    /// agree on the visible text while disagreeing about where a tombstone
    /// sits, and that disagreement becomes visible text the moment a
    /// concurrent insert lands next to the tombstone. Comparing only `text()`
    /// is how a convergence test passes on a replica that is already wrong.
    [[nodiscard]] std::vector<ElementId> traversal_ids() const;

    /// Ids of the VISIBLE elements, in order. This is `text()` with identities
    /// instead of characters, and it is what the strong-list-specification
    /// oracle needs: the constraint an insert imposes is stated over the
    /// elements the generating replica could see at that moment, so the
    /// harness must be able to capture that list through the public surface.
    [[nodiscard]] std::vector<ElementId> visible_ids() const;

    [[nodiscard]] std::size_t visible_size() const;

    /// Elements held in the tree, tombstones included, root excluded.
    [[nodiscard]] std::size_t element_count() const noexcept {
        return nodes_.empty() ? 0u : nodes_.size() - 1u;
    }

    /// Operations still waiting for their causal dependencies. A converged
    /// replica that has received a complete log must report 0; a non-zero
    /// count after a full delivery means the log referenced something that was
    /// never sent, which is a harness bug or a corrupt log.
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

    /// Deepest path from the root, counting a top-level element as depth 1.
    /// Exposed because the degenerate-depth behaviour in the header comment is
    /// a measured claim in the findings doc, not an assumption.
    [[nodiscard]] std::size_t tree_depth() const;

    [[nodiscard]] ReplicaId replica_id() const noexcept { return replica_; }
    [[nodiscard]] Variant   variant() const noexcept { return variant_; }

private:
    /// Algorithm 1 line 10: a tree node plus its two ordered child lists.
    /// Children are stored as indices into `nodes_` rather than ids, so a
    /// traversal step costs no hash lookup.
    struct Node final {
        ElementId                  id{};
        ElementId                  parent{};
        ElementId                  right_origin{};
        std::vector<std::uint32_t> left_children;
        std::vector<std::uint32_t> right_children;
        char                       value   = '\0';
        Side                       side    = Side::Right;
        bool                       deleted = false;
    };

    /// Sentinel used by the FugueMax sibling order for a right origin of
    /// `end`: `end` is after every element, so it sorts first under reversal.
    static constexpr std::size_t kEndPosition = static_cast<std::size_t>(-1);

    [[nodiscard]] const Node& node_at(std::uint32_t index) const noexcept {
        return nodes_[index];
    }

    /// Tombstone-inclusive in-order traversal, root first at position 0.
    /// Iterative: see departure (1) in the header comment.
    void traversal(std::vector<std::uint32_t>& out) const;

    /// `out[node_index]` = position of that node in `traversal()`.
    void position_map(std::vector<std::size_t>& out) const;

    [[nodiscard]] bool resolve(ElementId id, std::uint32_t& index) const;

    [[nodiscard]] bool already_applied(const Operation& op) const;
    [[nodiscard]] bool dependencies_present(const Operation& op) const;
    [[nodiscard]] static bool malformed(const Operation& op);

    [[nodiscard]] SpikeStatus integrate(const Operation& op);
    void                      drain_pending();

    /// Index at which `op` goes into `sibs`, per Algorithm 1 lines 33-37 for
    /// Fugue, or Definition 6 for FugueMax's right-side case.
    [[nodiscard]] std::size_t sibling_insert_index(const std::vector<std::uint32_t>& sibs,
                                                   const Operation&                  op) const;

    ReplicaId     replica_;
    Variant       variant_;
    std::uint32_t counter_ = 0;  ///< Algorithm 1 line 11

    /// nodes_[0] is the root (Algorithm 1 line 10: `root = (null, bottom,
    /// null, null)`). It is never emitted and never has left children,
    /// because the root is the first node of the traversal and so is never any
    /// element's right origin.
    std::vector<Node>                              nodes_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
    std::vector<Operation>                         pending_;
};

}  // namespace uapp::sync::spike

#endif  // UAPP_CORE_SYNC_SPIKE_FUGUE_H
