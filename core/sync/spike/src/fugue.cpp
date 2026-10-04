// SPDX-License-Identifier: Apache-2.0
//
// core/sync/spike/src/fugue.cpp — Algorithm 1 of arXiv:2305.00583, plus the
// FugueMax right-sibling rule of that paper's Definition 6.
//
// SPIKE CODE — NOT PRODUCTION. See include/uapp/sync/spike/element.h.
//
// Paper line numbers are quoted at each step. They refer to Algorithm 1 on
// page 5 of the arXiv v1 PDF.

#include "uapp/sync/spike/fugue.h"

#include <algorithm>
#include <utility>

namespace uapp::sync::spike {

// --- status strings --------------------------------------------------------

const char* spike_status_code(SpikeStatus s) noexcept {
    switch (s) {
        case SpikeStatus::Ok:                   return "SPIKE_OK";
        case SpikeStatus::IndexOutOfRange:      return "SPIKE_INDEX_OUT_OF_RANGE";
        case SpikeStatus::PendingBufferFull:    return "SPIKE_PENDING_BUFFER_FULL";
        case SpikeStatus::ElementLimitExceeded: return "SPIKE_ELEMENT_LIMIT_EXCEEDED";
        case SpikeStatus::OpMalformed:          return "SPIKE_OP_MALFORMED";
        case SpikeStatus::Internal:             return "SPIKE_INTERNAL";
    }
    return "SPIKE_UNKNOWN";
}

const char* spike_status_message(SpikeStatus s) noexcept {
    switch (s) {
        case SpikeStatus::Ok:
            return "ok";
        case SpikeStatus::IndexOutOfRange:
            return "index is outside [0, visible size]";
        case SpikeStatus::PendingBufferFull:
            return "causal-delivery buffer is full; the peer sent operations whose "
                   "dependencies never arrived";
        case SpikeStatus::ElementLimitExceeded:
            return "sequence already holds the maximum number of elements";
        case SpikeStatus::OpMalformed:
            return "operation is structurally impossible";
        case SpikeStatus::Internal:
            return "an invariant of the CRDT implementation was violated";
    }
    return "unknown spike status";
}

// --- construction ----------------------------------------------------------

FugueReplica::FugueReplica(ReplicaId replica_id, Variant variant_in)
    : replica_(replica_id), variant_(variant_in) {
    // Algorithm 1 line 10: the tree starts as {(root, [], [])}.
    Node root;
    root.id     = kNullId;
    root.parent = kNullId;
    nodes_.push_back(std::move(root));
    index_.emplace(element_key(kNullId), 0u);
}

// --- traversal -------------------------------------------------------------

void FugueReplica::traversal(std::vector<std::uint32_t>& out) const {
    out.clear();
    out.reserve(nodes_.size());

    // Algorithm 1 lines 14-20, de-recursed. `cursor` walks one combined range
    // over [left children] ++ [self] ++ [right children], so the frame holds
    // one integer rather than two cursors and a phase flag.
    struct Frame final {
        std::uint32_t node;
        std::size_t   cursor;
    };

    std::vector<Frame> stack;
    stack.push_back(Frame{0u, 0u});

    while (!stack.empty()) {
        const std::uint32_t current = stack.back().node;
        const std::size_t   cursor  = stack.back().cursor;
        const Node&         n       = nodes_[current];
        const std::size_t   nleft   = n.left_children.size();
        const std::size_t   nright  = n.right_children.size();

        if (cursor < nleft) {
            stack.back().cursor = cursor + 1u;
            // push_back may reallocate, which is why `current`/`cursor` were
            // copied out of the frame above rather than held by reference.
            stack.push_back(Frame{n.left_children[cursor], 0u});
            continue;
        }
        if (cursor == nleft) {
            stack.back().cursor = cursor + 1u;
            out.push_back(current);
            continue;
        }
        const std::size_t right_index = cursor - nleft - 1u;
        if (right_index < nright) {
            stack.back().cursor = cursor + 1u;
            stack.push_back(Frame{n.right_children[right_index], 0u});
            continue;
        }
        stack.pop_back();
    }
}

void FugueReplica::position_map(std::vector<std::size_t>& out) const {
    std::vector<std::uint32_t> order;
    traversal(order);
    out.assign(nodes_.size(), kEndPosition);
    for (std::size_t p = 0; p < order.size(); ++p) {
        out[order[p]] = p;
    }
}

std::string FugueReplica::text() const {
    std::vector<std::uint32_t> order;
    traversal(order);
    std::string result;
    result.reserve(order.size());
    for (const std::uint32_t idx : order) {
        if (idx == 0u) { continue; }  // the root carries no value
        const Node& n = nodes_[idx];
        if (!n.deleted) { result.push_back(n.value); }
    }
    return result;
}

std::vector<ElementId> FugueReplica::traversal_ids() const {
    std::vector<std::uint32_t> order;
    traversal(order);
    std::vector<ElementId> ids;
    ids.reserve(order.size());
    for (const std::uint32_t idx : order) {
        if (idx == 0u) { continue; }
        ids.push_back(nodes_[idx].id);
    }
    return ids;
}

std::vector<ElementId> FugueReplica::visible_ids() const {
    std::vector<std::uint32_t> order;
    traversal(order);
    std::vector<ElementId> ids;
    ids.reserve(order.size());
    for (const std::uint32_t idx : order) {
        if (idx == 0u) { continue; }
        if (!nodes_[idx].deleted) { ids.push_back(nodes_[idx].id); }
    }
    return ids;
}

std::size_t FugueReplica::visible_size() const {
    std::size_t visible = 0;
    for (std::size_t i = 1; i < nodes_.size(); ++i) {
        if (!nodes_[i].deleted) { ++visible; }
    }
    return visible;
}

std::size_t FugueReplica::tree_depth() const {
    // A top-down walk, NOT a pass over traversal() order: in an in-order
    // traversal a left child is visited BEFORE its parent, so deriving a
    // child's depth from its parent's while iterating the traversal would read
    // an unset value. Iterative for the same reason traversal() is — the tree
    // is a spine for sequentially typed text, so depth is O(element count).
    struct Frame final {
        std::uint32_t node;
        std::size_t   depth;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{0u, 0u});

    std::size_t deepest = 0;
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, f.depth);
        const Node& n = nodes_[f.node];
        for (const std::uint32_t child : n.left_children) {
            stack.push_back(Frame{child, f.depth + 1u});
        }
        for (const std::uint32_t child : n.right_children) {
            stack.push_back(Frame{child, f.depth + 1u});
        }
    }
    return deepest;
}

bool FugueReplica::resolve(ElementId id, std::uint32_t& index) const {
    const auto it = index_.find(element_key(id));
    if (it == index_.end()) { return false; }
    index = it->second;
    return true;
}

// --- local edits -----------------------------------------------------------

SpikeStatus FugueReplica::insert(std::size_t index, char value, Operation& out) {
    if (element_count() >= kMaxElements) { return SpikeStatus::ElementLimitExceeded; }

    std::vector<std::uint32_t> order;
    traversal(order);

    // Algorithm 1 line 23: leftOrigin = node for the (i-1)-th VISIBLE value,
    // or the root when i == 0. `left_pos` is its position in the
    // tombstone-inclusive traversal, which is what line 24 needs.
    std::size_t left_pos = 0;
    if (index > 0) {
        std::size_t seen  = 0;
        bool        found = false;
        for (std::size_t p = 1; p < order.size(); ++p) {
            if (nodes_[order[p]].deleted) { continue; }
            ++seen;
            if (seen == index) {
                left_pos = p;
                found    = true;
                break;
            }
        }
        if (!found) { return SpikeStatus::IndexOutOfRange; }
    }

    // Algorithm 1 line 24: rightOrigin = the next node after leftOrigin in the
    // traversal THAT INCLUDES TOMBSTONES. Using the visible successor instead
    // is a subtle wrong turn: it would make the new node a left child of a node
    // that has a tombstone before it, placing the insertion on the wrong side
    // of that tombstone and so on the wrong side of any concurrent insert that
    // attached to it.
    const std::uint32_t left_index = order[left_pos];
    const ElementId right_origin =
        (left_pos + 1u < order.size()) ? nodes_[order[left_pos + 1u]].id : kNullId;

    Operation op;
    op.kind         = OpKind::Insert;
    op.id           = ElementId{replica_, counter_};
    op.value        = value;
    op.right_origin = right_origin;

    if (nodes_[left_index].right_children.empty()) {
        // Algorithm 1 lines 25-26 (paper Figure 4(a)): leftOrigin has no right
        // children, so a right child of leftOrigin lands immediately after it.
        op.parent = nodes_[left_index].id;
        op.side   = Side::Right;
    } else {
        // Algorithm 1 lines 27-28 (paper Figure 4(b)). Theorem 1's proof shows
        // rightOrigin is then a DESCENDANT of leftOrigin and has no left
        // children, so it cannot be `end` and a left child of it lands
        // immediately before it.
        if (is_null(right_origin)) { return SpikeStatus::Internal; }
        op.parent = right_origin;
        op.side   = Side::Left;
    }

    const SpikeStatus st = integrate(op);
    if (!is_ok(st)) { return st; }

    ++counter_;  // Algorithm 1 line 22, deferred until the op actually took
    out = op;
    return SpikeStatus::Ok;
}

SpikeStatus FugueReplica::erase(std::size_t index, Operation& out) {
    // Algorithm 1 lines 39-41.
    std::vector<std::uint32_t> order;
    traversal(order);

    std::size_t seen = 0;
    for (std::size_t p = 1; p < order.size(); ++p) {
        const Node& n = nodes_[order[p]];
        if (n.deleted) { continue; }
        if (seen == index) {
            Operation op;
            op.kind = OpKind::Delete;
            op.id   = n.id;

            const SpikeStatus st = integrate(op);
            if (!is_ok(st)) { return st; }
            out = op;
            return SpikeStatus::Ok;
        }
        ++seen;
    }
    return SpikeStatus::IndexOutOfRange;
}

// --- remote edits ----------------------------------------------------------

bool FugueReplica::malformed(const Operation& op) {
    if (is_null(op.id)) { return true; }
    if (op.kind == OpKind::Delete) { return false; }
    if (op.parent == op.id) { return true; }
    // The root is the first node of the traversal, so nothing can be ordered
    // before it: a Left child of the root is not a position this algorithm can
    // ever produce, and accepting one would put an element before the root in
    // one replica's traversal and not another's.
    if (is_null(op.parent) && op.side == Side::Left) { return true; }
    return false;
}

bool FugueReplica::already_applied(const Operation& op) const {
    std::uint32_t index = 0u;
    if (!resolve(op.id, index)) { return false; }
    if (op.kind == OpKind::Insert) { return true; }
    return nodes_[index].deleted;
}

bool FugueReplica::dependencies_present(const Operation& op) const {
    std::uint32_t index = 0u;
    if (op.kind == OpKind::Delete) { return resolve(op.id, index); }
    if (!resolve(op.parent, index)) { return false; }
    // FugueMax orders right siblings by comparing their right origins'
    // POSITIONS, so a right child cannot be integrated until its right origin
    // is in the tree. Under plain Fugue the field is unused, and requiring it
    // would impose a dependency the paper's algorithm does not have.
    if (variant_ == Variant::FugueMax && op.side == Side::Right &&
        !is_null(op.right_origin)) {
        if (!resolve(op.right_origin, index)) { return false; }
    }
    return true;
}

SpikeStatus FugueReplica::apply(const Operation& op) {
    if (malformed(op)) { return SpikeStatus::OpMalformed; }

    // Property P2, idempotence: a duplicate delivery is a no-op, not an error.
    if (already_applied(op)) { return SpikeStatus::Ok; }

    if (!dependencies_present(op)) {
        for (const Operation& held : pending_) {
            if (same_operation(held, op)) { return SpikeStatus::Ok; }
        }
        if (pending_.size() >= kMaxPendingOperations) {
            return SpikeStatus::PendingBufferFull;
        }
        pending_.push_back(op);
        return SpikeStatus::Ok;
    }

    const SpikeStatus st = integrate(op);
    if (!is_ok(st)) { return st; }
    drain_pending();
    return SpikeStatus::Ok;
}

void FugueReplica::drain_pending() {
    // One newly-integrated element can unblock a chain of held operations, so
    // this repeats until a full sweep makes no progress. O(pending^2) worst
    // case, which the bound on `pending_` keeps finite.
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < pending_.size();) {
            const Operation held = pending_[i];
            if (already_applied(held)) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                progress = true;
                continue;
            }
            if (!dependencies_present(held)) {
                ++i;
                continue;
            }
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            if (!is_ok(integrate(held))) {
                // Integration of an operation whose dependencies are present
                // can only fail on the element limit. Dropping it would be
                // silent data loss, so put it back and stop: the caller sees a
                // non-zero pending_count() and an element count at the limit.
                pending_.push_back(held);
                return;
            }
            progress = true;
        }
    }
}

std::size_t FugueReplica::sibling_insert_index(const std::vector<std::uint32_t>& sibs,
                                               const Operation&                  op) const {
    // Algorithm 1 lines 33-37: "the least index such that node.id < sibs[i]",
    // i.e. keep the list sorted ascending by id. Left children always use this
    // rule; right children use it too under plain Fugue.
    const bool fugue_max_right = (variant_ == Variant::FugueMax && op.side == Side::Right);

    if (!fugue_max_right) {
        for (std::size_t i = 0; i < sibs.size(); ++i) {
            if (op.id < nodes_[sibs[i]].id) { return i; }
        }
        return sibs.size();
    }

    // FugueMax, Definition 6: right-side siblings are visited in the REVERSE
    // order of their right origins, ties broken by id. So this node goes
    // before sibs[i] when its right origin is LATER in the list than sibs[i]'s.
    //
    // Why this is well defined: a right origin is always causally prior to the
    // node that names it, and the relative order of two elements that already
    // exist never changes in an algorithm satisfying the strong list
    // specification. The comparison is therefore stable as more elements
    // arrive. It costs a full traversal per delivered right child, which is
    // acceptable for a spike and is exactly the kind of thing production would
    // have to index — noted in the findings doc.
    std::vector<std::size_t> pos;
    position_map(pos);

    std::uint32_t resolved = 0u;
    const std::size_t own = is_null(op.right_origin)
                                ? kEndPosition
                                : (resolve(op.right_origin, resolved) ? pos[resolved]
                                                                     : kEndPosition);

    for (std::size_t i = 0; i < sibs.size(); ++i) {
        const Node&       sib = nodes_[sibs[i]];
        const std::size_t theirs =
            is_null(sib.right_origin)
                ? kEndPosition
                : (resolve(sib.right_origin, resolved) ? pos[resolved] : kEndPosition);

        if (own > theirs) { return i; }
        if (own == theirs && op.id < sib.id) { return i; }
    }
    return sibs.size();
}

SpikeStatus FugueReplica::integrate(const Operation& op) {
    if (op.kind == OpKind::Delete) {
        // Algorithm 1 lines 42-44: tombstone, never removal. The node may be
        // the parent of elements that have not arrived yet.
        std::uint32_t index = 0u;
        if (!resolve(op.id, index)) { return SpikeStatus::Internal; }
        nodes_[index].deleted = true;
        return SpikeStatus::Ok;
    }

    if (element_count() >= kMaxElements) { return SpikeStatus::ElementLimitExceeded; }

    std::uint32_t parent_index = 0u;
    if (!resolve(op.parent, parent_index)) { return SpikeStatus::Internal; }

    // Algorithm 1 line 38: add the node to the tree...
    Node node;
    node.id           = op.id;
    node.parent       = op.parent;
    node.right_origin = op.right_origin;
    node.value        = op.value;
    node.side         = op.side;

    const auto new_index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(std::move(node));
    index_.emplace(element_key(op.id), new_index);

    // ...and into the correct one of its parent's two ordered child lists
    // (lines 31-37). Taken AFTER push_back so that sibling_insert_index can
    // see a consistent node table, and by index because push_back may have
    // reallocated.
    std::vector<std::uint32_t>& sibs = (op.side == Side::Right)
                                           ? nodes_[parent_index].right_children
                                           : nodes_[parent_index].left_children;
    const std::size_t at = sibling_insert_index(sibs, op);
    sibs.insert(sibs.begin() + static_cast<std::ptrdiff_t>(at), new_index);

    return SpikeStatus::Ok;
}

}  // namespace uapp::sync::spike
